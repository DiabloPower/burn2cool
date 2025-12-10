#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <dirent.h>

#define SOCKET_PATH "/tmp/cpu_throttle.sock"

char* get_profile_dir() {
    return "/var/lib/cpu_throttle/profiles";
}

void ensure_profile_dir() {
    char *dir = get_profile_dir();
    // Create ~/.config if needed
    char config_dir[512];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(config_dir, sizeof(config_dir), "%s/.config", home);
    mkdir(config_dir, 0755);
    
    // Create ~/.config/cpu_throttle if needed
    snprintf(config_dir, sizeof(config_dir), "%s/.config/cpu_throttle", home);
    mkdir(config_dir, 0755);
    
    // Create profiles directory
    mkdir(dir, 0755);
}

void print_help(const char *name) {
    printf("Usage: %s <command> [args]\n", name);
    printf("\nCommands:\n");
    printf("  set-safe-max <freq>    Set maximum frequency in kHz\n");
    printf("  set-safe-min <freq>    Set minimum frequency in kHz\n");
    printf("  set-temp-max <temp>    Set maximum temperature in °C\n");
    printf("  set-thermal-zone <num> Set thermal zone (-1=auto, 0-100)\n");
    printf("  set-use-avg-temp <0|1> Use average CPU temperature (0=no, 1=yes)\n");
    printf("  set-excluded-types <csv>  Comma-separated thermal type names to exclude (e.g. INT3400,INT3402)\n");
    printf("  status                 Show current status\n");
    printf("  quit                   Shutdown cpu_throttle daemon\n");
    printf("\nProfile commands:\n");
    printf("  save-profile <name>    Save current settings to a profile\n");
    printf("  load-profile <name>    Load settings from a profile\n");
    printf("  list-profiles          List all saved profiles (accepts --json/-j)\n");
    printf("  delete-profile <name>  Delete a profile\n");
    printf("  get-profile <name>     Print profile contents\n");
    printf("  put-profile <name> <file>  Upload profile contents from a file\n");
    printf("  version                Print daemon version\n");
    printf("  limits                 Print CPU min/max limits (accepts --json/-j)\n");
    printf("  zones                  Print thermal zones (accepts --json/-j)\n");
    printf("  restart                Restart the daemon (if running)\n");
    printf("  start                  Start the daemon (systemctl or background)\n");
    printf("\nSkin commands:\n");
        printf("  skins list             List installed system-wide skins\n");
        printf("  skins install <archive> Install a local skin archive (tar.gz, tar, zip) by uploading it to the daemon\n");
        printf("    --activate, -a       Activate the skin after install (attempts to parse installed id from response)\n");
        printf("  skins activate <id>    Activate a skin by id\n");
        printf("  skins deactivate <id>  Deactivate the specified skin\n");
        printf("  skins remove <id>      Remove a skin (delete files; admin-only)\n");
        printf("  skins default          Reset UI to the built-in default (clear active skin)\n");
        printf("    --json, -j           Request JSON output (daemon will return JSON)\n");
        printf("    --pretty, -p         Request JSON output and attempt to format it (if tool available)\n");
    printf("\nExamples:\n");
    printf("  %s set-safe-max 3000000\n", name);
    printf("  %s save-profile gaming\n", name);
    printf("  %s load-profile powersave\n", name);
    printf("  %s status\n", name);
    printf("\nProfiles are stored in: %s\n", get_profile_dir());
}

int send_command(const char *cmd) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect to cpu_throttle daemon.\n");
        fprintf(stderr, "Make sure cpu_throttle is running.\n");
        close(sock_fd);
        return -1;
    }

    printf("Connected to socket\n");

    char cmd_nl[512]; snprintf(cmd_nl, sizeof(cmd_nl), "%s\n", cmd);
    if (send(sock_fd, cmd_nl, strlen(cmd_nl), 0) < 0) {
        perror("send");
        close(sock_fd);
        return -1;
    }

    printf("Sent command: %s\n", cmd);

    char response[512];
    printf("Waiting for response...\n");
    ssize_t n = recv(sock_fd, response, sizeof(response) - 1, 0);
    printf("recv returned %zd\n", n);
    if (n > 0) {
        response[n] = '\0';
        printf("Received: '%s'\n", response);
        printf("%s", response);
    } else if (n == 0) {
        printf("Connection closed by server\n");
    } else {
        perror("recv");
    }

    close(sock_fd);
    return 0;
}

// Send a command and return the response string (malloc'ed, caller frees).
char *send_command_get_response(const char *cmd) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr)); addr.sun_family = AF_UNIX; strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock_fd); return NULL; }
    char cmd_nl[512]; snprintf(cmd_nl, sizeof(cmd_nl), "%s\n", cmd);
    if (send(sock_fd, cmd_nl, strlen(cmd_nl), 0) < 0) { close(sock_fd); return NULL; }
    // Read up to some reasonable max (e.g., 32KB)
    size_t cap = 32768; char *buf = malloc(cap);
    if (!buf) { close(sock_fd); return NULL; }
    size_t total = 0;
    while (1) {
        ssize_t n = recv(sock_fd, buf + total, (ssize_t)cap - 1 - total, 0);
        if (n > 0) {
            total += (size_t)n;
            if (cap - total < 4096) {
                size_t ncap = cap * 2;
                char *nb = realloc(buf, ncap);
                if (!nb) break;
                buf = nb; cap = ncap;
            }
            continue;
        }
        break;
    }
    ssize_t n = (ssize_t)total;
    close(sock_fd);
    if (n <= 0) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

void save_profile(const char *profile_name) {
    ensure_profile_dir();
    
    // Get current status from daemon
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect to daemon\n");
        close(sock_fd);
        return;
    }

    send(sock_fd, "status", 6, 0);
    char response[512];
    ssize_t n = recv(sock_fd, response, sizeof(response) - 1, 0);
    close(sock_fd);
    
    if (n <= 0) {
        fprintf(stderr, "Error: Failed to get status\n");
        return;
    }
    response[n] = '\0';
    
    // Parse status and save to profile
    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.config", get_profile_dir(), profile_name);
    
    FILE *fp = fopen(profile_path, "w");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create profile file\n");
        return;
    }
    
    // Parse response and extract values
    int safe_min = 0, safe_max = 0, temp_max = 0;
    char *line = strtok(response, "\n");
    while (line) {
        if (sscanf(line, "safe_min: %d", &safe_min) == 1) {
            if (safe_min > 0) fprintf(fp, "safe_min=%d\n", safe_min);
        } else if (sscanf(line, "safe_max: %d", &safe_max) == 1) {
            if (safe_max > 0) fprintf(fp, "safe_max=%d\n", safe_max);
        } else if (sscanf(line, "temp_max: %d", &temp_max) == 1) {
            fprintf(fp, "temp_max=%d\n", temp_max);
        }
        line = strtok(NULL, "\n");
    }
    
    fclose(fp);
    printf("Profile '%s' saved to %s\n", profile_name, profile_path);
}

void load_profile(const char *profile_name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "load-profile %s", profile_name);
    send_command(cmd);
}

// Client: streaming upload does not need base64

void list_profiles() {
    char *response = NULL;
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect to cpu_throttle daemon.\n");
        fprintf(stderr, "Make sure cpu_throttle is running.\n");
        close(sock_fd);
        return;
    }

    if (send(sock_fd, "list-profiles", 13, 0) < 0) {
        perror("send");
        close(sock_fd);
        return;
    }

    response = malloc(2048);
    if (!response) {
        close(sock_fd);
        return;
    }
    ssize_t n = recv(sock_fd, response, 2047, 0);
    close(sock_fd);
    if (n > 0) {
        response[n] = '\0';
        printf("Available profiles:\n");
        char *line = strtok(response, "\n");
        while (line) {
            // remove .config if present
            char *dot = strstr(line, ".config");
            if (dot) *dot = '\0';
            printf("  - %s\n", line);
            line = strtok(NULL, "\n");
        }
    }
    free(response);
}

void delete_profile(const char *profile_name) {
    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.config", get_profile_dir(), profile_name);
    
    if (unlink(profile_path) == 0) {
        printf("Profile '%s' deleted\n", profile_name);
    } else {
        fprintf(stderr, "Error: Profile '%s' not found\n", profile_name);
    }
}

void get_profile(const char *profile_name) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return; }
    struct sockaddr_un addr;
    memset(&addr,0,sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { fprintf(stderr, "Error: Cannot connect to daemon\n"); close(sock_fd); return; }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "get-profile %s", profile_name);
    send(sock_fd, cmd, strlen(cmd), 0);
    char response[8192];
    ssize_t n = recv(sock_fd, response, sizeof(response)-1, 0);
    close(sock_fd);
    if (n > 0) { response[n] = '\0'; printf("%s", response); }
}

void put_profile(const char *profile_name, const char *file_path) {
    FILE *fp = fopen(file_path, "rb");
    if (!fp) { fprintf(stderr, "Error: Cannot open file %s\n", file_path); return; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); fclose(fp); return; }
    struct sockaddr_un addr;
    memset(&addr,0,sizeof(addr)); addr.sun_family = AF_UNIX; strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { fprintf(stderr, "Error: Cannot connect to daemon\n"); close(sock_fd); fclose(fp); return; }

    // Send header: put-profile <name> <len>\n
    char header[512];
    int hlen = snprintf(header, sizeof(header), "put-profile %s %ld\n", profile_name, fsize);
    if (hlen < 0 || hlen >= (int)sizeof(header)) { fprintf(stderr, "Error: header too large\n"); close(sock_fd); fclose(fp); return; }
    if (send(sock_fd, header, hlen, 0) != hlen) { perror("send header"); close(sock_fd); fclose(fp); return; }

    // Stream file data in chunks to avoid high memory usage
    char sendbuf[4096];
    size_t remaining = (size_t)fsize;
    while (remaining > 0) {
        size_t toread = remaining > sizeof(sendbuf) ? sizeof(sendbuf) : remaining;
        size_t r = fread(sendbuf, 1, toread, fp);
        if (r <= 0) { fprintf(stderr, "Error: fread failed\n"); close(sock_fd); fclose(fp); return; }
        size_t off = 0;
        while (off < r) {
            ssize_t s = send(sock_fd, sendbuf + off, r - off, 0);
            if (s <= 0) { perror("send"); close(sock_fd); fclose(fp); return; }
            off += (size_t)s;
        }
        remaining -= r;
    }
    fclose(fp);

    // Read response
    char response[1024];
    ssize_t n = recv(sock_fd, response, sizeof(response)-1, 0);
    if (n > 0) { response[n] = '\0'; printf("%s\n", response); }
    close(sock_fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    // Handle profile commands locally
    if (strcmp(argv[1], "save-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        save_profile(argv[2]);
        return 0;
    } else if (strcmp(argv[1], "load-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        load_profile(argv[2]);
        return 0;
    } else if (strcmp(argv[1], "list-profiles") == 0) {
        // Optional --json/-j to return JSON via socket
        if (argc >= 3 && (strcmp(argv[2], "--json") == 0 || strcmp(argv[2], "-j") == 0 || strcmp(argv[2], "--pretty") == 0 || strcmp(argv[2], "-p") == 0)) {
            char *resp = send_command_get_response("list-profiles json");
            if (!resp) { fprintf(stderr, "Error: failed to query profiles\n"); return 1; }
            printf("%s\n", resp); free(resp); return 0;
        }
        list_profiles();
        return 0;
    } else if (strcmp(argv[1], "delete-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        delete_profile(argv[2]);
        return 0;
    }
    else if (strcmp(argv[1], "get-profile") == 0) {
        if (argc < 3) { fprintf(stderr, "Error: Profile name required\n"); return 1; }
        get_profile(argv[2]);
        return 0;
    }
    else if (strcmp(argv[1], "put-profile") == 0) {
        if (argc < 4) { fprintf(stderr, "Error: Profile name and file required\n"); return 1; }
        put_profile(argv[2], argv[3]);
        return 0;
    }
    else if (strcmp(argv[1], "start") == 0) {
        // Try to start via systemctl first
        if (system("systemctl start cpu_throttle.service") == 0) {
            printf("Started cpu_throttle via systemctl\n");
            return 0;
        }
        // Fallback: attempt to launch in background
        if (fork() == 0) {
            // Child
            execlp("cpu_throttle", "cpu_throttle", "--web-port", NULL);
            _exit(1);
        }
        printf("Started cpu_throttle in background\n");
        return 0;
    }

    // Skin commands
    if (strcmp(argv[1], "skins") == 0) {
                if (strcmp(argv[2], "install") == 0) {
                    // usage: cpu_throttle_ctl skins install <archive> [--activate|-a]
                    int activate_after = 0;
                    if (argc < 4) {
                        fprintf(stderr, "Error: archive path required for 'skins install'\n");
                        return 1;
                    }
                    // optional activate flag as argv[4]
                    const char *archive_path = argv[3];
                    if (argc >= 5) {
                        if (strcmp(argv[4], "--activate") == 0 || strcmp(argv[4], "-a") == 0) activate_after = 1;
                    }
                    // open file and send via put-skin
                    FILE *fp = fopen(archive_path, "rb");
                    if (!fp) { fprintf(stderr, "Error: cannot open file %s\n", argv[3]); return 1; }
                    fseek(fp, 0, SEEK_END); long fsize = ftell(fp); fseek(fp, 0, SEEK_SET);
                    if (fsize <= 0) { fprintf(stderr, "Error: empty file\n"); fclose(fp); return 1; }
                    // compose header: put-skin <basename> <len>\n
                    char *base = strrchr(argv[3], '/'); if (base) base++; else base = argv[3];
                    char header[512]; int hlen = snprintf(header, sizeof(header), "put-skin %s %ld\n", base, (long)fsize);
                    if (hlen < 0 || hlen >= (int)sizeof(header)) { fprintf(stderr, "Error: header too large\n"); fclose(fp); return 1; }
                    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (sock_fd < 0) { perror("socket"); fclose(fp); return 1; }
                    struct sockaddr_un addr; memset(&addr,0,sizeof(addr)); addr.sun_family=AF_UNIX; strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
                    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); close(sock_fd); fclose(fp); return 1; }
                    if (send(sock_fd, header, hlen, 0) != hlen) { perror("send header"); close(sock_fd); fclose(fp); return 1; }
                    char buf[4096]; size_t remaining = (size_t)fsize; while (remaining) {
                        size_t toread = remaining > sizeof(buf) ? sizeof(buf) : remaining; size_t r = fread(buf,1,toread,fp); if (r <= 0) { fprintf(stderr, "Error: fread failed\n"); close(sock_fd); fclose(fp); return 1; }
                        size_t off = 0; while (off < r) { ssize_t s = send(sock_fd, buf+off, r-off, 0); if (s <= 0) { perror("send"); close(sock_fd); fclose(fp); return 1; } off += (size_t)s; }
                        remaining -= r;
                    }
                    fclose(fp);
                    char resp[1024]; ssize_t n = recv(sock_fd, resp, sizeof(resp)-1, 0); if (n > 0) { resp[n]='\0'; printf("%s", resp); }
                    close(sock_fd);
                    // If requested, attempt to activate installed skin by parsing returned id
                    if (activate_after) {
                        const char *marker = "installed ";
                        char id[256] = {0};
                        const char *p = strstr(resp, marker);
                        if (p) {
                            p += strlen(marker);
                            size_t i = 0;
                            while (*p && *p != '\n' && *p != '\r' && *p != ' ' && i + 1 < sizeof(id)) id[i++] = *p++;
                            id[i] = '\0';
                            if (i > 0) {
                                char cmd[512]; snprintf(cmd, sizeof(cmd), "activate-skin %s", id);
                                send_command(cmd);
                            } else {
                                fprintf(stderr, "Warning: could not parse installed id for activation\n");
                            }
                        } else {
                            fprintf(stderr, "Warning: server did not report installed id, cannot activate\n");
                        }
                    }
                    return 0;
                }
        if (argc < 3) {
            fprintf(stderr, "Error: skins subcommand required (list|activate)");
            return 1;
        }
        if (strcmp(argv[2], "list") == 0) {
            // Optional 'json' argument to request JSON output
                if (argc >= 4 && (strcmp(argv[3], "json") == 0 || strcmp(argv[3], "--json") == 0 || strcmp(argv[3], "-j") == 0)) {
                char *resp = send_command_get_response("list-skins json");
                if (!resp) { fprintf(stderr, "Error: failed to query skins\n"); return 1; }
                printf("%s", resp);
                free(resp);
                return 0;
            }
            return send_command("list-skins");
        }
        else if (strcmp(argv[2], "activate") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Error: skin id required for 'skins activate'\n");
                return 1;
            }
            char cmd[256]; snprintf(cmd, sizeof(cmd), "activate-skin %s", argv[3]);
            return send_command(cmd);
        }
        else if (strcmp(argv[2], "deactivate") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Error: skin id required for 'skins deactivate'\n");
                return 1;
            }
            char cmd[256]; snprintf(cmd, sizeof(cmd), "deactivate-skin %s", argv[3]);
            return send_command(cmd);
        }
        else if (strcmp(argv[2], "default") == 0) {
            // Reset to default: find currently active skin via list-skins json, then deactivate it
            char *resp = send_command_get_response("list-skins json");
            if (!resp) { fprintf(stderr, "Error: failed to query skins\n"); return 1; }
            char *p = resp;
            int done = 0;
            while (p) {
                char *idpos = strstr(p, "\"id\":\"");
                if (!idpos) break;
                idpos += strlen("\"id\":\"");
                char *idend = strchr(idpos, '"'); if (!idend) break;
                size_t idlen = (size_t)(idend - idpos);
                char idbuf[256] = {0}; if (idlen >= sizeof(idbuf)) idlen = sizeof(idbuf)-1; memcpy(idbuf, idpos, idlen); idbuf[idlen] = '\0';
                char *activepos = strstr(idpos, "\"active\":true");
                if (activepos && activepos < strchr(idpos, '}')) {
                    // deactivate this skin
                    char cmd[256]; snprintf(cmd, sizeof(cmd), "deactivate-skin %s", idbuf);
                    send_command(cmd);
                    done = 1; break;
                }
                p = idpos + idlen;
            }
            free(resp);
            if (!done) { fprintf(stderr, "No active skin found or failed to deactivate.\n"); return 1; }
            return 0;
        }
        else if (strcmp(argv[2], "remove") == 0) {
            if (argc < 4) { fprintf(stderr, "Error: skin id required for 'skins remove'\n"); return 1; }
            char cmd[256]; snprintf(cmd, sizeof(cmd), "remove-skin %s", argv[3]);
            return send_command(cmd);
        }
        else {
            fprintf(stderr, "Error: Unknown skins subcommand '%s'\n", argv[2]);
            return 1;
        }
    }

    // status/limits/zones commands: support optional --json/-j or --pretty/-p
    if (strcmp(argv[1], "status") == 0) {
        if (argc >= 3 && (strcmp(argv[2], "--json") == 0 || strcmp(argv[2], "-j") == 0 || strcmp(argv[2], "--pretty") == 0 || strcmp(argv[2], "-p") == 0)) {
            char *resp = send_command_get_response("status json"); if (!resp) { fprintf(stderr, "Error: failed to query status\n"); return 1; } printf("%s", resp); free(resp); return 0;
        }
        return send_command("status");
    }
    if (strcmp(argv[1], "limits") == 0) {
        if (argc >= 3 && (strcmp(argv[2], "--json") == 0 || strcmp(argv[2], "-j") == 0 || strcmp(argv[2], "--pretty") == 0 || strcmp(argv[2], "-p") == 0)) {
            char *resp = send_command_get_response("limits json"); if (!resp) { fprintf(stderr, "Error: failed to query limits\n"); return 1; } printf("%s\n", resp); free(resp); return 0;
        }
        return send_command("limits");
    }
    if (strcmp(argv[1], "zones") == 0) {
        if (argc >= 3 && (strcmp(argv[2], "--json") == 0 || strcmp(argv[2], "-j") == 0 || strcmp(argv[2], "--pretty") == 0 || strcmp(argv[2], "-p") == 0)) {
            char *resp = send_command_get_response("zones json"); if (!resp) { fprintf(stderr, "Error: failed to query zones\n"); return 1; } printf("%s\n", resp); free(resp); return 0;
        }
        return send_command("zones");
    }

    // Build command string for daemon
    char cmd[256];
    if (argc == 2) {
        snprintf(cmd, sizeof(cmd), "%s", argv[1]);
    } else if (argc == 3) {
        snprintf(cmd, sizeof(cmd), "%s %s", argv[1], argv[2]);
    } else {
        fprintf(stderr, "Error: Too many arguments\n");
        print_help(argv[0]);
        return 1;
    }

    return send_command(cmd);
}
