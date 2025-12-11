#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pwd.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <ftw.h>
#include <fcntl.h>

#define SOCKET_PATH "/tmp/cpu_throttle.sock"

// Zones state type and globals (declared early so format parser can populate them)
typedef struct zone_entry { int zone; char type[64]; int temp; int excluded; } zone_entry_t;
static zone_entry_t zones_arr[256];
static int zones_count = 0;
static int zones_sel = 0;
static int zones_offset = 0;

// Simple JSON pretty printer for limits/zones
char* pretty_print_json(const char* json) {
    static char buf[4096];
    char* out = buf;
    int indent = 0;
    int in_string = 0;
    int escape = 0;

    buf[0] = '\0';
    for (const char* p = json; *p; p++) {
        if (escape) {
            *out++ = *p;
            escape = 0;
            continue;
        }
        if (*p == '\\') {
            *out++ = *p;
            escape = 1;
            continue;
        }
        if (*p == '"') {
            in_string = !in_string;
            *out++ = *p;
            continue;
        }
        if (in_string) {
            *out++ = *p;
            continue;
        }
        if (*p == '{' || *p == '[') {
            *out++ = *p;
            *out++ = '\n';
            indent += 2;
            for (int i = 0; i < indent; i++) *out++ = ' ';
        } else if (*p == '}' || *p == ']') {
            indent -= 2;
            *out++ = '\n';
            for (int i = 0; i < indent; i++) *out++ = ' ';
            *out++ = *p;
        } else if (*p == ',') {
            *out++ = *p;
            *out++ = '\n';
            for (int i = 0; i < indent; i++) *out++ = ' ';
        } else if (*p == ':') {
            *out++ = *p;
            *out++ = ' ';
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
    return buf;
}

// Human-readable formatter for limits/zones JSON
char* format_limits_zones(const char* json, int is_limits) {
    static char buf[4096];
    char* out = buf;
    buf[0] = '\0';

    if (is_limits) {
        // Parse {"cpu_min_freq":800000,"cpu_max_freq":4500000,"temp_sensor":"/sys/class/thermal/thermal_zone6/temp"}
        long cpu_min = -1, cpu_max = -1;
        char temp_sensor[256] = "";
        sscanf(json, "{\"cpu_min_freq\":%ld,\"cpu_max_freq\":%ld,\"temp_sensor\":\"%255[^\"]}", &cpu_min, &cpu_max, temp_sensor);
        if (cpu_min != -1 && cpu_max != -1) {
            out += sprintf(out, "CPU Min Freq: %ld kHz\n", cpu_min);
            out += sprintf(out, "CPU Max Freq: %ld kHz\n", cpu_max);
            if (temp_sensor[0]) out += sprintf(out, "Temp Sensor: %s\n", temp_sensor);
        } else {
            out += sprintf(out, "Failed to parse limits JSON\n");
        }
    } else {
        zones_count = 0; // reset global counter before parsing
        // Parse zones array
        const char* p = json;
        if (strncmp(p, "{\"zones\":[", 10) == 0) {
            p += 10;
            int first = 1;
            while (*p && *p != ']') {
                if (*p == '{') {
                    int zone = -1;
                    char type[64] = "";
                    char temp_str[64] = "";
                    char excluded_str[16] = "";
                    sscanf(p, "{\"zone\":%d,\"type\":\"%63[^\"]\",\"temp\":%63[^,],\"excluded\":%15[^}]}", &zone, type, temp_str, excluded_str);
                    int excluded = strcmp(excluded_str, "true") == 0;
                    if (zone != -1) {
                        if (!first) out += sprintf(out, "\n");
                        out += sprintf(out, "Zone %d (%s):\n", zone, type);
                        out += sprintf(out, "  Temp=%s\n", temp_str);
                        out += sprintf(out, "  Excluded=%s\n", excluded ? "yes" : "no");
                        // populate global zones_arr for navigation if provided by caller
                        if (zones_count < 256) {
                            zones_arr[zones_count].zone = zone;
                            strncpy(zones_arr[zones_count].type, type, sizeof(zones_arr[zones_count].type)-1);
                            zones_arr[zones_count].type[sizeof(zones_arr[zones_count].type)-1] = '\0';
                            zones_arr[zones_count].temp = atoi(temp_str);
                            zones_arr[zones_count].excluded = excluded ? 1 : 0;
                            zones_count++;
                        }
                        first = 0;
                    }
                    // Skip to next object
                    while (*p && *p != '}') p++;
                    if (*p == '}') p++;
                    if (*p == ',') p++;
                } else {
                    p++;
                }
            }
        } else {
            out += sprintf(out, "Failed to parse zones JSON\n");
        }
    }
    *out = '\0';
    return buf;
}

// Shared state updated by poller/worker threads
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t last_msg_lock = PTHREAD_MUTEX_INITIALIZER;
static char status_buf[4096] = "(no status yet)";
static time_t status_ts = 0;
static char limits_buf[4096] = "(no limits yet)";
static time_t limits_ts = 0;
static char zones_buf[4096] = "(no zones yet)";
static time_t zones_ts = 0;
static char skins_buf[4096] = "(no skins yet)";
static time_t skins_ts = 0;
static char last_msg[256] = "";
static volatile int keep_running = 1;
static volatile int help_visible = 0;
static int help_offset = 0;
// Modes: 0=limits, 1=zones, 2=skins, 3=profiles
static int current_mode = 0;
static int mode_count = 4;
// history for sparkline
/* history/sparkline output disabled */
// profile filter
static char profile_filter[128] = "";
// skins selection
static int skins_sel = 0;
static int skins_offset = 0;
static char skins_list[256][256];
static int skins_count = 0;


// Helper: get current value of use_avg_temp from status_buf; returns 0 or 1 or -1 if not available
static int get_use_avg_temp(void) {
    pthread_mutex_lock(&state_lock);
    char local[4096]; strncpy(local, status_buf, sizeof(local)-1); local[sizeof(local)-1] = '\0';
    pthread_mutex_unlock(&state_lock);
    char *p = strstr(local, "\"use_avg_temp\"");
    if (!p) return -1;
    p = strchr(p, ':'); if (!p) return -1; p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "1", 1) == 0) return 1;
    if (strncmp(p, "\"true\"", 6) == 0) return 1;
    return 0;
}

static int get_running_user(char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    pthread_mutex_lock(&state_lock);
    char local[4096]; strncpy(local, status_buf, sizeof(local)-1); local[sizeof(local)-1] = '\0';
    pthread_mutex_unlock(&state_lock);
    char *p = strstr(local, "\"running_user\"");
    if (!p) return -1;
    p = strchr(p, ':'); if (!p) return -1; p++;
    while (*p && isspace((unsigned char)*p)) p++;
    // accept "name" or plain value
    if (*p == '"') {
        p++; char *q = strchr(p, '"'); if (!q) return -1; size_t len = q - p; if (len >= outsz) len = outsz - 1; strncpy(out, p, len); out[len] = '\0'; return 0;
    } else {
        char *q = p; size_t len = 0; while (*q && !isspace((unsigned char)*q) && *q != ',' && *q != '}') { q++; len++; }
        if (len >= outsz) len = outsz - 1; strncpy(out, p, len); out[len] = '\0'; return 0;
    }
}

/* forward declarations */
char *send_unix_command(const char *cmd);
static void spawn_verify_daemon_async(void);
static void set_last_msg(const char *fmt, ...);

// portable millisecond sleep helper (uses nanosleep to avoid feature-macro issues)
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// Helper: check if a command exists in PATH
static int command_exists_in_path(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    char *paths = strdup(path);
    char *p = strtok(paths, ":");
    while (p) {
        char full[1024]; snprintf(full, sizeof(full), "%s/%s", p, cmd);
        if (access(full, X_OK) == 0) { free(paths); return 1; }
        p = strtok(NULL, ":");
    }
    free(paths);
    return 0;
}

// Helper: spawn and wait (no shell), argv must be NULL-terminated
static int spawn_and_waitvp(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    } else if (pid < 0) return -1;
    else {
        int status; waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    }
}

// Helper: spawn a command detached (for 'setsid foo &')
static int spawn_detached_exec(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t sid = setsid(); (void)sid;
        // redirect fds to /dev/null
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); if (fd > 2) close(fd); }
        execvp(argv[0], argv);
        _exit(127);
    } else if (pid < 0) return -1;
    // parent returns immediately
    return 0;
}

// prompt helpers (use input window passed by caller)
static int prompt_input(WINDOW *inputwin, const char *prompt, char *buf, int bufsz) {
    werase(inputwin); box(inputwin,0,0);
    // print prompt and existing buffer content so user can edit
    int px = 2;
    mvwprintw(inputwin,1,px, "%s", prompt);
    px += (int)strlen(prompt);
    int base_x = px;
    if (!buf) return 0;
    // ensure buf is valid
    buf[bufsz-1] = '\0';
    if (buf[0]) {
        mvwprintw(inputwin,1, base_x, "%s", buf);
    }
    // simple inline editor: support left/right cursor, insert/delete, backspace
        // keep a copy of the original so ESC can restore it
        char orig[bufsz];
        strncpy(orig, buf, bufsz-1); orig[bufsz-1] = '\0';
        int len = (int)strlen(buf);
        int pos = len; // cursor position in [0..len]
        wmove(inputwin, 1, base_x + pos);
        wrefresh(inputwin);
        noecho(); cbreak(); keypad(inputwin, TRUE); curs_set(1);
        int ch;
        while (1) {
            ch = wgetch(inputwin);
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
            if (ch == 27) { // ESC = cancel (restore original)
                strncpy(buf, orig, bufsz-1); buf[bufsz-1] = '\0';
                len = (int)strlen(buf); pos = len;
                // redraw and exit
                mvwprintw(inputwin,1, base_x, "%s", buf);
                // clear trailing area
                for (int i = base_x + (int)strlen(buf); i < getmaxx(inputwin)-2; ++i) mvwprintw(inputwin,1,i, " ");
                wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                break;
            }
            if (ch == KEY_LEFT) {
                if (pos > 0) pos--;
                wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                continue;
            }
            if (ch == KEY_RIGHT) {
                if (pos < len) pos++;
                wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                continue;
            }
            if (ch == KEY_HOME) { pos = 0; wmove(inputwin,1, base_x + pos); wrefresh(inputwin); continue; }
            if (ch == KEY_END) { pos = len; wmove(inputwin,1, base_x + pos); wrefresh(inputwin); continue; }
            if (ch == KEY_DC) { // Delete key (delete at cursor)
                if (pos < len) {
                    for (int i = pos; i < len; ++i) buf[i] = buf[i+1];
                    len--; buf[len] = '\0';
                    // redraw
                    mvwprintw(inputwin,1, base_x, "%s", buf);
                    // clear trailing
                    mvwprintw(inputwin,1, base_x + len, " ");
                    wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                }
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (pos > 0) {
                    // remove char before cursor
                    for (int i = pos-1; i < len; ++i) buf[i] = buf[i+1];
                    pos--; len--; buf[len] = '\0';
                    mvwprintw(inputwin,1, base_x, "%s", buf);
                    mvwprintw(inputwin,1, base_x + len, " ");
                    wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                }
                continue;
            }
            if (isprint(ch)) {
                if (len < bufsz-1) {
                    // insert at cursor
                    for (int i = len; i >= pos; --i) buf[i+1] = buf[i];
                    buf[pos] = (char)ch; pos++; len++; buf[len] = '\0';
                    mvwprintw(inputwin,1, base_x, "%s", buf);
                    wmove(inputwin,1, base_x + pos); wrefresh(inputwin);
                }
                continue;
            }
            // ignore other keys
    }
    // if user pressed enter without typing and buf might be empty, keep as-is
    // finalize
    noecho(); cbreak(); curs_set(0); keypad(inputwin, FALSE); keypad(stdscr, TRUE);
    werase(inputwin); wrefresh(inputwin);
    return (int)strlen(buf);
}

static int prompt_char(WINDOW *inputwin, const char *prompt) {
    werase(inputwin); box(inputwin,0,0);
    mvwprintw(inputwin,1,2, "%s", prompt);
    wrefresh(inputwin);
    curs_set(1);
    int ch = wgetch(inputwin);
    curs_set(0);
    werase(inputwin); wrefresh(inputwin);
    return ch;
}

// spawn a system command in background and store result in last_msg
struct sys_arg { char cmd[512]; };
/* spawn_system_async/worker_system helper removed (unused) */

// Try to start daemon: prefer systemctl (user or system), else try command in PATH, else return failure message
static void *worker_start_daemon(void *v) {
    (void)v;
    set_last_msg("Starting daemon...");
    char final_msg[256] = "Failed to start daemon (service not found)";
    const char *svc_names[] = {"cpu_throttle.service", "cpu-throttle.service"};
    const char *bin_names[] = {"cpu_throttle", "cpu-throttle"};

    // detect preferred scope/name if installed
    int preferred_scope = 0; // 1=user, 2=system
    const char *preferred_name = NULL;
    for (size_t i = 0; i < sizeof(svc_names)/sizeof(svc_names[0]); ++i) {
        char *const argv_user[] = {"systemctl", "--user", "status", (char *)svc_names[i], NULL};
        if (spawn_and_waitvp(argv_user) == 0) { preferred_scope = 1; preferred_name = svc_names[i]; break; }
        char *const argv_system[] = {"systemctl", "status", (char *)svc_names[i], NULL};
        if (spawn_and_waitvp(argv_system) == 0 && preferred_scope == 0) { preferred_scope = 2; preferred_name = svc_names[i]; }
    }

    int rc = -1;
    if (preferred_scope == 1 && preferred_name) {
        char *const argv_user_start[] = {"systemctl", "--user", "start", (char *)preferred_name, NULL};
        rc = spawn_and_waitvp(argv_user_start);
        if (rc == 0) {
            set_last_msg("systemctl --user started %s, waiting for daemon...", preferred_name);
            int started = 0;
            for (int i = 0; i < 25; ++i) {
                char *r = send_unix_command("status");
                if (r) { free(r); started = 1; break; }
                sleep_ms(200);
            }
            if (started) { snprintf(final_msg, sizeof(final_msg), "Started via systemctl --user (%s)", preferred_name); set_last_msg("%s", final_msg); return NULL; }
            else { snprintf(final_msg, sizeof(final_msg), "systemctl --user started %s but daemon did not respond", preferred_name); set_last_msg("%s", final_msg); return NULL; }
        }
    }

    if (preferred_scope == 2 && preferred_name && geteuid() == 0) {
        char *const argv_system_start[] = {"systemctl", "start", (char *)preferred_name, NULL};
        rc = spawn_and_waitvp(argv_system_start);
        if (rc == 0) {
            set_last_msg("systemctl (system) started %s, waiting for daemon...", preferred_name);
            int started = 0;
            for (int i = 0; i < 25; ++i) {
                char *r = send_unix_command("status");
                if (r) { free(r); started = 1; break; }
                sleep_ms(200);
            }
            if (started) { snprintf(final_msg, sizeof(final_msg), "Started via systemctl (system) (%s)", preferred_name); set_last_msg("%s", final_msg); return NULL; }
            else { snprintf(final_msg, sizeof(final_msg), "systemctl (system) started %s but daemon did not respond", preferred_name); set_last_msg("%s", final_msg); return NULL; }
        }
    }

    // try user service names
    for (size_t i = 0; i < sizeof(svc_names)/sizeof(svc_names[0]); ++i) {
        char *const argv_user_start[] = {"systemctl", "--user", "start", (char *)svc_names[i], NULL};
        rc = spawn_and_waitvp(argv_user_start);
        if (rc == 0) {
            set_last_msg("systemctl --user started %s, waiting for daemon...", svc_names[i]);
            int started = 0;
            for (int k = 0; k < 25; ++k) {
                char *r = send_unix_command("status");
                if (r) { free(r); started = 1; break; }
                sleep_ms(200);
            }
            if (started) { snprintf(final_msg, sizeof(final_msg), "Started via systemctl --user (%s)", svc_names[i]); set_last_msg("%s", final_msg); return NULL; }
            else { snprintf(final_msg, sizeof(final_msg), "systemctl --user started %s but daemon did not respond", svc_names[i]); set_last_msg("%s", final_msg); return NULL; }
        }
    }

    // try system service names if running as root
    if (geteuid() == 0) {
        for (size_t i = 0; i < sizeof(svc_names)/sizeof(svc_names[0]); ++i) {
            char *const argv_system_start[] = {"systemctl", "start", (char *)svc_names[i], NULL};
            rc = spawn_and_waitvp(argv_system_start);
            if (rc == 0) {
                set_last_msg("systemctl (system) started %s, waiting for daemon...", svc_names[i]);
                int started = 0;
                for (int k = 0; k < 25; ++k) {
                    char *r = send_unix_command("status");
                    if (r) { free(r); started = 1; break; }
                    sleep_ms(200);
                }
                if (started) { snprintf(final_msg, sizeof(final_msg), "Started via systemctl (system) (%s)", svc_names[i]); set_last_msg("%s", final_msg); return NULL; }
                else { snprintf(final_msg, sizeof(final_msg), "systemctl (system) started %s but daemon did not respond", svc_names[i]); set_last_msg("%s", final_msg); return NULL; }
            }
        }
    }

    // try to find cpu_throttle or cpu-throttle in PATH
    for (size_t bi = 0; bi < sizeof(bin_names)/sizeof(bin_names[0]); ++bi) {
        if (!command_exists_in_path(bin_names[bi])) continue;
        char *const argv_det[] = {(char *)bin_names[bi], NULL};
        rc = spawn_detached_exec(argv_det);
        if (rc == 0) {
            if (rc == 0) {
                set_last_msg("Started %s from PATH, waiting for daemon...", bin_names[bi]);
                int started = 0;
                for (int i = 0; i < 25; ++i) {
                    char *r = send_unix_command("status");
                    if (r) { free(r); started = 1; break; }
                    sleep_ms(200);
                }
                if (started) { snprintf(final_msg, sizeof(final_msg), "Started %s from PATH", bin_names[bi]); set_last_msg("%s", final_msg); return NULL; }
                else { snprintf(final_msg, sizeof(final_msg), "Started %s from PATH but no response", bin_names[bi]); set_last_msg("%s", final_msg); return NULL; }
            }
        }
    }

    // fallback: not found or failed
    set_last_msg("Start attempts failed; waiting briefly for daemon to appear...");
    spawn_verify_daemon_async();
    return NULL;
}

// spawn_start_daemon_async removed: not used

// Verifier: after start attempts fail, wait a short grace period and update last_msg if daemon appears
static void *worker_verify_daemon(void *v) {
    (void)v;
    int started = 0;
    for (int i = 0; i < 40; ++i) { // ~8s (40 * 200ms)
        char *r = send_unix_command("status");
        if (r) { free(r); started = 1; break; }
        sleep_ms(200);
    }
    if (started) {
        set_last_msg("Daemon appeared after start attempts; now running");
    } else {
        set_last_msg("Failed to start daemon (service not found)");
    }
    return NULL;
}

static void spawn_verify_daemon_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_verify_daemon, NULL);
    pthread_attr_destroy(&attr);
}

// Interactive foreground start: suspend ncurses, run systemctl (or sudo) in shell so user can enter password,
// then restore ncurses and spawn verifier to check for daemon socket.
static void interactive_start_daemon(int allow_sudo) {
    set_last_msg("Starting daemon (foreground)...");
    // save/update curses state and return to shell
    def_prog_mode();
    endwin();
    int rc = -1;
    // try user/system service names (support both underscore and hyphen)
    const char *svc_names[] = {"cpu_throttle.service", "cpu-throttle.service"};

    // detect preferred scope/name if installed
    int preferred_scope = 0; // 1=user, 2=system
    const char *preferred_name = NULL;
    for (size_t i = 0; i < sizeof(svc_names)/sizeof(svc_names[0]); ++i) {
        char *const argv_user_status[] = {"systemctl", "--user", "status", (char *)svc_names[i], NULL};
        if (spawn_and_waitvp(argv_user_status) == 0) { preferred_scope = 1; preferred_name = svc_names[i]; break; }
        char *const argv_system_status[] = {"systemctl", "status", (char *)svc_names[i], NULL};
        if (spawn_and_waitvp(argv_system_status) == 0 && preferred_scope == 0) { preferred_scope = 2; preferred_name = svc_names[i]; }
    }

    if (preferred_scope == 1 && preferred_name) {
        char *const argv_user_restart[] = {"systemctl", "--user", "restart", (char *)preferred_name, NULL};
        printf("Running: systemctl --user restart %s\n", preferred_name); fflush(stdout);
        rc = spawn_and_waitvp(argv_user_restart);
    } else if (preferred_scope == 2 && preferred_name) {
        if (geteuid() == 0) {
            char *const argv_system_restart[] = {"systemctl", "restart", (char *)preferred_name, NULL};
            printf("Running: systemctl restart %s\n", preferred_name); fflush(stdout);
            rc = spawn_and_waitvp(argv_system_restart);
        } else if (allow_sudo) {
            char *const argv_sudo_restart[] = {"sudo", "systemctl", "restart", (char *)preferred_name, NULL};
            printf("Running: sudo systemctl restart %s\n", preferred_name); fflush(stdout);
            rc = spawn_and_waitvp(argv_sudo_restart);
        } else {
            rc = -1; // skip privileged attempt
        }
    } else {
        // fallback: try user restart first
        for (size_t si = 0; si < sizeof(svc_names)/sizeof(svc_names[0]); ++si) {
            char *const argv_user_restart[] = {"systemctl", "--user", "restart", (char *)svc_names[si], NULL};
            printf("Running: systemctl --user restart %s\n", svc_names[si]); fflush(stdout);
            rc = spawn_and_waitvp(argv_user_restart);
            if (rc == 0) break;
        }
    }

    // if failed and user allowed sudo, try privileged restart
    if (rc != 0 && allow_sudo) {
        for (size_t si = 0; si < sizeof(svc_names)/sizeof(svc_names[0]); ++si) {
            char *const argv_sudo_restart[] = {"sudo", "systemctl", "restart", (char *)svc_names[si], NULL};
            printf("Running: sudo systemctl restart %s\n", svc_names[si]); fflush(stdout);
            rc = spawn_and_waitvp(argv_sudo_restart);
            if (rc == 0) break;
        }
    }

    // rc contains the exit status of the last command executed
    if (rc == 0) printf("Start command returned success.\n");
    else printf("Start command failed (exit %d)\n", rc);
    printf("Press ENTER to return to the TUI..."); fflush(stdout);
    // wait for user to acknowledge
    int c = getchar(); (void)c;
    // restore curses
    reset_prog_mode(); refresh();
    if (rc == 0) {
        set_last_msg("Start command finished; verifying daemon...");
        spawn_verify_daemon_async();
    } else {
        set_last_msg("Start command failed");
    }
}

static void set_last_msg(const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    pthread_mutex_lock(&last_msg_lock);
    strncpy(last_msg, tmp, sizeof(last_msg)-1);
    last_msg[sizeof(last_msg)-1] = '\0';
    pthread_mutex_unlock(&last_msg_lock);
}

// forward declarations for functions used by worker threads
static int write_profile(const char *name, const char *safe_min, const char *safe_max, const char *temp_max);
static int delete_profile(const char *name);
static int load_profile_by_name(const char *name);

// Send simple command to daemon over unix socket and return response (caller frees)
char* send_unix_command(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }
    // set modest send/recv timeouts so UI threads don't block forever
    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ssize_t w = send(fd, cmd, strlen(cmd), 0);
    (void)w;
    // politely indicate we're done sending so server may close connection
    shutdown(fd, SHUT_WR);
    // read until EOF
    size_t cap = 1024; size_t len = 0;
    char *res = malloc(cap);
    if (!res) { close(fd); return NULL; }
    for (;;) {
        ssize_t r = recv(fd, res + len, cap - len - 1, 0);
        if (r > 0) { len += (size_t)r; if (len + 128 >= cap) { cap *= 2; char *n = realloc(res, cap); if (!n) break; res = n; } }
        else break;
    }
    if (len == 0) { free(res); res = NULL; }
    else res[len] = '\0';
    close(fd);
    return res;
}

// Worker helpers: run a command in a detached thread and store result in last_msg
struct worker_arg { char cmd[512]; };
static void *worker_run_cmd(void *v) {
    struct worker_arg *a = v;
    // special local commands: __create__<name>\t<smin>\t<smax>\t<tmax>  OR delete:<name>
    if (strncmp(a->cmd, "__create__", 10) == 0) {
        char buf[512]; strncpy(buf, a->cmd + 10, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
        char *p = buf;
        char *nm = strtok(p, "\t");
        char *smin = strtok(NULL, "\t");
        char *smax = strtok(NULL, "\t");
        char *tmax = strtok(NULL, "\t");
        int rc = write_profile(nm ? nm : "", (smin && smin[0])?smin:NULL, (smax && smax[0])?smax:NULL, (tmax && tmax[0])?tmax:NULL);
        if (rc == 0) set_last_msg("Profile %s created", nm ? nm : "");
        else set_last_msg("Failed to create %s", nm ? nm : "");
    } else if (strncmp(a->cmd, "delete:", 7) == 0) {
        const char *nm = a->cmd + 7;
        int rc = delete_profile(nm);
        if (rc == 0) set_last_msg("Deleted %s", nm);
        else set_last_msg("Failed to delete %s", nm);
    } else {
        char *r = send_unix_command(a->cmd);
        if (r) {
            set_last_msg("%s", r);
            free(r);
        } else {
            set_last_msg("(no response)");
        }
    }
    free(a);
    return NULL;
}

static void spawn_cmd_async(const char *cmd) {
    struct worker_arg *a = calloc(1, sizeof(*a));
    if (!a) return;
    strncpy(a->cmd, cmd, sizeof(a->cmd)-1);
    pthread_t t;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_run_cmd, a);
    pthread_attr_destroy(&attr);
}

// spawn a worker to fetch status now and update state (non-blocking)
struct status_fetch_arg { int dummy; };
static void *worker_fetch_status(void *v) {
    (void)v;
    char *r = send_unix_command("status");
    pthread_mutex_lock(&state_lock);
    if (r) {
        strncpy(status_buf, r, sizeof(status_buf)-1);
        status_buf[sizeof(status_buf)-1] = '\0';
        free(r);
        status_ts = time(NULL);
        set_last_msg("Status refreshed");
    } else {
        strncpy(status_buf, "(daemon unreachable)", sizeof(status_buf)-1);
        status_buf[sizeof(status_buf)-1] = '\0';
    }
    pthread_mutex_unlock(&state_lock);
    return NULL;
}

static void spawn_fetch_status_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_fetch_status, NULL);
    pthread_attr_destroy(&attr);
}

// spawn a worker to fetch zones now and update state (non-blocking)
static void *worker_fetch_zones(void *v) {
    (void)v;
    char *r = send_unix_command("zones json");
    pthread_mutex_lock(&state_lock);
    if (r) {
        strncpy(zones_buf, r, sizeof(zones_buf)-1);
        zones_buf[sizeof(zones_buf)-1] = '\0';
        free(r);
        zones_ts = time(NULL);
        set_last_msg("Zones refreshed");
    } else {
        strncpy(zones_buf, "(daemon unreachable)", sizeof(zones_buf)-1);
        zones_buf[sizeof(zones_buf)-1] = '\0';
    }
    pthread_mutex_unlock(&state_lock);
    return NULL;
}

static void spawn_fetch_zones_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_fetch_zones, NULL);
    pthread_attr_destroy(&attr);
}

// load-profile worker
struct load_arg { char name[256]; };
// install-skin worker
struct install_arg { char path[512]; };
static void *worker_load_profile(void *v) {
    struct load_arg *a = v;
    int rc = load_profile_by_name(a->name);
    if (rc == 0) set_last_msg("Loaded %s", a->name);
    else set_last_msg("Failed to load %s", a->name);
    free(a);
    return NULL;
}

static void *worker_default_skin(void *v) {
    (void)v;
    // Fetch list-skins json
    char *json = send_unix_command("list-skins json");
    if (!json) {
        set_last_msg("Failed to fetch skins list");
        return NULL;
    }
    // Parse to find active skin
    const char *p = strstr(json, "\"active\":true");
    if (!p) {
        set_last_msg("No active skin to reset");
        free(json);
        return NULL;
    }
    // Find the id for this active skin: go back to find "id":"
    const char *id_pos = NULL;
    for (const char *q = p; q > json; q--) {
        if (strncmp(q, "\"id\":", 5) == 0) {
            id_pos = q + 5;
            break;
        }
    }
    if (!id_pos || *id_pos != '"') {
        set_last_msg("Failed to parse active skin id");
        free(json);
        return NULL;
    }
    const char *id_start = id_pos + 1;
    const char *id_end = strchr(id_start, '"');
    if (!id_end) {
        set_last_msg("Failed to parse active skin id");
        free(json);
        return NULL;
    }
    char id[256];
    size_t len = id_end - id_start;
    if (len >= sizeof(id)) len = sizeof(id) - 1;
    strncpy(id, id_start, len);
    id[len] = '\0';
    free(json);
    // Now deactivate it
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "deactivate-skin %s", id);
    char *resp = send_unix_command(cmd);
    if (resp) {
        if (strstr(resp, "OK:")) {
            set_last_msg("Reset to default skin");
        } else {
            set_last_msg("Failed to reset: %s", resp);
        }
        free(resp);
    } else {
        set_last_msg("Failed to reset skin");
    }
    return NULL;
}

static void *worker_install_skin(void *v) {
    struct install_arg *a = v;
    // Get basename
    const char *base = strrchr(a->path, '/');
    if (!base) base = a->path; else base++;
    // Remove .tar.xz if present
    char name[256];
    strncpy(name, base, sizeof(name)-1);
    name[sizeof(name)-1] = '\0';
    char *dot = strstr(name, ".tar.xz");
    if (dot) *dot = '\0';
    // Open file
    FILE *f = fopen(a->path, "rb");
    if (!f) {
        set_last_msg("Failed to open skin file");
        free(a);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        set_last_msg("Invalid skin file size");
        free(a);
        return NULL;
    }
    // Send header
    char header[512];
    int hlen = snprintf(header, sizeof(header), "put-skin %s %ld\n", name, fsize);
    // Connect and send
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fclose(f);
        set_last_msg("Failed to create socket");
        free(a);
        return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        fclose(f);
        set_last_msg("Failed to connect to daemon");
        free(a);
        return NULL;
    }
    // Send header
    if (send(sock, header, hlen, 0) != hlen) {
        close(sock);
        fclose(f);
        set_last_msg("Failed to send header");
        free(a);
        return NULL;
    }
    // Send file data
    char buf[4096];
    size_t total = 0;
    while (total < (size_t)fsize) {
        size_t toread = sizeof(buf);
        if (total + toread > (size_t)fsize) toread = (size_t)fsize - total;
        size_t n = fread(buf, 1, toread, f);
        if (n == 0) break;
        if (send(sock, buf, n, 0) != (ssize_t)n) {
            close(sock);
            fclose(f);
            set_last_msg("Failed to send file data");
            free(a);
            return NULL;
        }
        total += n;
    }
    fclose(f);
    // Receive response
    char resp[1024];
    ssize_t rlen = recv(sock, resp, sizeof(resp)-1, 0);
    close(sock);
    if (rlen > 0) {
        resp[rlen] = '\0';
        if (strstr(resp, "OK:")) {
            set_last_msg("Installed skin %s", name);
        } else {
            set_last_msg("Failed to install: %s", resp);
        }
    } else {
        set_last_msg("No response from daemon");
    }
    free(a);
    return NULL;
}

static void spawn_load_profile_async(const char *name) {
    struct load_arg *a = calloc(1, sizeof(*a)); if (!a) return;
    strncpy(a->name, name, sizeof(a->name)-1);
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_load_profile, a); pthread_attr_destroy(&attr);
}

static void spawn_default_skin_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_default_skin, NULL); pthread_attr_destroy(&attr);
}

static void spawn_install_skin_async(const char *path) {
    struct install_arg *a = calloc(1, sizeof(*a)); if (!a) return;
    strncpy(a->path, path, sizeof(a->path)-1);
    a->path[sizeof(a->path)-1] = '\0';
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_install_skin, a); pthread_attr_destroy(&attr);
}

const char* get_profile_dir() {
    return "/var/lib/cpu_throttle/profiles";
}

// Ensure profile dir exists
static int ensure_profile_dir(void) {
    const char *dir = get_profile_dir();
    struct stat st;
    if (stat(dir, &st) == 0) return 0;
    // try to create with 0755
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    // create intermediate .config/cpu_throttle if needed
    char base[512]; strncpy(base, tmp, sizeof(base));
    char *p = strstr(base, "/profiles");
    if (p) *p = '\0';
    mkdir(base, 0755);
    return mkdir(dir, 0755);
}

// Read profiles from daemon
int read_profiles(char names[][256], int max) {
    char *r = send_unix_command("list-profiles");
    if (!r) return 0;
    char *line = strtok(r, "\n");
    int i = 0;
    while (line && i < max) {
        // remove trailing \r if any
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';
        if (line[0]) {
            strncpy(names[i], line, 255);
            names[i][255] = '\0';
            i++;
        }
        line = strtok(NULL, "\n");
    }
    free(r);
    return i;
}

// Background poller: periodically fetch status and update status_buf
static void *poller_thread(void *v) {
    (void)v;
    while (keep_running) {
        // Update status (use JSON to include use_avg_temp field)
        char *r = send_unix_command("status json");
        pthread_mutex_lock(&state_lock);
        if (r) {
            strncpy(status_buf, r, sizeof(status_buf)-1);
            status_buf[sizeof(status_buf)-1] = '\0';
            free(r);
            status_ts = time(NULL);
        } else {
            strncpy(status_buf, "(daemon unreachable)", sizeof(status_buf)-1);
            status_buf[sizeof(status_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        // Update limits
        r = send_unix_command("limits json");
        pthread_mutex_lock(&state_lock);
        if (r) {
            strncpy(limits_buf, r, sizeof(limits_buf)-1);
            limits_buf[sizeof(limits_buf)-1] = '\0';
            free(r);
            limits_ts = time(NULL);
        } else {
            strncpy(limits_buf, "(daemon unreachable)", sizeof(limits_buf)-1);
            limits_buf[sizeof(limits_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        // Update zones
        r = send_unix_command("zones json");
        pthread_mutex_lock(&state_lock);
        if (r) {
            strncpy(zones_buf, r, sizeof(zones_buf)-1);
            zones_buf[sizeof(zones_buf)-1] = '\0';
            free(r);
            zones_ts = time(NULL);
        } else {
            strncpy(zones_buf, "(daemon unreachable)", sizeof(zones_buf)-1);
            zones_buf[sizeof(zones_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        // Update skins
        r = send_unix_command("list-skins");
        pthread_mutex_lock(&state_lock);
        if (r) {
            strncpy(skins_buf, r, sizeof(skins_buf)-1);
            skins_buf[sizeof(skins_buf)-1] = '\0';
            free(r);
            skins_ts = time(NULL);
        } else {
            strncpy(skins_buf, "(daemon unreachable)", sizeof(skins_buf)-1);
            skins_buf[sizeof(skins_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        sleep(2); // poll every 2 seconds
    }
    return NULL;
}

/* sparklines disabled */

// Load profile: send load-profile command to daemon
int load_profile_by_name(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "load-profile %s", name);
    char *r = send_unix_command(cmd);
    if (r) free(r);
    return 0; // assume success if no error
}

// Create a profile file with given values
static int write_profile(const char *name, const char *safe_min, const char *safe_max, const char *temp_max) {
    if (!name || name[0] == '\0') return -1;
    ensure_profile_dir();
    char path[512]; snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (safe_min) fprintf(f, "safe_min=%s\n", safe_min);
    if (safe_max) fprintf(f, "safe_max=%s\n", safe_max);
    if (temp_max) fprintf(f, "temp_max=%s\n", temp_max);
    fclose(f);
    return 0;
}

static int delete_profile(const char *name) {
    char path[512]; snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    return unlink(path);
}

int main() {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    // check minimum terminal size (recommended minimum: 80x24)
    int height, width; getmaxyx(stdscr, height, width);
    while (width < 80 || height < 24) {
        // suspend curses and ask user to resize or quit
        endwin();
        fprintf(stderr, "Terminal too small: got %dx%d, need at least 80x24.\n", width, height);
        fprintf(stderr, "Resize terminal and press ENTER to retry, or 'q' then ENTER to quit: "); fflush(stderr);
        char line[16]; if (!fgets(line, sizeof(line), stdin)) return 1;
        if (line[0] == 'q' || line[0] == 'Q') return 1;
        // reinitialize curses and re-measure
        initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
        getmaxyx(stdscr, height, width);
    }
    // init colors (no longer track a use_colors variable)
    if (has_colors()) { start_color(); init_pair(1, COLOR_GREEN, -1); init_pair(2, COLOR_YELLOW, -1); init_pair(3, COLOR_RED, -1); }
    getmaxyx(stdscr, height, width);
    WINDOW *status = newwin(7, width-2, 1, 1);
    WINDOW *data = newwin(height-13, 40, 9, 1);
    // Make help window match the data window height so it can show all lines
    WINDOW *helpwin = newwin(height-13, width-44, 9, 42);
    WINDOW *inputwin = newwin(3, width-2, height-4, 1);
    mvprintw(0,2,"cpu_throttle TUI");
    mvprintw(0,20,"Press 'h' for help | Tab: switch mode");
    refresh();

    // start background poller
    pthread_t poller;
    if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
        // failed to start poller, continue but UI will show no updates
        pthread_mutex_lock(&state_lock); strncpy(status_buf, "(poller failed)", sizeof(status_buf)-1); pthread_mutex_unlock(&state_lock);
    }

    char profs[256][256]; int prof_count = 0; int sel = 0; int offset = 0;
    char display_profs[256][256]; int display_count = 0;
    timeout(200); // responsive UI: 200ms

    // (using prompt_input and prompt_char helpers defined above)

    while (1) {
        werase(status); box(status, 0,0);
        mvwprintw(status, 0,2," Status ");
        // display cached status (updated by poller)
        pthread_mutex_lock(&state_lock);
        char local_status[4096]; strncpy(local_status, status_buf, sizeof(local_status)-1); local_status[sizeof(local_status)-1]='\0';
        time_t ts = status_ts;
        pthread_mutex_unlock(&state_lock);
        char local_msg[256];
        pthread_mutex_lock(&last_msg_lock); strncpy(local_msg, last_msg, sizeof(local_msg)-1); local_msg[sizeof(local_msg)-1]='\0'; pthread_mutex_unlock(&last_msg_lock);
        // print response lines
        int cur_avg = get_use_avg_temp();
        mvwprintw(status, 1, 40, "Avg temp usage: %s", cur_avg < 0 ? "n/a" : (cur_avg ? "true" : "false"));
        char run_user[64] = "";
        if (get_running_user(run_user, sizeof(run_user)) == 0) {
            mvwprintw(status, 2, 40, "Daemon user: %s", run_user);
            if (strcmp(run_user, "root") != 0) {
                mvwprintw(status, 3, 40, "Warning: daemon not running as root; settings may only be saved per user");
            }
        }
        int line=1; char *saveptr = NULL; char *p = strtok_r(local_status, "\n", &saveptr);
        while (p && line < 6) { mvwprintw(status, line, 2, "%s", p); p = strtok_r(NULL, "\n", &saveptr); line++; }
        if (ts != 0) mvwprintw(status,5,2,"Updated: %ld sec ago", (long)(time(NULL)-ts));
        else mvwprintw(status,5,2,"No update yet");
        // (sparklines disabled)
        wrefresh(status);

        // data window - depends on current_mode
        werase(data); box(data,0,0);
        const char *mode_names[] = {"Limits", "Zones", "Skins", "Profiles"};
        mvwprintw(data,0,2," %s ", mode_names[current_mode]);
        int page_lines = (height-13) - 2;
        if (current_mode == 0) { // Limits
            pthread_mutex_lock(&state_lock);
            char local_limits[4096]; strncpy(local_limits, limits_buf, sizeof(local_limits)-1); local_limits[sizeof(local_limits)-1]='\0';
            time_t ts = limits_ts;
            pthread_mutex_unlock(&state_lock);
            char *pretty = format_limits_zones(local_limits, 1);
            int line=1; char *saveptr = NULL; char *p = strtok_r(pretty, "\n", &saveptr);
            while (p && line <= page_lines) { mvwprintw(data, line, 2, "%s", p); p = strtok_r(NULL, "\n", &saveptr); line++; }
            if (ts != 0) mvwprintw(data, page_lines+1, 2, "Updated: %ld sec ago", (long)(time(NULL)-ts));
        } else if (current_mode == 1) { // Zones
            pthread_mutex_lock(&state_lock);
            char local_zones[4096]; strncpy(local_zones, zones_buf, sizeof(local_zones)-1); local_zones[sizeof(local_zones)-1]='\0';
            time_t ts = zones_ts;
            pthread_mutex_unlock(&state_lock);
            zones_count = 0; // reset before parsing
            (void)format_limits_zones(local_zones, 0);
            // Display parsed zones array with interactive selection
            if (zones_count == 0) {
                mvwprintw(data,1,2,"(no zones)");
            } else {
                if (zones_sel < 0) zones_sel = 0;
                if (zones_sel >= zones_count) zones_sel = zones_count - 1;
                if (zones_sel < zones_offset) zones_offset = zones_sel;
                if (zones_sel >= zones_offset + page_lines) zones_offset = zones_sel - page_lines + 1;
                for (int i = zones_offset; i < zones_count && i < zones_offset + page_lines; ++i) {
                    int row = i - zones_offset + 1;
                    if (i == zones_sel) wattron(data, A_REVERSE);
                    mvwprintw(data, row, 2, "Zone %d (%s): %s %d°C", zones_arr[i].zone, zones_arr[i].type, zones_arr[i].excluded ? "Excluded" : "Included", zones_arr[i].temp);
                    if (i == zones_sel) wattroff(data, A_REVERSE);
                }
            }
            if (ts != 0) mvwprintw(data, page_lines+1, 2, "Updated: %ld sec ago", (long)(time(NULL)-ts));
        } else if (current_mode == 2) { // Skins
            pthread_mutex_lock(&state_lock);
            char local_skins[4096]; strncpy(local_skins, skins_buf, sizeof(local_skins)-1); local_skins[sizeof(local_skins)-1]='\0';
            time_t ts = skins_ts;
            pthread_mutex_unlock(&state_lock);
            // parse skins list
            skins_count = 0;
            char *saveptr = NULL; char *p = strtok_r(local_skins, "\n", &saveptr);
            while (p && skins_count < 256) {
                strncpy(skins_list[skins_count], p, 255); skins_list[skins_count][255] = '\0';
                skins_count++;
                p = strtok_r(NULL, "\n", &saveptr);
            }
            if (skins_sel < 0) skins_sel = 0;
            if (skins_sel >= skins_count) skins_sel = skins_count - 1;
            if (skins_sel < 0) skins_sel = 0;
            if (skins_sel < skins_offset) skins_offset = skins_sel;
            if (skins_sel >= skins_offset + page_lines) skins_offset = skins_sel - page_lines + 1;
            if (skins_count == 0) mvwprintw(data,1,2,"(no skins)");
            for (int i=0;i<skins_count && i < skins_offset + page_lines;i++) {
                int row = i - skins_offset + 1;
                if (i==skins_sel) wattron(data, A_REVERSE);
                mvwprintw(data, row, 2, "%s", skins_list[i]);
                if (i==skins_sel) wattroff(data, A_REVERSE);
            }
            mvwprintw(data, page_lines+1, 2, "Skins: %d", skins_count);
            if (ts != 0) mvwprintw(data, page_lines+1, 20, "Updated: %ld sec ago", (long)(time(NULL)-ts));
        } else if (current_mode == 3) { // Profiles
            prof_count = read_profiles((char (*)[256])profs, 256);
            // apply filter into display_profs
            display_count = 0;
            for (int i = 0; i < prof_count; ++i) {
                if (profile_filter[0] == '\0' || strstr(profs[i], profile_filter)) {
                    strncpy(display_profs[display_count], profs[i], 255); display_profs[display_count][255] = '\0'; display_count++;
                }
            }
            if (sel < 0) sel = 0;
            if (sel >= display_count) sel = display_count - 1;
            if (sel < 0) sel = 0;
            if (sel < offset) offset = sel;
            if (sel >= offset + page_lines) offset = sel - page_lines + 1;
            if (display_count == 0) mvwprintw(data,1,2,"(no profiles)");
            for (int i=0;i<display_count && i < offset + page_lines;i++) {
                int row = i - offset + 1;
                if (i==sel) wattron(data, A_REVERSE);
                mvwprintw(data, row, 2, "%s", display_profs[i]);
                if (i==sel) wattroff(data, A_REVERSE);
            }
            mvwprintw(data, page_lines+1, 2, "Profiles: %d (filter: %s)", display_count, profile_filter[0] ? profile_filter : "-");
        }
        wrefresh(data);

        if (help_visible) {
            // paginated help content
            const char *help_lines[] = {
                "h/H/? : toggle help",
                "Tab : switch mode (Limits/Zones/Skins/Profiles)",
                "r : refresh data",
                "UP/DN : select item (in Profiles mode)",
                "/ : filter profiles (in Profiles mode)",
                "g : clear filter (in Profiles mode)",
                "l : load selected profile (in Profiles mode)",
                "c : create profile (in Profiles mode)",
                "e : edit selected profile (in Profiles mode)",
                "d : delete selected profile (in Profiles mode)",
                "s/m/t : set safe-max/safe-min/temp-max (in Limits mode)",
                "i : install skin | d : reset to default | a : activate | u : deactivate (in Skins mode)",
                "UP/DN : select zone (in Zones mode)",
                "x : toggle exclude for selected zone (in Zones mode)",
                "v : toggle use average temp (in Zones mode)",
                "D : stop daemon | S : start daemon",
                "q : quit",
                "Space/PageDown : next page | PageUp/p : previous page",
            };
            int help_count = sizeof(help_lines)/sizeof(help_lines[0]);
            werase(helpwin); box(helpwin,0,0); mvwprintw(helpwin,0,2," Help ");
            (void)getmaxx(helpwin); // intentionally unused; keep call for completeness
            int hh = getmaxy(helpwin);
            int page_lines = hh - 2; // available lines inside box
            if (help_offset < 0) help_offset = 0;
            if (help_offset >= help_count) help_offset = (help_count - 1) / page_lines * page_lines;
            int idx = help_offset;
            int row = 1;
            while (idx < help_count && row <= page_lines) {
                mvwprintw(helpwin, row, 2, "%s", help_lines[idx]);
                idx++; row++;
            }
            // footer hint if more pages
            if (help_offset + page_lines < help_count) mvwprintw(helpwin, hh-1, 2, "-- more: Space/PageDown --");
            else mvwprintw(helpwin, hh-1, 2, "-- end --");
            wrefresh(helpwin);
        } else {
            // footer with last message
            mvprintw(height-3, 2, "Msg: %s", local_msg);
            clrtoeol();
            if (current_mode == 3) { // Profiles
                mvprintw(height-2, 2, "Keys: h help | Tab mode | l load | c create | e edit | d delete | / filter | g clear | D stop | q quit"); clrtoeol();
            } else if (current_mode == 0) { // Limits
                mvprintw(height-2, 2, "Keys: h help | Tab mode | s/m/t set limits | D stop | q quit"); clrtoeol();
            } else if (current_mode == 2) { // Skins
                mvprintw(height-2, 2, "Keys: h help | Tab mode | i install | d default | a activate | u deactivate | D stop | q quit"); clrtoeol();
            } else if (current_mode == 1) { // Zones
                mvprintw(height-2, 2, "Keys: h help | Tab mode | r refresh | UP/DN/nav | x toggle exclude | v toggle avg temp | D stop | q quit"); clrtoeol();
            } else {
                mvprintw(height-2, 2, "Keys: h help | Tab mode | r refresh | D stop | q quit"); clrtoeol();
            }
            refresh();
        }

        int ch = getch();
        if (ch == 'q') { break; }
        else if (ch == '\t') { // Tab to switch mode
            current_mode = (current_mode + 1) % mode_count;
            sel = 0; offset = 0; // reset selection
        }
        else if (ch == 'r') { /* just loop to refresh */ }
        else if (ch == 'h' || ch == 'H' || ch == '?') { help_visible = !help_visible; if (!help_visible) { werase(helpwin); wrefresh(helpwin); } }
        else if (help_visible && (ch == ' ' || ch == KEY_NPAGE || ch == 'n')) {
            // next page
            help_offset += (getmaxy(helpwin) - 2);
            continue;
        } else if (help_visible && (ch == KEY_PPAGE || ch == 'p')) {
            // previous page
            help_offset -= (getmaxy(helpwin) - 2);
            if (help_offset < 0) help_offset = 0;
            continue;
        }
        else if (current_mode == 3 && ch == KEY_UP) { if (sel>0) sel--; }
        else if (current_mode == 3 && ch == KEY_DOWN) { if (sel<display_count-1) sel++; }
        else if (current_mode == 2 && ch == KEY_UP) { if (skins_sel>0) skins_sel--; }
        else if (current_mode == 2 && ch == KEY_DOWN) { if (skins_sel<skins_count-1) skins_sel++; }
        else if (current_mode == 1 && ch == KEY_UP) { if (zones_sel>0) zones_sel--; }
        else if (current_mode == 1 && ch == KEY_DOWN) { if (zones_sel<zones_count-1) zones_sel++; }
        else if (current_mode == 1 && ch == KEY_NPAGE) { int page = getmaxy(data) - 3; if (page < 1) page = 1; zones_sel += page; if (zones_sel >= zones_count) zones_sel = zones_count - 1; }
        else if (current_mode == 1 && ch == KEY_PPAGE) { int page = getmaxy(data) - 3; if (page < 1) page = 1; zones_sel -= page; if (zones_sel < 0) zones_sel = 0; }
        else if (current_mode == 1 && (ch == 'x' || ch == 'X')) {
            if (zones_count == 0) { set_last_msg("No zones to toggle"); }
            else {
                // Determine token to toggle (normalize zone type)
                char token[128]; strncpy(token, zones_arr[zones_sel].type, sizeof(token)-1); token[sizeof(token)-1] = '\0';
                // trim and lowercase
                char *s = token; while (*s && isspace((unsigned char)*s)) s++; char *e = s + strlen(s) - 1; while (e > s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
                for (char *p = s; *p; ++p) *p = tolower((unsigned char)*p);
                if (!s || !*s) { set_last_msg("Empty token" ); continue; }
                // Fetch existing excluded-types CSV from daemon so we don't clobber tokens unknown to this device
                char existing_csv[1024] = "";
                char *resp = send_unix_command("get-excluded-types");
                if (resp) {
                    snprintf(existing_csv, sizeof(existing_csv), "%s", resp);
                    free(resp);
                }
                // Remember previous excluded state
                int was_excluded = zones_arr[zones_sel].excluded;
                // Parse CSV into list and dedupe
                char tokens[64][128]; int token_count = 0;
                if (existing_csv[0]) {
                    char tmp[1024]; strncpy(tmp, existing_csv, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
                    char *tok = strtok(tmp, ",");
                    while (tok) {
                        // trim and lowercase
                        char tk[128]; strncpy(tk, tok, sizeof(tk)-1); tk[sizeof(tk)-1] = '\0';
                        char *tk_s = tk; while (*tk_s && isspace((unsigned char)*tk_s)) tk_s++; char *tk_e = tk_s + strlen(tk_s) - 1; while (tk_e > tk_s && isspace((unsigned char)*tk_e)) { *tk_e = '\0'; tk_e--; }
                        for (char *p = tk_s; *p; ++p) *p = tolower((unsigned char)*p);
                        if (tk_s && *tk_s) {
                            int found = 0; for (int i = 0; i < token_count; ++i) { if (strcmp(tokens[i], tk_s) == 0) { found = 1; break; } }
                            if (!found && token_count < (int)(sizeof(tokens)/sizeof(tokens[0]))) { strncpy(tokens[token_count], tk_s, sizeof(tokens[0])-1); tokens[token_count][sizeof(tokens[0])-1] = '\0'; token_count++; }
                        }
                        tok = strtok(NULL, ",");
                    }
                }
                // check if token is present; toggle: remove if present, add if not
                int present = 0; for (int i = 0; i < token_count; ++i) { if (strcmp(tokens[i], s) == 0) { present = 1; /* remove */ for (int k = i; k + 1 < token_count; ++k) strncpy(tokens[k], tokens[k+1], sizeof(tokens[0])-1); token_count--; break; } }
                if (!present) {
                    if (token_count < (int)(sizeof(tokens)/sizeof(tokens[0]))) { strncpy(tokens[token_count], s, sizeof(tokens[0])-1); tokens[token_count][sizeof(tokens[0])-1] = '\0'; token_count++; }
                }
                else {
                    // removed by exact match; present==1 and token removed above
                }
                // If we were excluded but didn't find an exact token to remove, attempt substring matches
                if (was_excluded && !present) {
                    int removed_any = 0;
                    for (int i = 0; i < token_count; ++i) {
                        if (strstr(s, tokens[i])) {
                            // remove tokens[i]
                            for (int k = i; k + 1 < token_count; ++k) strncpy(tokens[k], tokens[k+1], sizeof(tokens[0])-1);
                            token_count--; i--; removed_any = 1;
                        }
                    }
                    // if we removed substring tokens, update present flag to reflect toggle-off
                    if (removed_any) present = 1; // treat as removal
                }
                // rebuild csv
                char csv[1024] = "";
                for (int i = 0; i < token_count; ++i) {
                    if (i) strncat(csv, ",", sizeof(csv)-strlen(csv)-1);
                    strncat(csv, tokens[i], sizeof(csv)-strlen(csv)-1);
                }
                // apply optimistic UI change to zone
                zones_arr[zones_sel].excluded = present ? 0 : 1;
                // send to daemon
                char unique_types[256][64]; int unique_count = 0;
                char cmd[1536];
                if (csv[0] == '\0') snprintf(cmd, sizeof(cmd), "set-excluded-types none");
                else snprintf(cmd, sizeof(cmd), "set-excluded-types %s", csv);
                spawn_cmd_async(cmd);
                // refresh zones and status so UI shows persisted change and saved value
                spawn_fetch_zones_async();
                spawn_fetch_status_async();
                set_last_msg("Toggled excluded for %s", zones_arr[zones_sel].type);
            }
        }
        else if (current_mode == 1 && (ch == 'v' || ch == 'V')) {
            int cur = get_use_avg_temp();
            if (cur < 0) {
                // Try fetching status synchronously once so we can toggle immediately
                set_last_msg("Refreshing status (please wait)");
                char *r = send_unix_command("status json");
                int retries = 0;
                while (!r && retries < 3) { // try a few times in case of transient failures
                    retries++; sleep_ms(150);
                    r = send_unix_command("status json");
                }
                if (r) {
                    pthread_mutex_lock(&state_lock);
                    strncpy(status_buf, r, sizeof(status_buf)-1);
                    status_buf[sizeof(status_buf)-1] = '\0';
                    free(r);
                    status_ts = time(NULL);
                    pthread_mutex_unlock(&state_lock);
                    // try again to parse use_avg_temp
                    cur = get_use_avg_temp();
                } else {
                    set_last_msg("Failed to get status from daemon");
                    cur = -1;
                }
            }
            if (cur < 0) {
                set_last_msg("Status not available; unable to toggle (daemon unreachable)");
            } else {
                int newv = cur ? 0 : 1;
                char cmd[256]; snprintf(cmd, sizeof(cmd), "set-use-avg-temp %d", newv);
                spawn_cmd_async(cmd);
                // request a quick status refresh so UI shows new value earlier
                spawn_fetch_status_async();
                set_last_msg("set-use-avg-temp %d", newv);
            }
        }
        else if (current_mode == 3 && ch == 'l') {
            if (display_count>0) {
                mvprintw(height-2,2,"Loading profile %s...", display_profs[sel]); clrtoeol(); refresh();
                spawn_load_profile_async(display_profs[sel]);
                set_last_msg("Loading...");
            }
        }
        else if (current_mode == 3 && ch == 'c') {
            help_visible = 0;
            char name[128] = {0};
            if (prompt_input(inputwin, "Create profile name: ", name, sizeof(name)) > 0) {
                char smin[64] = {0}, smax[64] = {0}, tmax[64] = {0};
                prompt_input(inputwin, "safe_min (or empty): ", smin, sizeof(smin));
                prompt_input(inputwin, "safe_max (or empty): ", smax, sizeof(smax));
                prompt_input(inputwin, "temp_max (or empty): ", tmax, sizeof(tmax));
                char cmd[512]; snprintf(cmd, sizeof(cmd), "__create__%s\t%s\t%s\t%s", name, smin, smax, tmax);
                spawn_cmd_async(cmd);
                set_last_msg("Creating profile...");
            } else {
                set_last_msg("Cancelled");
            }
            clrtoeol(); refresh();
        }
        else if (current_mode == 3 && ch == 'e') {
            // edit selected profile inline
            if (display_count > 0) {
                const char *name = display_profs[sel];
                // read existing values
                char path[512]; snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
                char cur_smin[64] = "", cur_smax[64] = "", cur_tmax[64] = "";
                FILE *f = fopen(path, "r");
                if (f) {
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        char key[64], val[64];
                        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
                            if (strcmp(key, "safe_min") == 0) strncpy(cur_smin, val, sizeof(cur_smin)-1);
                            else if (strcmp(key, "safe_max") == 0) strncpy(cur_smax, val, sizeof(cur_smax)-1);
                            else if (strcmp(key, "temp_max") == 0) strncpy(cur_tmax, val, sizeof(cur_tmax)-1);
                        }
                    }
                    fclose(f);
                }
                // prompt edits
                char smin[128], smax[128], tmax[128];
                snprintf(smin, sizeof(smin), "%s", cur_smin[0] ? cur_smin : "(not set)");
                snprintf(smax, sizeof(smax), "%s", cur_smax[0] ? cur_smax : "(not set)");
                snprintf(tmax, sizeof(tmax), "%s", cur_tmax[0] ? cur_tmax : "(not set)");
                char prompt_smin[256]; snprintf(prompt_smin, sizeof(prompt_smin), "safe_min (current: %s): ", cur_smin[0] ? cur_smin : "not set");
                char prompt_smax[256]; snprintf(prompt_smax, sizeof(prompt_smax), "safe_max (current: %s): ", cur_smax[0] ? cur_smax : "not set");
                char prompt_tmax[256]; snprintf(prompt_tmax, sizeof(prompt_tmax), "temp_max (current: %s): ", cur_tmax[0] ? cur_tmax : "not set");
                prompt_input(inputwin, prompt_smin, smin, sizeof(smin));
                prompt_input(inputwin, prompt_smax, smax, sizeof(smax));
                prompt_input(inputwin, prompt_tmax, tmax, sizeof(tmax));
                // write back
                int rc = write_profile(name, (smin[0]?smin:NULL), (smax[0]?smax:NULL), (tmax[0]?tmax:NULL));
                pthread_mutex_lock(&state_lock);
                if (rc == 0) set_last_msg("Saved %s", name);
                else set_last_msg("Failed to save %s", name);
                pthread_mutex_unlock(&state_lock);
            }
        }
        else if (current_mode == 3 && ch == '/') {
            // filter profiles
            char f[128] = {0};
            if (prompt_input(inputwin, "Filter profiles (substring, empty to clear): ", f, sizeof(f)) >= 0) {
                strncpy(profile_filter, f, sizeof(profile_filter)-1);
                profile_filter[sizeof(profile_filter)-1] = '\0';
                sel = 0; offset = 0;
                set_last_msg("Filter applied");
            }
        }
        else if (current_mode == 3 && ch == 'g') {
            profile_filter[0] = '\0'; sel = 0; offset = 0; set_last_msg("Filter cleared");
        }
        else if (current_mode == 3 && ch == 'd') {
            if (display_count>0) {
                help_visible = 0;
                int a = 0;
                size_t need = (size_t)snprintf(NULL, 0, "Delete profile %s? (y/N): ", display_profs[sel]) + 1;
                char *qprompt = malloc(need);
                if (qprompt) {
                    snprintf(qprompt, need, "Delete profile %s? (y/N): ", display_profs[sel]);
                    a = prompt_char(inputwin, qprompt);
                    free(qprompt);
                } else {
                    char qbuf[128]; snprintf(qbuf, sizeof(qbuf), "Delete profile %.100s? (y/N): ", display_profs[sel]);
                    a = prompt_char(inputwin, qbuf);
                }
                if (a == 'y' || a == 'Y') {
                    char cmd[512]; snprintf(cmd, sizeof(cmd), "delete:%s", display_profs[sel]);
                    spawn_cmd_async(cmd);
                    set_last_msg("Deleting...");
                } else {
                    set_last_msg("Cancelled");
                }
                clrtoeol(); refresh();
            }
        }
        else if (current_mode == 2 && ch == 'i') {
            help_visible = 0;
            char path[256] = {0};
            if (prompt_input(inputwin, "Skin archive path: ", path, sizeof(path)) > 0) {
                spawn_install_skin_async(path);
                set_last_msg("Installing skin...");
            } else {
                set_last_msg("Cancelled");
            }
            refresh();
        }
        else if (current_mode == 2 && ch == 'd') {
            spawn_default_skin_async();
            set_last_msg("Resetting to default skin...");
            refresh();
        }
        else if (current_mode == 2 && ch == 'a') {
            if (skins_count > 0) {
                char cmd[512]; snprintf(cmd, sizeof(cmd), "activate-skin %s", skins_list[skins_sel]);
                spawn_cmd_async(cmd);
                set_last_msg("Activating skin %s...", skins_list[skins_sel]);
            } else {
                set_last_msg("No skins to activate");
            }
            refresh();
        }
        else if (current_mode == 2 && ch == 'u') {
            if (skins_count > 0) {
                char cmd[512]; snprintf(cmd, sizeof(cmd), "deactivate-skin %s", skins_list[skins_sel]);
                spawn_cmd_async(cmd);
                set_last_msg("Deactivating skin %s...", skins_list[skins_sel]);
            } else {
                set_last_msg("No skins to deactivate");
            }
            refresh();
        }
        else if (current_mode == 0 && (ch == 's' || ch == 'm' || ch == 't')) {
            help_visible = 0;
            char val[64] = {0};
            if (prompt_input(inputwin, "Enter value: ", val, sizeof(val)) > 0) {
                if (ch == 's') { char cmd[256]; snprintf(cmd,sizeof(cmd),"set-safe-max %s", val); spawn_cmd_async(cmd); }
                if (ch == 'm') { char cmd[256]; snprintf(cmd,sizeof(cmd),"set-safe-min %s", val); spawn_cmd_async(cmd); }
                if (ch == 't') { char cmd[256]; snprintf(cmd,sizeof(cmd),"set-temp-max %s", val); spawn_cmd_async(cmd); }
                set_last_msg("Command sent");
            } else {
                set_last_msg("Cancelled");
            }
            refresh();
        }
        else if (ch == 'D') {
            // confirm quit daemon
            help_visible = 0;
            char qprompt[256]; snprintf(qprompt, sizeof(qprompt), "Stop daemon? (y/N): ");
            int a = prompt_char(inputwin, qprompt);
            if (a == 'y' || a == 'Y') {
                spawn_cmd_async("quit");
                set_last_msg("Sent quit to daemon");
            } else {
                set_last_msg("Cancelled");
            }
            clrtoeol(); refresh();
        }
        else if (ch == 'S') {
            // start daemon interactively in foreground (allows sudo password)
            help_visible = 0;
            int allow_sudo = 0;
            int a = prompt_char(inputwin, "Attempt privileged start if user start fails? (y/N): ");
            if (a == 'y' || a == 'Y') allow_sudo = 1;
            interactive_start_daemon(allow_sudo);
            refresh();
        }

    }

    // stop poller and cleanup windows
    keep_running = 0;
    pthread_join(poller, NULL);
    delwin(status); delwin(data); delwin(helpwin); delwin(inputwin); endwin();
    return 0;
}
