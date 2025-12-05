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

#define CPUFREQ_PATH "/sys/devices/system/cpu"
#define SOCKET_PATH "/tmp/cpu_throttle.sock"
#define PID_FILE "/var/run/cpu_throttle.pid"
#define CONFIG_FILE "/etc/cpu_throttle.conf"

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
int socket_fd = -1; // socket file descriptor
int log_level = LOGLEVEL_NORMAL; // default logging level
volatile sig_atomic_t should_exit = 0; // flag for graceful shutdown

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
    printf("  --verbose            Enable verbose logging\n");
    printf("  --quiet              Quiet mode (errors only)\n");
    printf("  --silent             Silent mode (no output)\n");
    printf("  --help               Show this help message\n");
    printf("\nConfig file: %s (optional)\n", CONFIG_FILE);
    printf("Supported keys: temp_max, safe_min, safe_max, sensor\n");
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

    int last_freq = 0;
    int current_temp = 0;
    int current_freq = 0;

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
    
    LOG_VERBOSE("CPU Frequency range: %d - %d kHz\n", min_freq, max_freq_limit);
    LOG_INFO("CPU Throttle daemon started (PID: %d)\n", getpid());

    while (!should_exit) {
        // Handle socket commands (non-blocking)
        if (socket_fd >= 0) {
            handle_socket_commands(&current_temp, &current_freq, min_freq, max_freq_limit);
        }
        
        // Recalculate max_freq based on safe_max
        int max_freq = max_freq_limit;
        if (safe_max > 0 && safe_max < max_freq) {
            max_freq = safe_max;
        }
        
        int temp = read_temp();
        if (temp < 0) {
            LOG_ERROR("Failed to read CPU temperature\n");
            cleanup_socket();
            return 1;
        }
        
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