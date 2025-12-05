#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define CPUFREQ_PATH "/sys/devices/system/cpu"
#define SOCKET_PATH "/tmp/cpu_throttle.sock"
#define PID_FILE "/var/run/cpu_throttle.pid"
#define CONFIG_FILE "/etc/cpu_throttle.conf"
#define DEFAULT_WEB_PORT 8086  // Intel 8086 tribute!
#define DAEMON_VERSION "2.0"

// Logging levels
#define LOGLEVEL_SILENT 0
#define LOGLEVEL_QUIET 1
#define LOGLEVEL_NORMAL 2
#define LOGLEVEL_VERBOSE 3

char temp_path[512] = "/sys/class/thermal/thermal_zone0/temp";
int dry_run = 0;
FILE *logfile = NULL;
int safe_min = 0; // optional safe minimum frequency in kHz
int safe_max = 0; // optional safe maximum frequency in kHz
int temp_max = 95; // maximum temperature threshold in °C (default 95)
int socket_fd = -1; // unix socket file descriptor
int http_fd = -1; // HTTP socket file descriptor
int web_port = DEFAULT_WEB_PORT; // HTTP port (DEFAULT_WEB_PORT = 8086)
int log_level = LOGLEVEL_NORMAL; // default logging level
volatile sig_atomic_t should_exit = 0; // flag for graceful shutdown

// Current state for API responses
int current_temp = 0;
int current_freq = 0;
int cpu_min_freq = 0;
int cpu_max_freq = 0;

// Logging macros
#define LOG_ERROR(fmt, ...) do { if (log_level >= LOGLEVEL_QUIET) fprintf(stderr, fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...) do { if (log_level >= LOGLEVEL_NORMAL) printf(fmt, ##__VA_ARGS__); } while(0)
#define LOG_VERBOSE(fmt, ...) do { if (log_level >= LOGLEVEL_VERBOSE) printf(fmt, ##__VA_ARGS__); } while(0)

void signal_handler(int sig) {
    should_exit = 1;
}

void cleanup_socket() {
    if (socket_fd >= 0) {
        close(socket_fd);
        unlink(SOCKET_PATH);
    }
    if (http_fd >= 0) {
        close(http_fd);
    }
    unlink(PID_FILE);
}

int write_pid_file() {
    FILE *fp = fopen(PID_FILE, "w");
    if (!fp) {
        LOG_ERROR("Warning: Cannot create PID file %s\n", PID_FILE);
        return -1;
    }
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

void load_config_file() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        LOG_VERBOSE("No config file found at %s, using defaults\n", CONFIG_FILE);
        return;
    }
    
    char line[256];
    int line_num = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        // Skip comments and empty lines
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        
        // Parse key=value
        char key[64], value[64];
        if (sscanf(p, "%63[^=]=%63s", key, value) == 2) {
            // Trim whitespace from key
            char *end = key + strlen(key) - 1;
            while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';
            
            if (strcmp(key, "temp_max") == 0) {
                temp_max = atoi(value);
                LOG_VERBOSE("Config: temp_max = %d\n", temp_max);
            } else if (strcmp(key, "safe_min") == 0) {
                safe_min = atoi(value);
                LOG_VERBOSE("Config: safe_min = %d\n", safe_min);
            } else if (strcmp(key, "safe_max") == 0) {
                safe_max = atoi(value);
                LOG_VERBOSE("Config: safe_max = %d\n", safe_max);
            } else if (strcmp(key, "sensor") == 0) {
                strncpy(temp_path, value, sizeof(temp_path) - 1);
                LOG_VERBOSE("Config: sensor = %s\n", temp_path);
            } else if (strcmp(key, "web_port") == 0) {
                web_port = atoi(value);
                LOG_VERBOSE("Config: web_port = %d\n", web_port);
            } else {
                LOG_VERBOSE("Config: Unknown key '%s' at line %d\n", key, line_num);
            }
        }
    }
    fclose(fp);
}

int read_temp() {
    FILE *fp = fopen(temp_path, "r");
    if (!fp) return -1;
    int temp_raw;
    fscanf(fp, "%d", &temp_raw);
    fclose(fp);
    return temp_raw / 1000;
}

int read_freq_value(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val;
    fscanf(fp, "%d", &val);
    fclose(fp);
    return val;
}

void set_max_freq_all_cpus(int freq) {
    DIR *dir = opendir(CPUFREQ_PATH);
    struct dirent *entry;
    if (!dir) return;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            char path[512];
            snprintf(path, sizeof(path),
                     "%s/%s/cpufreq/scaling_max_freq", CPUFREQ_PATH, entry->d_name);
            if (!dry_run) {
                FILE *fp = fopen(path, "w");
                if (fp) {
                    fprintf(fp, "%d", freq);
                    fclose(fp);
                }
            }
        }
    }
    closedir(dir);
}

int clamp(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

int setup_socket() {
    struct sockaddr_un addr;
    
    // Remove old socket if exists
    unlink(SOCKET_PATH);
    
    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }
    
    // Set non-blocking
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(socket_fd);
        return -1;
    }
    
    // Set socket permissions so non-root users can connect
    chmod(SOCKET_PATH, 0666);
    
    if (listen(socket_fd, 5) < 0) {
        perror("listen");
        close(socket_fd);
        return -1;
    }
    
    return 0;
}

int setup_http_server() {
    struct sockaddr_in addr;
    
    if (web_port == 0) {
        return 0; // HTTP disabled
    }
    
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (http_fd < 0) {
        perror("HTTP socket");
        return -1;
    }
    
    // Allow port reuse
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set non-blocking
    int flags = fcntl(http_fd, F_GETFL, 0);
    fcntl(http_fd, F_SETFL, flags | O_NONBLOCK);
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(web_port);
    
    if (bind(http_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind HTTP port %d: %s\n", web_port, strerror(errno));
        close(http_fd);
        http_fd = -1;
        return -1;
    }
    
    if (listen(http_fd, 10) < 0) {
        perror("HTTP listen");
        close(http_fd);
        http_fd = -1;
        return -1;
    }
    
    LOG_INFO("✅ Web interface available at http://localhost:%d/\n", web_port);
    return 0;
}

// HTTP Response helper
void send_http_response(int client_fd, const char *status, const char *content_type, const char *body) {
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, content_type, strlen(body));
    
    write(client_fd, header, strlen(header));
    write(client_fd, body, strlen(body));
}

// JSON helper - build status response
void build_status_json(char *buffer, size_t size) {
    snprintf(buffer, size,
             "{"
             "\"temperature\":%d,"
             "\"frequency\":%d,"
             "\"safe_min\":%d,"
             "\"safe_max\":%d,"
             "\"temp_max\":%d,"
             "\"sensor\":\"%s\""
             "}",
             current_temp, current_freq, safe_min, safe_max, temp_max, temp_path);
}

// JSON helper - build limits response
void build_limits_json(char *buffer, size_t size) {
    snprintf(buffer, size,
             "{"
             "\"cpu_min_freq\":%d,"
             "\"cpu_max_freq\":%d,"
             "\"temp_sensor\":\"%s\""
             "}",
             cpu_min_freq, cpu_max_freq, temp_path);
}

// Profile helpers
const char* get_profile_dir() {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    snprintf(path, sizeof(path), "%s/.config/cpu_throttle/profiles", home);
    return path;
}

int ensure_profile_dir() {
    const char *dir = get_profile_dir();
    struct stat st;
    if (stat(dir, &st) == 0) return 0;
    // create recursively
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(dir, 0755);
}

// List profiles as JSON array
void build_profiles_list_json(char *buffer, size_t size) {
    char json[4096];
    json[0] = '\0';
    int first = 1;
    const char *dir = get_profile_dir();
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(buffer, size, "[]");
        return;
    }
    struct dirent *ent;
    strcat(json, "[");
    while ((ent = readdir(d)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            if (!first) strcat(json, ",");
            first = 0;
            char name[256];
            snprintf(name, sizeof(name), "\"%s\"", ent->d_name);
            strcat(json, name);
        }
    }
    strcat(json, "]");
    closedir(d);
    snprintf(buffer, size, "%s", json);
}

// Read profile file into buffer
int read_profile_file(const char *name, char *out, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    size_t r = fread(out, 1, size - 1, fp);
    out[r] = '\0';
    fclose(fp);
    return 0;
}

// Write profile file from key=value format (body)
int write_profile_file(const char *name, const char *body) {
    if (ensure_profile_dir() != 0) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fwrite(body, 1, strlen(body), fp);
    fclose(fp);
    return 0;
}

// Delete profile
int delete_profile_file(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
    return unlink(path);
}

// Embedded HTML dashboard
const char* html_dashboard = 
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>CPU Throttle Monitor</title>"
"<style>"
"body{font-family:sans-serif;max-width:800px;margin:20px auto;padding:20px;background:#1a1a1a;color:#fff}"
"h1{color:#FF8C00}h2{border-bottom:2px solid #FF8C00;padding-bottom:10px}"
".card{background:#2a2a2a;padding:20px;margin:10px 0;border-radius:8px}"
".value{font-size:2em;font-weight:bold;color:#FF8C00}"
".label{color:#999;font-size:0.9em}"
"button{background:#FF8C00;color:#fff;border:none;padding:10px 20px;margin:5px;border-radius:4px;cursor:pointer}"
"button:hover{background:#e67e22}"
"input{padding:8px;margin:5px;border:1px solid #555;background:#333;color:#fff;border-radius:4px}"
"</style>"
"</head><body>"
"<h1>CPU Throttle Monitor</h1>"
"<div class='card'>"
"<h2>Current Status</h2>"
"<div class='label'>Temperature</div>"
"<div class='value' id='temp'>--</div>"
"<div class='label'>Frequency</div>"
"<div class='value' id='freq'>--</div>"
"</div>"
"<div class='card'>"
"<h2>Settings</h2>"
"<div><input id='safe_max' type='number' placeholder='Safe Max (kHz)'>"
"<button onclick='setSetting(\"safe-max\",document.getElementById(\"safe_max\").value)'>Set Max</button></div>"
"<div><input id='safe_min' type='number' placeholder='Safe Min (kHz)'>"
"<button onclick='setSetting(\"safe-min\",document.getElementById(\"safe_min\").value)'>Set Min</button></div>"
"<div><input id='temp_max' type='number' placeholder='Temp Max (C)'>"
"<button onclick='setSetting(\"temp-max\",document.getElementById(\"temp_max\").value)'>Set Temp</button></div>"
"</div>"
"<script>"
"function update(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('temp').textContent=d.temperature+'°C';"
"document.getElementById('freq').textContent=(d.frequency/1000).toFixed(0)+' MHz';"
"})}"
"function setSetting(name,val){"
"fetch('/api/settings/'+name,{method:'POST',body:JSON.stringify({value:parseInt(val)}),headers:{'Content-Type':'application/json'}})"
".then(()=>update())}"
"setInterval(update,1000);update();"
"</script>"
"</body></html>";

// Parse HTTP request and route to handlers
void handle_http_request(int client_fd, const char *request) {
    char method[16], path[256];
    sscanf(request, "%s %s", method, path);
    
    LOG_VERBOSE("HTTP %s %s\n", method, path);
    
    char response[2048];
    
    // Route API requests
    if (strcmp(path, "/") == 0) {
        send_http_response(client_fd, "200 OK", "text/html", html_dashboard);
    }
    else if (strcmp(path, "/api/status") == 0 && strcmp(method, "GET") == 0) {
        build_status_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/limits") == 0 && strcmp(method, "GET") == 0) {
        build_limits_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/daemon/version") == 0 && strcmp(method, "GET") == 0) {
        snprintf(response, sizeof(response), "{\"version\":\"%s\"}", DAEMON_VERSION);
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/daemon/shutdown") == 0 && strcmp(method, "POST") == 0) {
        should_exit = 1;
        snprintf(response, sizeof(response), "{\"status\":\"shutting down\"}");
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strncmp(path, "/api/profiles", 13) == 0) {
        // /api/profiles or /api/profiles/<name> (optionally /load)
        const char *sub = path + 13; // points to "" or "/..."
        if (sub == NULL || sub[0] == '\0' || strcmp(sub, "") == 0 || strcmp(sub, "/") == 0) {
            // list profiles
            char listbuf[4096];
            build_profiles_list_json(listbuf, sizeof(listbuf));
            send_http_response(client_fd, "200 OK", "application/json", listbuf);
        } else {
            // sub like /name or /name/load
            char name[256];
            const char *p = sub + 1; // skip '/'
            const char *slash = strchr(p, '/');
            if (slash) {
                size_t nlen = slash - p;
                if (nlen >= sizeof(name)) nlen = sizeof(name)-1;
                memcpy(name, p, nlen);
                name[nlen] = '\0';
                const char *action = slash + 1;
                if (strcmp(action, "load") == 0 && strcmp(method, "POST") == 0) {
                    char body[2048];
                    if (read_profile_file(name, body, sizeof(body)) == 0) {
                        // parse key=value lines
                        char *ln = strtok(body, "\n");
                        while (ln) {
                            char key[64], val[64];
                            if (sscanf(ln, "%63[^=]=%63s", key, val) == 2) {
                                if (strcmp(key, "safe_min") == 0) safe_min = atoi(val);
                                else if (strcmp(key, "safe_max") == 0) safe_max = atoi(val);
                                else if (strcmp(key, "temp_max") == 0) temp_max = atoi(val);
                            }
                            ln = strtok(NULL, "\n");
                        }
                        snprintf(response, sizeof(response), "{\"status\":\"ok\",\"loaded\":\"%s\"}", name);
                        send_http_response(client_fd, "200 OK", "application/json", response);
                    } else {
                        snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"profile not found\"}");
                        send_http_response(client_fd, "404 Not Found", "application/json", response);
                    }
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"unknown action\"}");
                    send_http_response(client_fd, "400 Bad Request", "application/json", response);
                }
            } else {
                // path is /api/profiles/<name>
                const char *prof = p;
                if (strcmp(method, "GET") == 0) {
                    char body[4096];
                    if (read_profile_file(prof, body, sizeof(body)) == 0) {
                        // return file contents as text/plain
                        send_http_response(client_fd, "200 OK", "text/plain", body);
                    } else {
                        snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"not found\"}");
                        send_http_response(client_fd, "404 Not Found", "application/json", response);
                    }
                } else if (strcmp(method, "POST") == 0) {
                    const char *body_start = strstr(request, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        // save profile
                        if (write_profile_file(prof, body_start) == 0) {
                            snprintf(response, sizeof(response), "{\"status\":\"ok\"}");
                            send_http_response(client_fd, "201 Created", "application/json", response);
                        } else {
                            snprintf(response, sizeof(response), "{\"status\":\"error\"}");
                            send_http_response(client_fd, "500 Internal Server Error", "application/json", response);
                        }
                    }
                } else if (strcmp(method, "DELETE") == 0) {
                    if (delete_profile_file(prof) == 0) {
                        snprintf(response, sizeof(response), "{\"status\":\"ok\"}");
                        send_http_response(client_fd, "200 OK", "application/json", response);
                    } else {
                        snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"not found\"}");
                        send_http_response(client_fd, "404 Not Found", "application/json", response);
                    }
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"method not allowed\"}");
                    send_http_response(client_fd, "405 Method Not Allowed", "application/json", response);
                }
            }
        }
    }
    else if (strncmp(path, "/api/settings/", 14) == 0 && strcmp(method, "POST") == 0) {
        // Extract setting name (safe-max, safe-min, temp-max)
        const char *setting = path + 14;
        
        // Parse JSON body {\"value\":12345}
        const char *body_start = strstr(request, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int value = 0;
            sscanf(body_start, "{\"value\":%d}", &value);
            
            if (strcmp(setting, "safe-max") == 0) {
                safe_max = value;
                snprintf(response, sizeof(response), "{\"status\":\"ok\",\"safe_max\":%d}", safe_max);
            }
            else if (strcmp(setting, "safe-min") == 0) {
                safe_min = value;
                snprintf(response, sizeof(response), "{\"status\":\"ok\",\"safe_min\":%d}", safe_min);
            }
            else if (strcmp(setting, "temp-max") == 0) {
                if (value >= 50 && value <= 110) {
                    temp_max = value;
                    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"temp_max\":%d}", temp_max);
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"temp_max must be 50-110\"}");
                }
            }
            else {
                snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"unknown setting\"}");
            }
            send_http_response(client_fd, "200 OK", "application/json", response);
        }
    }
    else {
        snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"not found\"}");
        send_http_response(client_fd, "404 Not Found", "application/json", response);
    }
}

// Handle incoming HTTP connections
void handle_http_connections() {
    if (http_fd < 0) return;
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(http_fd, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd >= 0) {
        char buffer[4096];
        ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            handle_http_request(client_fd, buffer);
        }
        close(client_fd);
    }
}

void handle_socket_commands(int *current_temp, int *current_freq, int min_freq, int max_freq_limit) {
    int client_fd = accept(socket_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return;
    }
    
    char buffer[256];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        
        // Parse command
        char cmd[64], arg[192];
        char response[512];
        
        if (sscanf(buffer, "%63s %191s", cmd, arg) >= 1) {
            if (strcmp(cmd, "set-safe-max") == 0 && sscanf(arg, "%d", &safe_max) == 1) {
                if (safe_max > max_freq_limit) safe_max = max_freq_limit;
                if (safe_max < min_freq) safe_max = 0;
                snprintf(response, sizeof(response), "OK: safe_max set to %d kHz\n", safe_max);
            }
            else if (strcmp(cmd, "set-safe-min") == 0 && sscanf(arg, "%d", &safe_min) == 1) {
                if (safe_min < min_freq) safe_min = min_freq;
                if (safe_min > max_freq_limit) safe_min = max_freq_limit;
                snprintf(response, sizeof(response), "OK: safe_min set to %d kHz\n", safe_min);
            }
            else if (strcmp(cmd, "set-temp-max") == 0 && sscanf(arg, "%d", &temp_max) == 1) {
                if (temp_max < 50 || temp_max > 110) {
                    snprintf(response, sizeof(response), "ERROR: temp_max must be 50-110°C\n");
                } else {
                    snprintf(response, sizeof(response), "OK: temp_max set to %d°C\n", temp_max);
                }
            }
            else if (strcmp(cmd, "status") == 0) {
                snprintf(response, sizeof(response), 
                    "Temperature: %d°C\n"
                    "Current Freq: %d kHz\n"
                    "safe_min: %d kHz\n"
                    "safe_max: %d kHz\n"
                    "temp_max: %d°C\n",
                    *current_temp, *current_freq, safe_min, safe_max, temp_max);
            }
            else if (strcmp(cmd, "quit") == 0) {
                snprintf(response, sizeof(response), "OK: Shutting down\n");
                should_exit = 1;
            }
            else {
                snprintf(response, sizeof(response), "ERROR: Unknown command\n");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: Invalid command format\n");
        }
        
        send(client_fd, response, strlen(response), 0);
    }
    
    close(client_fd);
}

void print_help(const char *name) {
    printf("Usage: %s [OPTIONS]\n", name);
    printf("  --dry-run            Simulate frequency setting (no writes)\n");
    printf("  --log <path>         Append log messages to a file\n");
    printf("  --sensor <path>      Manually specify temp sensor file\n");
    printf("  --safe-min <freq>    Optional safe minimum frequency in kHz (e.g. 2000000)\n");
    printf("  --safe-max <freq>    Optional safe maximum frequency in kHz (e.g. 3000000)\n");
    printf("  --temp-max <temp>    Maximum temperature threshold in °C (default 95)\n");
    printf("  --web-port [port]    Enable web interface (default port: %d, or specify custom)\n", DEFAULT_WEB_PORT);
    printf("  --verbose            Enable verbose logging\n");
    printf("  --quiet              Quiet mode (errors only)\n");
    printf("  --silent             Silent mode (no output)\n");
    printf("  --help               Show this help message\n");
    printf("\nConfig file: %s (optional)\n", CONFIG_FILE);
    printf("Supported keys: temp_max, safe_min, safe_max, sensor, web_port\n");
    printf("\nWeb Interface:\n");
    printf("  Use --web-port (without argument) for default port %d\n", DEFAULT_WEB_PORT);
    printf("  Use --web-port <port> for custom port (1024-65535)\n");
}

int main(int argc, char *argv[]) {
    // Load config file first (CLI args will override)
    load_config_file();
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logfile = fopen(argv[++i], "a");
            if (!logfile) {
                fprintf(stderr, "Error: Cannot open log file: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            strncpy(temp_path, argv[++i], sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
        } else if (strcmp(argv[i], "--safe-min") == 0 && i + 1 < argc) {
            safe_min = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--safe-max") == 0 && i + 1 < argc) {
            safe_max = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--temp-max") == 0 && i + 1 < argc) {
            temp_max = atoi(argv[++i]);
            if (temp_max < 50 || temp_max > 110) {
                fprintf(stderr, "Error: --temp-max must be between 50 and 110°C\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            log_level = LOGLEVEL_VERBOSE;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            log_level = LOGLEVEL_QUIET;
        } else if (strcmp(argv[i], "--silent") == 0) {
            log_level = LOGLEVEL_SILENT;
        } else if (strcmp(argv[i], "--web-port") == 0) {
            // Check if next arg is a number or another flag
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                web_port = atoi(argv[++i]);
                if (web_port < 1024 || web_port > 65535) {
                    fprintf(stderr, "Error: Invalid port number (use 1024-65535)\n");
                    return 1;
                }
            } else {
                // No port specified, use default
                web_port = DEFAULT_WEB_PORT;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    // Setup signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Write PID file
    write_pid_file();
    
    // Setup socket for remote control
    if (setup_socket() < 0) {
        LOG_ERROR("Warning: Failed to setup control socket\n");
    } else {
        LOG_VERBOSE("Control socket created at %s (permissions: 0666)\n", SOCKET_PATH);
    }
    
    // Setup HTTP server if web port specified
    if (web_port > 0) {
        if (setup_http_server() < 0) {
            LOG_ERROR("Warning: Failed to setup web interface on port %d\n", web_port);
        }
    }

    int last_freq = 0;
    int temp = 0;
    int freq = 0;

    char min_path[512], max_path[512];
    snprintf(min_path, sizeof(min_path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    snprintf(max_path, sizeof(max_path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");

    int min_freq = read_freq_value(min_path);
    int max_freq_limit = read_freq_value(max_path);

    if (min_freq <= 0 || max_freq_limit <= 0) {
        LOG_ERROR("Failed to read min/max CPU frequency\n");
        cleanup_socket();
        return 1;
    }
    
    // Store CPU limits in globals for API
    cpu_min_freq = min_freq;
    cpu_max_freq = max_freq_limit;
    
    LOG_VERBOSE("CPU Frequency range: %d - %d kHz\n", min_freq, max_freq_limit);
    LOG_INFO("CPU Throttle daemon started (PID: %d)\n", getpid());

    while (!should_exit) {
        // Handle socket commands (non-blocking)
        if (socket_fd >= 0) {
            handle_socket_commands(&temp, &freq, min_freq, max_freq_limit);
        }
        
        // Handle HTTP requests (non-blocking)
        handle_http_connections();
        
        // Recalculate max_freq based on safe_max
        int max_freq = max_freq_limit;
        if (safe_max > 0 && safe_max < max_freq) {
            max_freq = safe_max;
        }
        
        temp = read_temp();
        if (temp < 0) {
            LOG_ERROR("Failed to read CPU temperature\n");
            cleanup_socket();
            return 1;
        }
        
        // Update global state for API
        current_temp = temp;

        // Calculate temperature thresholds proportional to temp_max
        // Default: 75, 82, 88, 95 (relative to 95°C max)
        // Thresholds at: 79%, 86%, 93%, 100% of temp_max
        int thresh_light = temp_max * 79 / 100;   // ~75°C at default
        int thresh_medium = temp_max * 86 / 100;  // ~82°C at default
        int thresh_strong = temp_max * 93 / 100;  // ~88°C at default

        int freq = max_freq;

        // Temperature thresholds optimized for mobile CPU
        if (temp >= temp_max) {
            // absolute failsafe at temp_max
            freq = min_freq;
        }
        else if (temp >= thresh_strong) {
            // strong throttle: 40% of range above minimum
            freq = min_freq + (max_freq - min_freq) * 40 / 100;
        }
        else if (temp >= thresh_medium) {
            // medium throttle: 65% of range above minimum
            freq = min_freq + (max_freq - min_freq) * 65 / 100;
        }
        else if (temp >= thresh_light) {
            // light throttle: 85% of range above minimum
            freq = min_freq + (max_freq - min_freq) * 85 / 100;
        }
        else {
            // cool enough → full speed
            freq = max_freq;
        }

        // apply optional safe minimum if set (except in emergency)
        if (safe_min > 0 && temp < temp_max) {
            if (freq < safe_min) {
                freq = safe_min;
            }
        }
        
        current_freq = freq;

        // hysteresis: only change if difference is significant (10%)
        if (abs(freq - last_freq) > (max_freq - min_freq) / 10) {
            set_max_freq_all_cpus(freq);
            LOG_INFO("Temp: %d°C → MaxFreq: %d kHz%s\n",
                   temp, freq, dry_run ? " [DRY-RUN]" : "");
            if (logfile) {
                fprintf(logfile, "Temp: %d°C → MaxFreq: %d kHz\n", temp, freq);
                fflush(logfile);
            }
            last_freq = freq;
        }

        sleep(1);
    }

    cleanup_socket();
    if (logfile) fclose(logfile);
    LOG_INFO("Shutting down gracefully...\n");
    return 0;
}