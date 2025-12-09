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
#include <pwd.h>
#include <pwd.h>
#include <strings.h>
#include <sys/time.h>
#include <poll.h>

#define CPUFREQ_PATH "/sys/devices/system/cpu"
#define SOCKET_PATH "/tmp/cpu_throttle.sock"
#define PID_FILE "/var/run/cpu_throttle.pid"
#define CONFIG_FILE "/etc/cpu_throttle.conf"
#define DEFAULT_WEB_PORT 8086  // Intel 8086 tribute!
#define DAEMON_VERSION "3.0"

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
int web_port = 0; // HTTP port (0 = disabled, DEFAULT_WEB_PORT = 8086 when enabled)
int log_level = LOGLEVEL_NORMAL; // default logging level
volatile sig_atomic_t should_exit = 0; // flag for graceful shutdown
int thermal_zone = -1; // thermal zone number (-1 = auto-detect, prefer zone 0 if CPU)
int use_avg_temp = 0; // use average temperature from CPU thermal zones

// Current state for API responses
int current_temp = 0;
int current_freq = 0;
int cpu_min_freq = 0;
int cpu_max_freq = 0;

/* Optional generated asset headers. Run `make assets` to generate headers in
 * `include/` and this file `include/assets_generated.h` will define
 * `USE_ASSET_HEADERS` so the server will prefer the generated binary arrays.
 */
#include "include/assets_generated.h"
#ifdef USE_ASSET_HEADERS
#include "include/index_html.h"   /* defines assets_index_html and assets_index_html_len */
#include "include/main_js.h"      /* defines assets_main_js and assets_main_js_len */
#include "include/styles_css.h"   /* defines assets_styles_css and assets_styles_css_len */
#include "include/favicon_ico.h"  /* defines assets_favicon_ico and assets_favicon_ico_len (may be zero-length) */

/* Provide canonical macro names that the rest of the source can use regardless
 * of how the header generator named the variables. xxd uses the sanitized
 * filename (with directory components) as the symbol base, e.g. for
 * `assets/index.html` it emits `assets_index_html`.
 */
#ifndef ASSET_INDEX_HTML
#define ASSET_INDEX_HTML assets_index_html
#define ASSET_INDEX_HTML_LEN assets_index_html_len
#endif
#ifndef ASSET_MAIN_JS
#define ASSET_MAIN_JS assets_main_js
#define ASSET_MAIN_JS_LEN assets_main_js_len
#endif
#ifndef ASSET_STYLES_CSS
#define ASSET_STYLES_CSS assets_styles_css
#define ASSET_STYLES_CSS_LEN assets_styles_css_len
#endif
#ifndef ASSET_FAVICON
#define ASSET_FAVICON assets_favicon_ico
#define ASSET_FAVICON_LEN assets_favicon_ico_len
#endif
#endif

// Logging macros (use __VA_ARGS__ form to satisfy pedantic compilers)
#define LOG_ERROR(...) do { if (log_level >= LOGLEVEL_QUIET) fprintf(stderr, __VA_ARGS__); } while(0)
#define LOG_INFO(...) do { if (log_level >= LOGLEVEL_NORMAL) printf(__VA_ARGS__); } while(0)
#define LOG_VERBOSE(...) do { if (log_level >= LOGLEVEL_VERBOSE) printf(__VA_ARGS__); } while(0)

void signal_handler(int sig) {
    (void)sig;
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
            } else if (strcmp(key, "thermal_zone") == 0) {
                thermal_zone = atoi(value);
                LOG_VERBOSE("Config: thermal_zone = %d\n", thermal_zone);
            } else if (strcmp(key, "avg_temp") == 0) {
                use_avg_temp = atoi(value);
                LOG_VERBOSE("Config: avg_temp = %d\n", use_avg_temp);
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
    if (use_avg_temp) {
        return read_avg_cpu_temp();
    }
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
    if (web_port == 0) {
        return 0; // HTTP disabled
    }
    
    // Create IPv6 socket and allow dual-stack (both IPv6 and IPv4) to avoid
    // delays when clients prefer IPv6 (e.g. 'localhost' resolving to ::1).
    http_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (http_fd < 0) {
        perror("HTTP socket");
        return -1;
    }
    
    // Allow port reuse
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Attempt to allow both IPv6 and IPv4 on the same socket (dual-stack).
    // On many Linux systems this is the default, but explicitly disable
    // IPV6_V6ONLY where supported to ensure clients connecting to ::1 or 127.0.0.1
    // reach the server without a fallback timeout.
    int ipv6only = 0;
    setsockopt(http_fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only));
    
    // Set non-blocking
    int flags = fcntl(http_fd, F_GETFL, 0);
    fcntl(http_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to IPv6 any; with IPV6_V6ONLY=0 this accepts IPv4 too.
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr = in6addr_any;
    addr6.sin6_port = htons(web_port);

    if (bind(http_fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
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
static ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)len;
}

void send_http_response_len(int client_fd, const char *status, const char *content_type, const void *body, size_t len, const char *extra_headers) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n",
             status, content_type, len);
    if (hlen < 0) hlen = 0;
    if (extra_headers) {
        size_t extra_len = strlen(extra_headers);
        if ((size_t)hlen + extra_len < sizeof(header) - 4) {
            memcpy(header + hlen, extra_headers, extra_len);
            hlen += (int)extra_len;
        }
    }
    if (hlen < (int)sizeof(header) - 4) {
        memcpy(header + hlen, "\r\n", 2);
        hlen += 2;
    }

    write_all(client_fd, header, (size_t)hlen);
    if (len > 0) write_all(client_fd, body, len);
}

// Backwards-compatible wrapper for null-terminated bodies
void send_http_response(int client_fd, const char *status, const char *content_type, const char *body) {
    send_http_response_len(client_fd, status, content_type, body, strlen(body), NULL);
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
    return "/var/lib/cpu_throttle/profiles";
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

// Profile helpers
int write_profile_file(const char *name, const char *body);

// Create default profiles if none exist
void create_default_profiles(int min_freq, int max_freq, int base_freq) {
    const char *dir = get_profile_dir();
    DIR *d = opendir(dir);
    if (!d) return; // dir not accessible
    struct dirent *ent;
    int has_profiles = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_type == DT_REG && strstr(ent->d_name, ".config")) {
            has_profiles = 1;
            break;
        }
    }
    closedir(d);
    if (has_profiles) return; // already has profiles

    // Create Energy Saver
    char body[256];
    snprintf(body, sizeof(body), "safe_min=%d\nsafe_max=%d\ntemp_max=%d\n", min_freq, min_freq, temp_max);
    if (write_profile_file("Energy Saver", body) == 0) {
        LOG_INFO("Created default profile: Energy Saver\n");
    }

    // Create Maximum Power
    snprintf(body, sizeof(body), "safe_min=%d\nsafe_max=%d\ntemp_max=%d\n", min_freq, max_freq, temp_max);
    if (write_profile_file("Maximum Power", body) == 0) {
        LOG_INFO("Created default profile: Maximum Power\n");
    }

    // Create Work Mode if base_freq available and different from max
    if (base_freq > 0 && base_freq != max_freq) {
        snprintf(body, sizeof(body), "safe_min=%d\nsafe_max=%d\ntemp_max=%d\n", min_freq, base_freq, temp_max);
        if (write_profile_file("Work Mode", body) == 0) {
            LOG_INFO("Created default profile: Work Mode\n");
        }
    }
}

// Forward declaration
int read_profile_file(const char *name, char *out, size_t size);

// List profiles as JSON array
void build_profiles_list_json(char *buffer, size_t size) {
    LOG_INFO("Building profiles list\n");
    const char *dir = get_profile_dir();
    DIR *d = opendir(dir);
    if (!d) {
        LOG_ERROR("Failed to open profiles dir: %s\n", dir);
        snprintf(buffer, size, "[]");
        return;
    }
    struct dirent *ent;
    size_t pos = 0;
    int first = 1;
    if (size > 0) {
        buffer[0] = '\0';
    }
    // start array
    pos += snprintf(buffer + pos, (pos < size) ? size - pos : 0, "[");
    while ((ent = readdir(d)) != NULL) {
        // skip hidden entries
        if (ent->d_name[0] == '.') continue;
        // only .config files
        if (!strstr(ent->d_name, ".config")) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            // extract name without .config
            char name[256];
            strncpy(name, ent->d_name, sizeof(name));
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';
            // read content
            char content[4096];
            if (read_profile_file(name, content, sizeof(content)) == 0) {
                // escape content for JSON
                char escaped[8192];
                size_t ei = 0;
                for (size_t i = 0; content[i] && ei < sizeof(escaped) - 10; i++) {
                    if (content[i] == '"') {
                        escaped[ei++] = '\\';
                        escaped[ei++] = '"';
                    } else if (content[i] == '\n') {
                        escaped[ei++] = '\\';
                        escaped[ei++] = 'n';
                    } else if (content[i] == '\\') {
                        escaped[ei++] = '\\';
                        escaped[ei++] = '\\';
                    } else {
                        escaped[ei++] = content[i];
                    }
                }
                escaped[ei] = '\0';
                // append comma if not first
                if (!first) {
                    pos += snprintf(buffer + pos, (pos < size) ? size - pos : 0, ",");
                }
                first = 0;
                // append object
                pos += snprintf(buffer + pos, (pos < size) ? size - pos : 0, "{\"name\":\"%s\",\"content\":\"%s\"}", name, escaped);
                LOG_INFO("Added profile: %s\n", name);
                // if buffer full, stop early
                if (pos >= size - 1) break;
            } else {
                LOG_ERROR("Failed to read profile: %s\n", name);
            }
        }
    }
    // close array
    pos += snprintf(buffer + pos, (pos < size) ? size - pos : 0, "]");
    LOG_INFO("Profiles list built: %s\n", buffer);
    closedir(d);
}

// (Removed) directory-specific profile listing helper — web UI uses global profiles only now.

// URL decode helper functions
int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '%' && *(src+1) && *(src+2) &&
            isxdigit(*(src+1)) && isxdigit(*(src+2))) {
            *dst++ = (hex_to_int(*(src+1)) << 4) | hex_to_int(*(src+2));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Read profile file into buffer
int read_profile_file(const char *name, char *out, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    LOG_INFO("Reading profile: %s\n", path);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open profile: %s\n", path);
        return -1;
    }
    size_t r = fread(out, 1, size - 1, fp);
    out[r] = '\0';
    fclose(fp);
    return 0;
}

// Write profile file from key=value format (body)
int write_profile_file(const char *name, const char *body) {
    if (ensure_profile_dir() != 0) return -1;
    char unescaped[4096];
    size_t i = 0, j = 0;
    while (body[i] && j < sizeof(unescaped) - 1) {
        if (body[i] == '\\' && body[i+1] == 'n') {
            unescaped[j++] = '\n';
            i += 2;
        } else {
            unescaped[j++] = body[i++];
        }
    }
    unescaped[j] = '\0';
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Failed to open for write: %s, errno: %d\n", path, errno);
        return -1;
    }
    size_t len = strlen(unescaped);
    size_t written = fwrite(unescaped, 1, len, fp);
    if (written != len) {
        LOG_ERROR("Failed to write to file: %s, written: %zu, expected: %zu\n", path, written, len);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (chmod(path, 0644) != 0) {
        LOG_ERROR("Failed to chmod: %s, errno: %d\n", path, errno);
    }
    return 0;
}

// Delete profile
int delete_profile_file(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    return unlink(path);
}

// Variants that operate on an explicit directory
// (Removed) directory-specific profile read helper — using global `read_profile_file`.

// (Removed) directory-specific profile write helper — using global `write_profile_file`.

// (Removed) directory-specific profile delete helper — using global `delete_profile_file`.

// Embedded dashboard: keep the original final visual design and add profile controls
const char *html_dashboard =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>CPU Throttle Monitor</title>"
"<style>"
"*, *:before, *:after { box-sizing: border-box; }"
"body{font-family:sans-serif;max-width:800px;margin:20px auto;padding:20px;background:#1a1a1a;color:#fff}"
"h1{color:#FF8C00}h2{border-bottom:2px solid #FF8C00;padding-bottom:10px}"
".card{background:#2a2a2a;padding:20px;margin:10px 0;border-radius:8px}"
".value{font-size:2em;font-weight:bold;color:#FF8C00}"
".label{color:#999;font-size:0.9em}"
"button{background:#FF8C00;color:#fff;border:none;padding:8px 12px;margin:5px;border-radius:4px;cursor:pointer;width:140px;text-align:center;display:inline-block}"
"button:hover{background:#e67e22}"
"input{padding:8px;margin:5px;border:1px solid #555;background:#333;color:#fff;border-radius:4px;max-width:100%}"
        "textarea{width:100%;max-width:100%;box-sizing:border-box;padding:8px;margin:5px;border:1px solid #555;background:#333;color:#fff;border-radius:4px;resize:vertical}"
        ".topbar{display:flex;justify-content:space-between;align-items:center;padding:6px 10px;font-size:0.9em;background:rgba(0,0,0,0.15);border-radius:6px;margin-bottom:12px}"
        ".topbar a{color:#fff;text-decoration:none;font-weight:600}"
        ".topbar .version{color:#ddd;font-size:0.85em}"
        "@media (max-width:700px) {"
            "body{padding:12px;margin:8px}"
            ".card{padding:12px}"
            ".topbar{font-size:0.85em;flex-direction:column;align-items:flex-start;gap:6px}"
            ".topbar .version{align-self:flex-end}"
            "input, textarea, button { width:100%; box-sizing:border-box; margin:6px 0; }"
            "/* ensure value text scales a bit */"
            ".value{font-size:1.6em}"
        "}"
    "</style>"
    "<link rel='stylesheet' href='styles.css'>"
    "</head><body>"
"<div class='topbar'><a href='https://github.com/DiabloPower/burn2cool' target='_blank' rel='noopener'>GitHub</a><div class='version'>v" DAEMON_VERSION "</div></div>"
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
"<div class='card'>"
"<h2>Profiles</h2>"
"<div class='label'>"
"  <label for='profilesSelect'>Choose profile:</label>"
"  <select id='profilesSelect' class='profiles-select'>"
"    <option value=''>(loading...)</option>"
"  </select>"
"</div>"
"<hr/>"
"<input id='pname' placeholder='profile filename' />"
"<textarea id='pcontent' rows='6' placeholder='profile content (key=value lines)'></textarea>"
"<div>"
"  <button id='createBtn' onclick='createProfile()'>Create</button>"
"  <button id='saveBtn' onclick='saveProfile()'>Save</button>"
"  <button id='deleteBtn' onclick='deleteProfile()'>Delete</button>"
"  <button id='loadBtn' onclick='loadProfile()'>Load to daemon</button>"
"</div>"
"<div id='toast' style='position:fixed; top:20px; right:20px; padding:12px 16px; border-radius:6px; background:#333; border:1px solid #555; color:#fff; box-shadow:0 4px 12px rgba(0,0,0,0.3); z-index:1000; display:none; font-weight:bold;'></div>"
"</div>"
"<script>"
"function showToast(message, type = 'success'){"
"  console.log('Showing toast:', message, type);"
"  const toast = document.getElementById('toast');"
"  console.log('Toast element:', toast);"
"  toast.innerText = message;"
"  if (type === 'success') {"
"    toast.style.background = 'green';"
"    toast.style.color = 'white';"
"  } else {"
"    toast.style.background = 'red';"
"    toast.style.color = 'white';"
"  }"
"  toast.style.display = 'block';"
"  setTimeout(() => { toast.style.display = 'none'; }, 3000);"
"}"
"function update(){"
"  fetch('/api/status').then(r=>r.json()).then(d=>{"
"    document.getElementById('temp').textContent=d.temperature+'°C';"
"    document.getElementById('freq').textContent=(d.frequency/1000).toFixed(0)+' MHz';"
"  }).catch(()=>{});"
"  refreshProfiles();"
"}"
"function setSetting(name,val){"
"  fetch('/api/settings/'+name,{method:'POST',body:JSON.stringify({value:parseInt(val)}),headers:{'Content-Type':'application/json'}}).then(()=>update());"
"}"
"function refreshProfiles(){"
"  fetch('/api/profiles').then(r=>r.json()).then(list=>{"
"    const select = document.getElementById('profilesSelect');"
"    const prev = select.value;"
"    select.innerHTML = '';"
"    if(!list || !Array.isArray(list) || list.length===0){"
"      const opt = document.createElement('option'); opt.value=''; opt.textContent='(no profiles)'; select.appendChild(opt); return;"
"    }"
"    const placeholder = document.createElement('option'); placeholder.value=''; placeholder.textContent='(select a profile)'; select.appendChild(placeholder);"
"    list.forEach(name=>{"
"      const opt = document.createElement('option'); opt.value = name; opt.textContent = name; select.appendChild(opt);"
"    });"
"    if(prev){ const found = Array.from(select.options).some(o=>o.value===prev); if(found) select.value = prev; }"
"    select.onchange = function(){ if(this.value) loadProfileToEditor(this.value); };"
"  }).catch(()=>{"
"    const select = document.getElementById('profilesSelect'); select.innerHTML=''; const opt=document.createElement('option'); opt.value=''; opt.textContent='(failed)'; select.appendChild(opt);"
"  });"
"}"
"function loadProfileToEditor(name){"
"  fetch('/api/profiles/'+encodeURIComponent(name)).then(r=>{ if(!r.ok) throw 0; return r.text(); }).then(t=>{"
"    console.log('Loaded to editor'); document.getElementById('pname').value = name;"
"    document.getElementById('pcontent').value = t;"
"    showToast('Profile loaded to editor', 'success');"
"  }).catch(e=>{ console.log('Load to editor error:', e); showToast('Failed to load profile', 'error'); });"
"}"
"function createProfile(){"
"  const name = document.getElementById('pname').value.trim(); const content = document.getElementById('pcontent').value;"
"  if(!name){ alert('Enter filename'); return; }"
"  fetch('/api/profiles', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name,content})}).then(r=>{ if(r.ok){ showToast('Profile created', 'success'); refreshProfiles(); } else { showToast('Create failed', 'error'); } });"
"}"
"function saveProfile(){"
"  const name = document.getElementById('pname').value.trim(); const content = document.getElementById('pcontent').value;"
"  if(!name){ alert('Enter filename'); return; }"
"  fetch('/api/profiles/'+encodeURIComponent(name), {method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({content})}).then(r=>{ if(r.ok){ console.log('Save successful'); showToast('Profile saved', 'success'); refreshProfiles(); } else { console.log('Save failed'); showToast('Save failed', 'error'); } }).catch(e=>{ console.log('Fetch error:', e); });"
"}"
"function deleteProfile(){"
"  const name = document.getElementById('pname').value.trim(); if(!name){ alert('Enter filename'); return; }"
"  if(!confirm('Delete '+name+'?')) return;"
"  fetch('/api/profiles/'+encodeURIComponent(name), {method:'DELETE'}).then(r=>{ if(r.ok){ showToast('Profile deleted', 'success'); document.getElementById('pname').value=''; document.getElementById('pcontent').value=''; refreshProfiles(); } else { showToast('Delete failed', 'error'); } });"
"}"
"function loadProfile(){"
"  const name = document.getElementById('pname').value.trim(); if(!name){ alert('Enter filename'); return; }"
"  fetch('/api/command', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({cmd:'load-profile '+name})}).then(r=>r.json()).then(j=>{ if(j && j.ok){ showToast('Profile loaded to daemon', 'success'); } else { showToast('Load failed', 'error'); } }).catch(()=>{ showToast('Load failed', 'error'); });"
"}"
"document.addEventListener('DOMContentLoaded', ()=>{"
"  document.getElementById('createBtn').addEventListener('click', createProfile);"
"  document.getElementById('saveBtn').addEventListener('click', saveProfile);"
"  document.getElementById('deleteBtn').addEventListener('click', deleteProfile);"
"  document.getElementById('loadBtn').addEventListener('click', loadProfile);"
"  setInterval(update,1000); update();"
"});"
"</script>"
"</body></html>";

const char *main_js =
"async function api(path, method='GET', body=null){\n"
"  const opts = {method, headers:{}};\n"
"  if(body!==null){ opts.headers['Content-Type']='application/json'; opts.body=JSON.stringify(body); }\n"
"  const r = await fetch('/api'+path, opts);\n"
"  return r.json();\n"
"}\n\n"
"let profilesCache = [];\n\n"
"async function refresh(){\n"
"  const data = await api('/profiles');\n"
"  if(!data.ok){ return; }\n"
"  profilesCache = data.profiles;\n"
"  renderList();\n"
"}\n\n"
"function renderList(){\n"
"  const list = document.getElementById('list');\n"
"  const filter = document.getElementById('filter').value.trim().toLowerCase();\n"
"  list.innerHTML = '';\n"
"  for(const p of profilesCache){\n"
"    if(filter && !p.name.toLowerCase().includes(filter) && !p.content.toLowerCase().includes(filter)) continue;\n"
"    const el = document.createElement('div');\n"
"    el.className = 'profile';\n"
"    el.innerHTML = `<strong>${p.name}</strong><div class='preview'>${escapeHtml(p.content).replace(/\\n/g,'<br>')}</div>`;\n"
"    el.addEventListener('click', ()=>{\n"
"      document.getElementById('pname').value = p.name;\n"
"      document.getElementById('pcontent').value = p.content;\n"
"    });\n"
"    const loadBtn = document.createElement('button'); loadBtn.textContent='Load';\n"
"    loadBtn.addEventListener('click', async (ev)=>{ ev.stopPropagation(); await loadProfile(p.name); });\n"
"    el.appendChild(loadBtn);\n"
"    list.appendChild(el);\n"
"  }\n"
"}\n\n"
"function escapeHtml(str){ return (str||'').replace(/[&<>\\\"]/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c])); }\n\n"
"async function createProfile(){\n"
"  const name = document.getElementById('pname').value.trim();\n"
"  const content = document.getElementById('pcontent').value;\n"
"  if(!name){ alert('Enter profile filename'); return; }\n"
"  const r = await api('/profiles', 'POST', {name, content});\n"
"  if(!r.ok) ;\n"
"  else await refresh();\n"
"}\n\n"
"async function saveProfile(){\n"
"  const name = document.getElementById('pname').value.trim();\n"
"  const content = document.getElementById('pcontent').value;\n"
"  if(!name){ alert('Enter profile filename'); return; }\n"
"  const r = await api('/profiles/'+encodeURIComponent(name), 'PUT', {content});\n"
"  if(!r.ok) ;\n"
"  else await refresh();\n"
"}\n\n"
"async function deleteProfile(){\n"
"  const name = document.getElementById('pname').value.trim();\n"
"  if(!name){ alert('Enter profile filename'); return; }\n"
"  if(!confirm('Delete profile '+name+' ?')) return;\n"
"  const r = await api('/profiles/'+encodeURIComponent(name), 'DELETE');\n"
"  if(!r.ok) ;\n"
"  else { document.getElementById('pname').value=''; document.getElementById('pcontent').value=''; await refresh(); }\n"
"}\n\n"
"async function loadProfile(name){\n"
"  // ask server to send load-profile NAME\n"
"  const r = await api('/command','POST',{cmd:`load-profile ${name}`});\n"
"  if(!r.ok) ;\n"
"  else ;\n"
"}\n\n"
"async function sendCmd(){\n"
"  const cmd = document.getElementById('cmdInput').value.trim();\n"
"  if(!cmd) return;\n"
"  const r = await api('/command','POST',{cmd});\n"
"  if(!r.ok) ;\n"
"  else document.getElementById('statusBox').textContent = r.resp;\n"
"}\n\n"
"window.addEventListener('DOMContentLoaded', ()=>{\n"
"  document.getElementById('refresh').addEventListener('click', refresh);\n"
"  document.getElementById('create').addEventListener('click', createProfile);\n"
"  document.getElementById('save').addEventListener('click', saveProfile);\n"
"  document.getElementById('delete').addEventListener('click', deleteProfile);\n"
"  document.getElementById('load').addEventListener('click', ()=>{ const n=document.getElementById('pname').value.trim(); if(n) loadProfile(n); });\n"
"  document.getElementById('sendCmd').addEventListener('click', sendCmd);\n"
"  document.getElementById('filter').addEventListener('keydown', (e)=>{ if(e.key==='Enter') refresh(); });\n"
"  document.getElementById('clearFilter').addEventListener('click', ()=>{ document.getElementById('filter').value=''; refresh(); });\n"
"  refresh();\n"
"});\n";

const char *styles_css =
"*, *:before, *:after { box-sizing: border-box; }\n"
"body{ font-family: sans-serif; margin: 12px; }\n"
"header h1{ margin:0 0 12px 0 }\n"
"main{ display:flex; gap:20px }\n"
"#profiles, #status{ flex:1 }\n"
"#list{ max-height:400px; overflow:auto; border:1px solid #ccc; padding:6px }\n"
".profile{ padding:6px; margin-bottom:6px; border-bottom:1px dashed #eee }\n"
".profile .preview{ color:#333; font-size:0.9em }\n"
"textarea{ width:100%; max-width:100%; box-sizing:border-box; resize:vertical }\n"
"input, textarea, button{ font-size:0.95em }\n"
"button{ min-width:140px; width:140px; padding:8px 12px; }\n"
"pre{ background:#f6f6f6; padding:8px; border:1px solid #ddd }\n"
"@media (max-width:700px) {\n"
"  body{ margin:8px; padding:12px }\n"
"  .card{ padding:12px }\n"
"  input, textarea, button { width:100%; box-sizing:border-box; margin:6px 0 }\n"
"}\n";

// Simple JSON helper to extract string value for a key like "cmd" or "name" from a small JSON body
static int extract_json_string(const char *body, const char *key, char *out, size_t outsz) {
    const char *k = strstr(body, key);
    if (!k) return -1;
    const char *col = strchr(k, ':');
    if (!col) return -1;
    const char *first_quote = strchr(col, '"');
    if (!first_quote) return -1;
    first_quote++;
    const char *end_quote = first_quote;
    while (*end_quote && *end_quote != '"') {
        if (*end_quote == '\\' && *(end_quote+1)) end_quote += 2; else end_quote++;
    }
    if (*end_quote != '"') return -1;
    size_t len = (size_t)(end_quote - first_quote);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, first_quote, len);
    out[len] = '\0';
    return 0;
}

// (Removed) helper: format IPv4 address into /proc/net/tcp hex — not used anymore.

// (Removed) per-connection UID lookup — web UI is global-only now.

// (Removed) per-connection UID lookup helper — not used for global-only web UI.

// (Removed) per-client profile dir resolver — web UI uses global profile dir.

// Parse HTTP request and route to handlers
void handle_http_request(int client_fd, const char *request) {
    char method[16], path[256];
    sscanf(request, "%s %s", method, path);
    
    LOG_VERBOSE("HTTP %s %s\n", method, path);
    
    char response[2048];
    
    // Serve bundled static assets
                if (strcmp(path, "/favicon.ico") == 0) {
            #ifdef USE_ASSET_HEADERS
                send_http_response_len(client_fd, "200 OK", "image/x-icon", ASSET_FAVICON, ASSET_FAVICON_LEN, NULL);
            #else
                send_http_response(client_fd, "200 OK", "image/x-icon", "");
            #endif
                return;
                }

                if (strcmp(path, "/main.js") == 0) {
            #ifdef USE_ASSET_HEADERS
                send_http_response_len(client_fd, "200 OK", "application/javascript", ASSET_MAIN_JS, ASSET_MAIN_JS_LEN, NULL);
            #else
                send_http_response(client_fd, "200 OK", "application/javascript", main_js);
            #endif
                return;
                }
            if (strcmp(path, "/styles.css") == 0) {
        #ifdef USE_ASSET_HEADERS
            send_http_response_len(client_fd, "200 OK", "text/css", ASSET_STYLES_CSS, ASSET_STYLES_CSS_LEN, NULL);
        #else
            send_http_response(client_fd, "200 OK", "text/css", styles_css);
        #endif
            return;
            }

    // Route API requests
        if (strcmp(path, "/") == 0) {
    #ifdef USE_ASSET_HEADERS
        send_http_response_len(client_fd, "200 OK", "text/html", ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN, NULL);
    #else
        send_http_response(client_fd, "200 OK", "text/html", html_dashboard);
    #endif
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
    else if (strncmp(path, "/api/profiles/", 14) == 0) {
        // /api/profiles/<name> or /api/profiles/<name>/load
        const char *p = path + 14; // points to name...
        const char *slash = strchr(p, '/');
        if (slash) {
            // action route: /api/profiles/<name>/load
            char name[256];
            size_t nlen = (size_t)(slash - p);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, p, nlen);
            name[nlen] = '\0';
            url_decode(name, name);
            const char *action = slash + 1;
            if (strcmp(action, "load") == 0 && strcmp(method, "POST") == 0) {
                char body[2048];
                if (read_profile_file(name, body, sizeof(body)) == 0) {
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
                    snprintf(response, sizeof(response), "{\"ok\":true,\"loaded\":\"%s\"}", name);
                    send_http_response(client_fd, "200 OK", "application/json", response);
                } else {
                    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"not found\"}");
                    send_http_response(client_fd, "404 Not Found", "application/json", response);
                }
            } else {
                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"unknown action\"}");
                send_http_response(client_fd, "400 Bad Request", "application/json", response);
            }
        } else {
            // /api/profiles/<name>
            char prof[256];
            url_decode(prof, p);
            // sanitize profile name: disallow path traversal and slashes
            if (strstr(prof, "..") || strchr(prof, '/')) {
                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"invalid profile name\"}");
                send_http_response(client_fd, "400 Bad Request", "application/json", response);
                return;
            }
                    if (strcmp(method, "GET") == 0) {
                        char body[4096];
                        if (read_profile_file(prof, body, sizeof(body)) == 0) {
                            send_http_response(client_fd, "200 OK", "text/plain", body);
                        } else {
                            snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"not found\"}");
                            send_http_response(client_fd, "404 Not Found", "application/json", response);
                        }
                    } else if (strcmp(method, "POST") == 0) {
                        const char *body_start = strstr(request, "\r\n\r\n");
                        if (body_start) {
                            body_start += 4;
                            if (write_profile_file(prof, body_start) == 0) {
                                snprintf(response, sizeof(response), "{\"ok\":true}");
                                send_http_response(client_fd, "201 Created", "application/json", response);
                            } else {
                                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"write failed\"}");
                                send_http_response(client_fd, "500 Internal Server Error", "application/json", response);
                            }
                        }
                    } else if (strcmp(method, "DELETE") == 0) {
                        if (delete_profile_file(prof) == 0) {
                            snprintf(response, sizeof(response), "{\"ok\":true}");
                            send_http_response(client_fd, "200 OK", "application/json", response);
                        } else {
                            snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"not found\"}");
                            send_http_response(client_fd, "404 Not Found", "application/json", response);
                        }
                    } else if (strcmp(method, "PUT") == 0) {
                        LOG_INFO("PUT request for profile: %s\n", prof);
                        /* Update profile (JSON {"content":"..."}) */
                        const char *body_start = strstr(request, "\r\n\r\n");
                        if (body_start) {
                            body_start += 4;
                            char content[4096] = {0};
                            if (extract_json_string(body_start, "\"content\"", content, sizeof(content)) == 0) {
                                LOG_INFO("PUT profile: %s, content length: %zu\n", prof, strlen(content));
                                if (write_profile_file(prof, content) == 0) {
                                    LOG_INFO("Saved profile: %s\n", prof);
                                    snprintf(response, sizeof(response), "{\"ok\":true}");
                                    send_http_response(client_fd, "200 OK", "application/json", response);
                                } else {
                                    LOG_ERROR("Failed to save profile: %s\n", prof);
                                    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"write failed\"}");
                                    send_http_response(client_fd, "500 Internal Server Error", "application/json", response);
                                }
                            } else {
                                LOG_ERROR("Invalid JSON in PUT for profile: %s\n", prof);
                                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"invalid json\"}");
                                send_http_response(client_fd, "400 Bad Request", "application/json", response);
                            }
                        }
                    } else {
                        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"method not allowed\"}");
                        send_http_response(client_fd, "405 Method Not Allowed", "application/json", response);
                    }
        }
    }
    else if (strcmp(path, "/api/profiles") == 0 && strcmp(method, "GET") == 0) {
        char listbuf[4096];
        build_profiles_list_json(listbuf, sizeof(listbuf));
        char resp[8192];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"profiles\":%s}", listbuf);
        send_http_response(client_fd, "200 OK", "application/json", resp);
    }
    
    else if (strcmp(path, "/api/profiles") == 0 && strcmp(method, "POST") == 0) {
        // Create profile from JSON {"name":"...","content":"..."}
        const char *body_start = strstr(request, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char name[256] = {0};
            char content[4096] = {0};
            if (extract_json_string(body_start, "\"name\"", name, sizeof(name)) == 0 &&
                extract_json_string(body_start, "\"content\"", content, sizeof(content)) == 0) {
                if (write_profile_file(name, content) == 0) {
                    snprintf(response, sizeof(response), "{\"ok\":true}");
                    send_http_response(client_fd, "201 Created", "application/json", response);
                } else {
                    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"write failed\"}");
                    send_http_response(client_fd, "500 Internal Server Error", "application/json", response);
                }
            } else {
                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"invalid json\"}");
                send_http_response(client_fd, "400 Bad Request", "application/json", response);
            }
        }
    }
    
    else if (strcmp(path, "/api/command") == 0 && strcmp(method, "POST") == 0) {
        // Accept JSON {"cmd":"..."} and handle a small set of commands locally
        const char *body_start = strstr(request, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            char cmd[256] = {0};
            if (extract_json_string(body_start, "\"cmd\"", cmd, sizeof(cmd)) == 0) {
                // handle known commands locally
                if (strcmp(cmd, "status") == 0) {
                    char body[2048];
                    build_status_json(body, sizeof(body));
                    send_http_response(client_fd, "200 OK", "application/json", body);
                } else if (strncmp(cmd, "load-profile ", 13) == 0) {
                    const char *pname = cmd + 13;
                    char body[4096];
                    if (read_profile_file(pname, body, sizeof(body)) == 0) {
                        // apply key=values
                        char tmp[4096];
                        strncpy(tmp, body, sizeof(tmp)-1);
                        char *ln = strtok(tmp, "\n");
                        while (ln) {
                            char key[64], val[64];
                            if (sscanf(ln, "%63[^=]=%63s", key, val) == 2) {
                                if (strcmp(key, "safe_min") == 0) safe_min = atoi(val);
                                else if (strcmp(key, "safe_max") == 0) safe_max = atoi(val);
                                else if (strcmp(key, "temp_max") == 0) temp_max = atoi(val);
                            }
                            ln = strtok(NULL, "\n");
                        }
                        snprintf(response, sizeof(response), "{\"ok\":true,\"loaded\":\"%s\"}", pname);
                        send_http_response(client_fd, "200 OK", "application/json", response);
                    } else {
                        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"not found\"}");
                        send_http_response(client_fd, "404 Not Found", "application/json", response);
                    }
                } else if (strcmp(cmd, "quit") == 0) {
                    should_exit = 1;
                    snprintf(response, sizeof(response), "{\"ok\":true,\"status\":\"shutting down\"}");
                    send_http_response(client_fd, "200 OK", "application/json", response);
                } else {
                    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"unknown command\"}");
                    send_http_response(client_fd, "400 Bad Request", "application/json", response);
                }
            } else {
                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"missing cmd\"}");
                send_http_response(client_fd, "400 Bad Request", "application/json", response);
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
    struct sockaddr_in6 client_addr6;
    socklen_t client_len = sizeof(client_addr6);
    // accept in a loop to drain all pending connections
    while (1) {
        int client_fd = accept(http_fd, (struct sockaddr*)&client_addr6, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            break;
        }
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
    // Drain all pending control-socket connections
    while (1) {
        int client_fd = accept(socket_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno != EINTR) perror("accept");
            break;
        }

        char buffer[256];
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';
            LOG_INFO("Received command: '%s'\n", buffer);

            // If the incoming data looks like an HTTP request, pass it to the HTTP handler
            if (strncmp(buffer, "GET ", 4) == 0 || strncmp(buffer, "POST ", 5) == 0 ||
                strncmp(buffer, "HEAD ", 5) == 0) {
                handle_http_request(client_fd, buffer);
                close(client_fd);
                continue;
            }

            // Parse command
            char cmd[64], arg[192];
            char response[512];

            // Find first space to split cmd and arg
            char *space = strchr(buffer, ' ');
            if (space) {
                size_t cmd_len = space - buffer;
                if (cmd_len > 63) cmd_len = 63;
                strncpy(cmd, buffer, cmd_len);
                cmd[cmd_len] = '\0';
                strncpy(arg, space + 1, 191);
                arg[191] = '\0'; // ensure null termination
                // trim trailing newline or spaces from arg
                char *end = arg + strlen(arg) - 1;
                while (end > arg && (*end == '\n' || *end == '\r' || *end == ' ')) {
                    *end = '\0';
                    end--;
                }
            } else {
                strncpy(cmd, buffer, 63);
                cmd[63] = '\0';
                arg[0] = '\0';
            }
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
                    if (strcmp(arg, "json") == 0) {
                        build_status_json(response, sizeof(response));
                    } else {
                        snprintf(response, sizeof(response), 
                            "Temperature: %d°C\n"
                            "Current Freq: %d kHz\n"
                            "safe_min: %d kHz\n"
                            "safe_max: %d kHz\n"
                            "temp_max: %d°C\n",
                            *current_temp, *current_freq, safe_min, safe_max, temp_max);
                    }
                }
                else if (strcmp(cmd, "list-profiles") == 0) {
                    if (strcmp(arg, "json") == 0) {
                        build_profiles_list_json(response, sizeof(response));
                    } else {
                        // For non-json, list as text
                        DIR *dir = opendir(get_profile_dir());
                        if (dir) {
                            struct dirent *ent;
                            char *ptr = response;
                            size_t remaining = sizeof(response);
                            while ((ent = readdir(dir)) && remaining > 2) {
                                if (ent->d_name[0] == '.') continue;
                                // check if regular file and ends with .config
                                char full[512];
                                snprintf(full, sizeof(full), "%s/%s", get_profile_dir(), ent->d_name);
                                struct stat st;
                                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && strstr(ent->d_name, ".config")) {
                                    // remove .config
                                    char name[256];
                                    strncpy(name, ent->d_name, sizeof(name));
                                    char *dot = strrchr(name, '.');
                                    if (dot) *dot = '\0';
                                    int written = snprintf(ptr, remaining, "%s\n", name);
                                    if (written > 0 && (size_t)written < remaining) {
                                        ptr += written;
                                        remaining -= written;
                                    } else {
                                        break;
                                    }
                                }
                            }
                            closedir(dir);
                        } else {
                            snprintf(response, sizeof(response), "ERROR: Cannot open profiles directory\n");
                        }
                    }
                }
                else if (strcmp(cmd, "load-profile") == 0) {
                    char body[4096];
                    if (read_profile_file(arg, body, sizeof(body)) == 0) {
                        // apply key=values
                        char tmp[4096];
                        strncpy(tmp, body, sizeof(tmp)-1);
                        char *ln = strtok(tmp, "\n");
                        while (ln) {
                            char key[64], val[64];
                            if (sscanf(ln, "%63[^=]=%63s", key, val) == 2) {
                                if (strcmp(key, "safe_min") == 0) safe_min = atoi(val);
                                else if (strcmp(key, "safe_max") == 0) safe_max = atoi(val);
                                else if (strcmp(key, "temp_max") == 0) temp_max = atoi(val);
                            }
                            ln = strtok(NULL, "\n");
                        }
                        snprintf(response, sizeof(response), "OK: Loaded profile %s\n", arg);
                    } else {
                        snprintf(response, sizeof(response), "ERROR: Profile %s not found\n", arg);
                    }
                }
                else {
                    snprintf(response, sizeof(response), "ERROR: Unknown command\n");
                }

            send(client_fd, response, strlen(response), 0);
        }

        close(client_fd);
    }
}

int read_avg_cpu_temp() {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return -1;
    struct dirent *entry;
    int total_temp = 0;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            char type_path[512];
            snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", entry->d_name);
            FILE *fp = fopen(type_path, "r");
            if (fp) {
                char type[256];
                if (fgets(type, sizeof(type), fp)) {
                    type[strcspn(type, "\n")] = 0;
                    char lower_type[256];
                    strcpy(lower_type, type);
                    for (char *p = lower_type; *p; ++p) *p = tolower(*p);
                    if (strstr(lower_type, "cpu") || strstr(lower_type, "core") || strstr(lower_type, "x86") ||
                        strstr(lower_type, "intel") || strstr(lower_type, "amd") || strstr(lower_type, "pkg")) {
                        char temp_path_zone[512];
                        snprintf(temp_path_zone, sizeof(temp_path_zone), "/sys/class/thermal/%s/temp", entry->d_name);
                        FILE *temp_fp = fopen(temp_path_zone, "r");
                        if (temp_fp) {
                            int temp_raw;
                            if (fscanf(temp_fp, "%d", &temp_raw) == 1) {
                                int temp_c = temp_raw / 1000;
                                if (temp_c > 0 && temp_c < 150) {
                                    total_temp += temp_c;
                                    count++;
                                }
                            }
                            fclose(temp_fp);
                        }
                    }
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    if (count == 0) return -1;
    int avg = total_temp / count;
    // Apply offset for avg_temp to reduce throttling frequency
    if (use_avg_temp) avg -= 10;
    return avg;
}

int detect_cpu_thermal_zone() {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) {
        LOG_VERBOSE("Thermal zones directory not found, using default zone 0\n");
        return 0;
    }
    struct dirent *entry;
    int best_zone = 0; // Default to zone 0
    int max_temp = -1;
    int zone0_temp = -1;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            int zone_num = atoi(entry->d_name + 12);
            char type_path[512];
            snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", entry->d_name);
            FILE *fp = fopen(type_path, "r");
            if (fp) {
                char type[256];
                if (fgets(type, sizeof(type), fp)) {
                    type[strcspn(type, "\n")] = 0;
                    char lower_type[256];
                    strcpy(lower_type, type);
                    for (char *p = lower_type; *p; ++p) *p = tolower(*p);
                    int is_cpu = strstr(lower_type, "cpu") || strstr(lower_type, "core") || strstr(lower_type, "x86") ||
                                 strstr(lower_type, "intel") || strstr(lower_type, "amd") || strstr(lower_type, "pkg");
                    // Read temp
                    char temp_path_test[512];
                    snprintf(temp_path_test, sizeof(temp_path_test), "/sys/class/thermal/%s/temp", entry->d_name);
                    FILE *temp_fp = fopen(temp_path_test, "r");
                    if (temp_fp) {
                        int temp_raw;
                        if (fscanf(temp_fp, "%d", &temp_raw) == 1) {
                            int temp_c = temp_raw / 1000;
                            if (temp_c > 0 && temp_c < 150) {
                                if (zone_num == 0) zone0_temp = temp_c;
                                if (is_cpu && temp_c > max_temp) {
                                    max_temp = temp_c;
                                    best_zone = zone_num;
                                }
                            }
                        }
                        fclose(temp_fp);
                    }
                }
                fclose(fp);
            }
        }
    }
    closedir(dir);
    // If no CPU zone found or zone 0 has reasonable temp, prefer zone 0
    if (best_zone == 0 || (zone0_temp > 0 && zone0_temp < 100)) {
        LOG_VERBOSE("Using thermal zone 0 (temp: %d°C)\n", zone0_temp);
        return 0;
    }
    LOG_VERBOSE("Auto-detected CPU thermal zone %d (temp: %d°C)\n", best_zone, max_temp);
    return best_zone;
}

void set_thermal_zone_path(int zone) {
    if (zone < 0) return;
    snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", zone);
    LOG_VERBOSE("Using thermal zone %d: %s\n", zone, temp_path);
}

void print_help(const char *name) {
    printf("Usage: %s [OPTIONS]\n", name);
    printf("  --dry-run            Simulate frequency setting (no writes)\n");
    printf("  --log <path>         Append log messages to a file\n");
    printf("  --sensor <path>      Manually specify temp sensor file\n");
    printf("  --thermal-zone <num> Manually specify thermal zone number (0,1,2,...)\n");
    printf("  --avg-temp           Use average temperature from CPU thermal zones\n");
    printf("  --safe-min <freq>    Optional safe minimum frequency in kHz (e.g. 2000000)\n");
    printf("  --safe-max <freq>    Optional safe maximum frequency in kHz (e.g. 3000000)\n");
    printf("  --temp-max <temp>    Maximum temperature threshold in °C (default 95)\n");
    printf("  --web-port [port]    Enable web interface (default port: %d, or specify custom)\n", DEFAULT_WEB_PORT);
    printf("  --verbose            Enable verbose logging\n");
    printf("  --quiet              Quiet mode (errors only)\n");
    printf("  --silent             Silent mode (no output)\n");
    printf("  --help               Show this help message\n");
    printf("\nConfig file: %s (optional)\n", CONFIG_FILE);
    printf("Supported keys: temp_max, safe_min, safe_max, sensor, thermal_zone, avg_temp, web_port\n");
    printf("\nWeb Interface:\n");
    printf("  Use --web-port (without argument) for default port %d\n", DEFAULT_WEB_PORT);
    printf("  Use --web-port <port> for custom port (1024-65535)\n");
}

int main(int argc, char *argv[]) {
    // Load config file first (CLI args will override)
    load_config_file();
    log_level = LOGLEVEL_VERBOSE; // Override config for debugging
    
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
        } else if (strcmp(argv[i], "--thermal-zone") == 0 && i + 1 < argc) {
            thermal_zone = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--avg-temp") == 0) {
            use_avg_temp = 1;
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

    // Setup thermal zone
    if (strcmp(temp_path, "/sys/class/thermal/thermal_zone0/temp") == 0) { // default not overridden by sensor
        if (thermal_zone != -1) {
            set_thermal_zone_path(thermal_zone);
        } else {
            int detected_zone = detect_cpu_thermal_zone();
            set_thermal_zone_path(detected_zone);
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

    char min_path[512], max_path[512], base_path[512];
    snprintf(min_path, sizeof(min_path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    snprintf(max_path, sizeof(max_path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    snprintf(base_path, sizeof(base_path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_base_frequency");

    int min_freq = read_freq_value(min_path);
    int max_freq_limit = read_freq_value(max_path);
    int base_freq = read_freq_value(base_path); // may be -1 if not available
    if (base_freq <= 0) {
        // Try alternative path for base frequency
        snprintf(base_path, sizeof(base_path), "/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
        base_freq = read_freq_value(base_path);
    }

    if (min_freq <= 0 || max_freq_limit <= 0) {
        LOG_ERROR("Failed to read min/max CPU frequency\n");
        cleanup_socket();
        return 1;
    }
    
    // Store CPU limits in globals for API
    cpu_min_freq = min_freq;
    cpu_max_freq = max_freq_limit;
    
    LOG_VERBOSE("CPU Frequency range: %d - %d kHz\n", min_freq, max_freq_limit);
    if (base_freq > 0) {
        LOG_VERBOSE("CPU Base frequency: %d kHz\n", base_freq);
    }

    // Create default profiles if none exist
    create_default_profiles(min_freq, max_freq_limit, base_freq);
    LOG_INFO("CPU Throttle daemon started (PID: %d)\n", getpid());

    // Use poll() to wait for incoming connections and run periodic tasks.
    struct timeval last_temp_tv;
    gettimeofday(&last_temp_tv, NULL);
    const int poll_timeout_ms = 250; // wake up periodically to handle tasks

    while (!should_exit) {
        struct pollfd pfds[2];
        int nfds = 0;
        if (socket_fd >= 0) {
            pfds[nfds].fd = socket_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (http_fd >= 0) {
            pfds[nfds].fd = http_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int pret = poll(pfds, nfds, poll_timeout_ms);
        if (pret > 0) {
            for (int i = 0; i < nfds; ++i) {
                if (pfds[i].revents & POLLIN) {
                    if (pfds[i].fd == socket_fd) {
                        handle_socket_commands(&temp, &freq, min_freq, max_freq_limit);
                    } else if (pfds[i].fd == http_fd) {
                        handle_http_connections();
                    }
                }
            }
        }

        // Periodic temperature read and throttle update (approx every 1s)
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - last_temp_tv.tv_sec) * 1000 + (now.tv_usec - last_temp_tv.tv_usec) / 1000;
        if (elapsed_ms >= 1000) {
            // Recalculate max_freq based on safe_max
            int max_freq = max_freq_limit;
            if (safe_max > 0 && safe_max < max_freq) max_freq = safe_max;

            temp = read_temp();
            if (temp < 0) {
                LOG_ERROR("Failed to read CPU temperature\n");
                cleanup_socket();
                return 1;
            }
            current_temp = temp;

            int thresh_light = temp_max * 79 / 100;
            int thresh_medium = temp_max * 86 / 100;
            int thresh_strong = temp_max * 93 / 100;
            int new_freq = max_freq;

            if (temp >= temp_max) new_freq = min_freq;
            else if (temp >= thresh_strong) new_freq = min_freq + (max_freq - min_freq) * 40 / 100;
            else if (temp >= thresh_medium) new_freq = min_freq + (max_freq - min_freq) * 65 / 100;
            else if (temp >= thresh_light) new_freq = min_freq + (max_freq - min_freq) * 85 / 100;
            else new_freq = max_freq;

            if (safe_min > 0 && temp < temp_max && new_freq < safe_min) new_freq = safe_min;

            current_freq = new_freq;
            if (abs(new_freq - last_freq) > (max_freq - min_freq) / 10) {
                set_max_freq_all_cpus(new_freq);
                LOG_INFO("Temp: %d°C → MaxFreq: %d kHz%s\n", temp, new_freq, dry_run ? " [DRY-RUN]" : "");
                if (logfile) {
                    fprintf(logfile, "Temp: %d°C → MaxFreq: %d kHz\n", temp, new_freq);
                    fflush(logfile);
                }
                last_freq = new_freq;
            }

            last_temp_tv = now;
        }
    }

    cleanup_socket();
    if (logfile) fclose(logfile);
    LOG_INFO("Shutting down gracefully...\n");
    return 0;
}