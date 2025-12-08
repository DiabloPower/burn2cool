#include <stdio.h>
#include <stdlib.h>
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

#define SOCKET_PATH "/tmp/cpu_throttle.sock"

// Shared state updated by poller/worker threads
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t last_msg_lock = PTHREAD_MUTEX_INITIALIZER;
static char status_buf[4096] = "(no status yet)";
static time_t status_ts = 0;
static char last_msg[256] = "";
static volatile int keep_running = 1;
static volatile int help_visible = 0;
static int help_offset = 0;
// history for sparkline
/* history removed (sparklines disabled) */
// profile filter
static char profile_filter[128] = "";

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
/* removed spawn_system_async/worker_system helper (unused) */

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
        char chk[256]; snprintf(chk, sizeof(chk), "systemctl --user status %s >/dev/null 2>&1", svc_names[i]);
        if (system(chk) == 0) { preferred_scope = 1; preferred_name = svc_names[i]; break; }
        snprintf(chk, sizeof(chk), "systemctl status %s >/dev/null 2>&1", svc_names[i]);
        if (system(chk) == 0 && preferred_scope == 0) { preferred_scope = 2; preferred_name = svc_names[i]; }
    }

    int rc = -1;
    if (preferred_scope == 1 && preferred_name) {
        char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl --user start %s >/dev/null 2>&1", preferred_name);
        rc = system(cmd);
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
        char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl start %s >/dev/null 2>&1", preferred_name);
        rc = system(cmd);
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
        char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl --user start %s >/dev/null 2>&1", svc_names[i]);
        rc = system(cmd);
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
            char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl start %s >/dev/null 2>&1", svc_names[i]);
            rc = system(cmd);
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
        char chk[128]; snprintf(chk, sizeof(chk), "command -v %s >/dev/null 2>&1", bin_names[bi]);
        rc = system(chk);
        if (rc == 0) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "setsid %s >/dev/null 2>&1 &", bin_names[bi]);
            rc = system(cmd);
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

static void spawn_start_daemon_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_start_daemon, NULL);
    pthread_attr_destroy(&attr);
}

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
        char chk[256]; snprintf(chk, sizeof(chk), "systemctl --user status %s >/dev/null 2>&1", svc_names[i]);
        if (system(chk) == 0) { preferred_scope = 1; preferred_name = svc_names[i]; break; }
        snprintf(chk, sizeof(chk), "systemctl status %s >/dev/null 2>&1", svc_names[i]);
        if (system(chk) == 0 && preferred_scope == 0) { preferred_scope = 2; preferred_name = svc_names[i]; }
    }

    if (preferred_scope == 1 && preferred_name) {
        char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl --user restart %s", preferred_name);
        printf("Running: %s\n", cmd); fflush(stdout);
        rc = system(cmd);
    } else if (preferred_scope == 2 && preferred_name) {
        if (geteuid() == 0) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl restart %s", preferred_name);
            printf("Running: %s\n", cmd); fflush(stdout);
            rc = system(cmd);
        } else if (allow_sudo) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "sudo systemctl restart %s", preferred_name);
            printf("Running: %s\n", cmd); fflush(stdout);
            rc = system(cmd);
        } else {
            rc = -1; // skip privileged attempt
        }
    } else {
        // fallback: try user restart first
        for (size_t si = 0; si < sizeof(svc_names)/sizeof(svc_names[0]); ++si) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "systemctl --user restart %s", svc_names[si]);
            printf("Running: %s\n", cmd); fflush(stdout);
            rc = system(cmd);
            if (rc == 0) break;
        }
    }

    // if failed and user allowed sudo, try privileged restart
    if (rc != 0 && allow_sudo) {
        for (size_t si = 0; si < sizeof(svc_names)/sizeof(svc_names[0]); ++si) {
            char cmd[256]; snprintf(cmd, sizeof(cmd), "sudo systemctl restart %s", svc_names[si]);
            printf("Running: %s\n", cmd); fflush(stdout);
            rc = system(cmd);
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

// load-profile worker
struct load_arg { char name[256]; };
static void *worker_load_profile(void *v) {
    struct load_arg *a = v;
    int rc = load_profile_by_name(a->name);
    if (rc == 0) set_last_msg("Loaded %s", a->name);
    else set_last_msg("Failed to load %s", a->name);
    free(a);
    return NULL;
}

static void spawn_load_profile_async(const char *name) {
    struct load_arg *a = calloc(1, sizeof(*a)); if (!a) return;
    strncpy(a->name, name, sizeof(a->name)-1);
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_load_profile, a); pthread_attr_destroy(&attr);
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
        char *r = send_unix_command("status");
        pthread_mutex_lock(&state_lock);
        if (r) {
            strncpy(status_buf, r, sizeof(status_buf)-1);
            status_buf[sizeof(status_buf)-1] = '\0';
            free(r);
            status_ts = time(NULL);
            /* sparkline parsing removed */
        } else {
            strncpy(status_buf, "(daemon unreachable)", sizeof(status_buf)-1);
            status_buf[sizeof(status_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);
        sleep(1);
    }
    return NULL;
}

/* sparklines removed */

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
    char path[512]; snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (safe_min) fprintf(f, "safe_min=%s\n", safe_min);
    if (safe_max) fprintf(f, "safe_max=%s\n", safe_max);
    if (temp_max) fprintf(f, "temp_max=%s\n", temp_max);
    fclose(f);
    return 0;
}

static int delete_profile(const char *name) {
    char path[512]; snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
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
    WINDOW *profiles = newwin(height-13, 40, 9, 1);
    // Make help window match the profiles window height so it can show all lines
    WINDOW *helpwin = newwin(height-13, width-44, 9, 42);
    WINDOW *inputwin = newwin(3, width-2, height-4, 1);
    mvprintw(0,2,"cpu_throttle TUI");
    mvprintw(0,20,"Press 'h' for help");
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
        int line=1; char *saveptr = NULL; char *p = strtok_r(local_status, "\n", &saveptr);
        while (p && line < 6) { mvwprintw(status, line, 2, "%s", p); p = strtok_r(NULL, "\n", &saveptr); line++; }
        if (ts != 0) mvwprintw(status,5,2,"Updated: %ld sec ago", (long)(time(NULL)-ts));
        else mvwprintw(status,5,2,"No update yet");
        // (sparklines disabled)
        wrefresh(status);

        // profiles
        werase(profiles); box(profiles,0,0); mvwprintw(profiles,0,2," Profiles ");
        prof_count = read_profiles((char (*)[256])profs, 256);
        // apply filter into display_profs
        display_count = 0;
        for (int i = 0; i < prof_count; ++i) {
            if (profile_filter[0] == '\0' || strstr(profs[i], profile_filter)) {
                strncpy(display_profs[display_count], profs[i], 255); display_profs[display_count][255] = '\0'; display_count++;
            }
        }
        int page_lines = (height-13) - 2;
        if (sel < 0) sel = 0;
        if (sel >= display_count) sel = display_count - 1;
        if (sel < 0) sel = 0;
        if (sel < offset) offset = sel;
        if (sel >= offset + page_lines) offset = sel - page_lines + 1;
        if (display_count == 0) mvwprintw(profiles,1,2,"(no profiles)");
        for (int i=0;i<display_count && i < offset + page_lines;i++) {
            int row = i - offset + 1;
            if (i==sel) wattron(profiles, A_REVERSE);
            mvwprintw(profiles, row, 2, "%s", display_profs[i]);
            if (i==sel) wattroff(profiles, A_REVERSE);
        }
        mvwprintw(profiles, page_lines+1, 2, "Profiles: %d (filter: %s)", display_count, profile_filter[0] ? profile_filter : "-");
        wrefresh(profiles);

        if (help_visible) {
            // paginated help content
            const char *help_lines[] = {
                "h/H/? : toggle help",
                "r : refresh status",
                "UP/DN : select profile",
                "/ : filter profiles (enter substring)",
                "g : clear filter",
                "l : load selected profile",
                "c : create profile (prompt)",
                "e : edit selected profile",
                "d : delete selected profile",
                "s/m/t : set safe-max/safe-min/temp-max",
                "D : stop daemon | S : start daemon",
                "q : quit",
                "Space/PageDown : next page | PageUp/p : previous page",
            };
            int help_count = sizeof(help_lines)/sizeof(help_lines[0]);
            werase(helpwin); box(helpwin,0,0); mvwprintw(helpwin,0,2," Help ");
            int hw = getmaxx(helpwin);
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
            mvprintw(height-2, 2, "Keys: h help | l load | c create | e edit | d delete | / filter | g clear | s/m/t set | D stop | q quit"); clrtoeol();
            refresh();
        }

        int ch = getch();
        if (ch == 'q') { break; }
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
        else if (ch == KEY_UP) { if (sel>0) sel--; }
        else if (ch == KEY_DOWN) { if (sel<prof_count-1) sel++; }
        else if (ch == 'l') {
            if (display_count>0) {
                mvprintw(height-2,2,"Loading profile %s...", display_profs[sel]); clrtoeol(); refresh();
                spawn_load_profile_async(display_profs[sel]);
                set_last_msg("Loading...");
            }
        }
        else if (ch == 'c') {
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
        else if (ch == 'e') {
            // edit selected profile inline
            if (display_count > 0) {
                const char *name = display_profs[sel];
                // read existing values
                char path[512]; snprintf(path, sizeof(path), "%s/%s", get_profile_dir(), name);
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
                char smin[64], smax[64], tmax[64];
                snprintf(smin, sizeof(smin), "%s", cur_smin);
                snprintf(smax, sizeof(smax), "%s", cur_smax);
                snprintf(tmax, sizeof(tmax), "%s", cur_tmax);
                prompt_input(inputwin, "safe_min (blank to keep): ", smin, sizeof(smin));
                prompt_input(inputwin, "safe_max (blank to keep): ", smax, sizeof(smax));
                prompt_input(inputwin, "temp_max (blank to keep): ", tmax, sizeof(tmax));
                // write back
                int rc = write_profile(name, (smin[0]?smin:NULL), (smax[0]?smax:NULL), (tmax[0]?tmax:NULL));
                pthread_mutex_lock(&state_lock);
                if (rc == 0) set_last_msg("Saved %s", name);
                else set_last_msg("Failed to save %s", name);
                pthread_mutex_unlock(&state_lock);
            }
        }
        else if (ch == '/') {
            // filter profiles
            char f[128] = {0};
            if (prompt_input(inputwin, "Filter profiles (substring, empty to clear): ", f, sizeof(f)) >= 0) {
                strncpy(profile_filter, f, sizeof(profile_filter)-1);
                profile_filter[sizeof(profile_filter)-1] = '\0';
                sel = 0; offset = 0;
                set_last_msg("Filter applied");
            }
        }
        else if (ch == 'g') {
            profile_filter[0] = '\0'; sel = 0; offset = 0; set_last_msg("Filter cleared");
        }
        else if (ch == 'd') {
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
        else if (ch == 's' || ch == 'm' || ch == 't') {
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
    delwin(status); delwin(profiles); delwin(helpwin); delwin(inputwin); endwin();
    return 0;
}
