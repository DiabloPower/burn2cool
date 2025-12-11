//#include <ftw.h> // not using nftw; keep code portable
// (qsort comparator declared lower near zone_entry_t definition)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <libgen.h>
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
// Additional runtime config path for system use
#define VARLIB_CONFIG "/var/lib/cpu_throttle/cpu_throttle.conf"
#define DEFAULT_WEB_PORT 8086  // Intel 8086 tribute!
#define DAEMON_VERSION "4.0"
#define THROTTLE_START_OFFSET 30  // Start throttling 30°C below temp_max
#define HYSTERESIS 3              // °C hysteresis
#define POLL_TIMEOUT_MS 250       // Poll timeout in ms
#define TEMP_READ_INTERVAL_MS 1000 // Temp read interval in ms
#define MAX_LOG_SIZE (10 * 1024 * 1024) // 10 MB max log size

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
char **cpu_freq_paths = NULL; // cached paths to CPU scaling_max_freq files
int num_cpus = 0; // number of CPUs
volatile sig_atomic_t should_exit = 0; // flag for graceful shutdown
volatile sig_atomic_t should_restart = 0; // request restart by exec-ing self
int thermal_zone = -1; // thermal zone number (-1 = auto-detect, prefer zone 0 if CPU)
int use_avg_temp = 0; // use average temperature from CPU thermal zones
int last_throttle_temp = 0; // hysteresis for throttling

/* System-wide skins directory. Skins are installed system-wide by installer or
 * manually by an administrator. Each subfolder is one skin (id = folder name). */
const char *SKINS_DIR = "/usr/local/share/burn2cool/skins";
/* Assets directory: default path to serve static assets. */
const char *ASSETS_DIR = "assets";

/* Active skin id (matches a subdirectory name under SKINS_DIR). Empty string
 * means use default embedded assets. */
char active_skin[256] = "";

// Forward declaration for helper normalizer
static void normalize_excluded_types(char *out, size_t out_sz, const char *in);

// Zone entry representation for JSON generation and sorting
typedef struct zone_entry { int zone_num; char type[256]; int temp_c; } zone_entry_t;

// Compare function for qsort
static int zone_entry_cmp(const void *a, const void *b) {
    const zone_entry_t *za = (const zone_entry_t*)a;
    const zone_entry_t *zb = (const zone_entry_t*)b;
    if (za->zone_num < zb->zone_num) return -1;
    if (za->zone_num > zb->zone_num) return 1;
    return 0;
}

// Current state for API responses
int current_temp = 0;
int current_freq = 0;
int cpu_min_freq = 0;
int cpu_max_freq = 0;
char **saved_argv = NULL;

#if defined(__has_include)
    #if __has_include("include/assets_generated.h")
        /* Optional generated asset headers. Run `make assets` to generate headers in
         * `include/` and this file `include/assets_generated.h` will define
         * `USE_ASSET_HEADERS` so the server will prefer the generated binary arrays.
         */
        #include "include/assets_generated.h"
    #endif
#else
    /* Fallback for compilers without __has_include: still try to include the header
     * if present. The build will fail if the header is missing when the Makefile
     * expects it. */
    #include "include/assets_generated.h"
#endif
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

#else /* !USE_ASSET_HEADERS */
#/**************************************************************/

/* forward declaration for send_http_response_len to avoid implicit declaration
 * when file-serving helper is compiled earlier than the response function. */
void send_http_response_len(int client_fd, const char *status, const char *content_type, const void *body, size_t len, const char *extra_headers);
/* forward declaration for guess_mime for use by serve_file */
static const char *guess_mime(const char *path);
/* forward declaration for parse_skin_manifest used by installer install helper */
/* forward declaration for parse_skin_manifest used by installer install helper: will be declared unconditionally below */

#endif /* USE_ASSET_HEADERS */

/* Ensure send_http_response_len is declared even when assets are embedded. */
void send_http_response_len(int client_fd, const char *status, const char *content_type, const void *body, size_t len, const char *extra_headers);
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

// Logging macros (use __VA_ARGS__ form to satisfy pedantic compilers)
#define LOG_ERROR(...) do { if (log_level >= LOGLEVEL_QUIET) fprintf(stderr, __VA_ARGS__); } while(0)
#define LOG_INFO(...) do { if (log_level >= LOGLEVEL_NORMAL) printf(__VA_ARGS__); } while(0)
#define LOG_VERBOSE(...) do { if (log_level >= LOGLEVEL_VERBOSE) printf(__VA_ARGS__); } while(0)

// Rotate log file if it exceeds MAX_LOG_SIZE
static void rotate_log_file(const char *log_path) {
    if (!log_path) return;
    struct stat st;
    if (stat(log_path, &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        char old_path[512];
        snprintf(old_path, sizeof(old_path), "%s.old", log_path);
        rename(log_path, old_path);
        LOG_INFO("Log file rotated: %s -> %s\n", log_path, old_path);
    }
}

void signal_handler(int sig) {
    (void)sig;
    should_exit = 1;
}

/* forward declaration for parse_skin_manifest used across the daemon */
static void parse_skin_manifest(const char *manifest_path, char *out_id, size_t id_sz, char *out_name, size_t name_sz, int *allow_extra_js);

// Helper: recursively remove a directory tree using nftw (safe, no shell required)
static int remove_path_recursive(const char *path);

// Implement a recursive directory removal function without nftw
static int remove_dir_recursive_impl(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *entry;
    int rv = 0;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024]; snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat st; if (lstat(child, &st) != 0) { rv = -1; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (remove_dir_recursive_impl(child) != 0) rv = -1;
            if (rmdir(child) != 0) { LOG_ERROR("remove_dir_recursive_impl: rmdir failed %s: %s\n", child, strerror(errno)); rv = -1; }
        } else {
            if (unlink(child) != 0) { LOG_ERROR("remove_dir_recursive_impl: unlink failed %s: %s\n", child, strerror(errno)); rv = -1; }
        }
    }
    closedir(d);
    return rv;
}

static int remove_path_recursive(const char *path) {
    struct stat st; if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        // remove contents, then rmdir the dir itself
        int r = remove_dir_recursive_impl(path);
        if (r != 0) return -1;
        if (rmdir(path) != 0) { LOG_ERROR("remove_path_recursive: rmdir failed %s: %s\n", path, strerror(errno)); return -1; }
        return 0;
    } else {
        if (unlink(path) != 0) return -1; 
        return 0;
    }
}

/* spawn_and_waitvp and command_exists_in_path are provided by the TUI binary; daemon does not need them */

// Helper: find a file named `name` in `dir` up to max_depth and return path
static int find_file_in_dir(const char *dir, const char *name, int max_depth, char *out_path, size_t out_sz) {
    if (!dir || !name || !out_path) return -1;
    if (max_depth < 0) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof(p), "%s/%s", dir, entry->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (S_ISREG(st.st_mode) && strcmp(entry->d_name, name) == 0) {
            snprintf(out_path, out_sz, "%s", p);
            closedir(d);
            return 0;
        }
        if (S_ISDIR(st.st_mode) && max_depth > 0) {
            if (find_file_in_dir(p, name, max_depth - 1, out_path, out_sz) == 0) { closedir(d); return 0; }
        }
    }
    closedir(d);
    return -1;
}

/* Read a file from disk and return allocated buffer (caller must free()) */
static char *read_file_alloc(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t r = fread(buf, 1, len, fp);
    fclose(fp);
    if (r != (size_t)len) { free(buf); return NULL; }
    buf[r] = '\0';
    if (out_len) *out_len = r;
    return buf;
}

static const char *guess_mime(const char *path){
    size_t l = strlen(path);
    if (l >= 5 && strcmp(path + l - 5, ".html") == 0) return "text/html";
    if (l >= 4 && strcmp(path + l - 4, ".css") == 0) return "text/css";
    if (l >= 3 && strcmp(path + l - 3, ".js") == 0) return "application/javascript";
    if (l >= 4 && strcmp(path + l - 4, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

/* Portable memmem implementation for platforms that do not have glibc memmem.
 * This is a naive implementation suitable for small assets (index.html) and
 * avoids relying on non-portable functions on non-glibc systems. */
#ifdef USE_ASSET_HEADERS
static void *memmem_shim(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len) {
    if (!haystack || !needle) return NULL;
    const unsigned char *h = haystack;
    const unsigned char *n = needle;
    if (needle_len == 0) return (void *)haystack;
    if (haystack_len < needle_len) return NULL;
    size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; ++i) {
        if (h[i] == n[0] && memcmp(&h[i], n, needle_len) == 0) return (void *)&h[i];
    }
    return NULL;
}
#endif

/* Read a file from disk and send as HTTP response (content length known).
 * Returns 1 on success (served), 0 if not found, -1 on error. */
static int serve_file(int client_fd, const char *path) {
    size_t len; char *buf = read_file_alloc(path, &len);
    if (!buf) return 0;
    const char *mime = guess_mime(path);
    send_http_response_len(client_fd, "200 OK", mime, buf, len, NULL);
    free(buf);
    return 1;
}

static int skin_has_file(const char *skin_id, const char *relpath) {
    if (!skin_id || !skin_id[0]) return 0;
    char path[1024];
    const char *r = relpath[0] == '/' ? relpath + 1 : relpath;
    snprintf(path, sizeof(path), "%s/%s/%s", SKINS_DIR, skin_id, r);
    struct stat st; return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* Serve HTML from disk/path and inject skin extra.js script if the active
 * skin has extra.js. */
static int serve_file_with_skin_extra(int client_fd, const char *path, const char *skin_id) {
    size_t len; char *buf = read_file_alloc(path, &len);
    if (!buf) return 0;
    // Inject skin CSS and extra.js when serving default index so skins can style
    // the existing UI without providing a complete index.html.
    int inject_css = 0;
    if (skin_id && skin_id[0] && skin_has_file(skin_id, "styles.css")) inject_css = 1;
    if (skin_id && skin_id[0] && skin_has_file(skin_id, "extra.js")) {
        char injection[512];
        snprintf(injection, sizeof(injection), "<script src=\"/skins/%s/extra.js\"></script>", skin_id);
        // Find last </body> occurrence to inject before
        char *pos = NULL; char *p = strstr(buf, "</body>");
        if (p) pos = p; else pos = NULL;
            if (pos) {
            size_t injlen = strlen(injection);
            size_t newlen = len + injlen;
            char *nb = malloc(newlen + 1);
                if (nb) {
                size_t prefix = pos - buf;
                memcpy(nb, buf, prefix);
                memcpy(nb + prefix, injection, injlen);
                memcpy(nb + prefix + injlen, pos, len - prefix);
                nb[newlen] = '\0';
                send_http_response_len(client_fd, "200 OK", "text/html", nb, newlen, NULL);
                free(nb);
                free(buf);
                return 1;
            }
        }
    }
    // CSS injection: insert <link rel="stylesheet" href="/skins/%s/styles.css"> inside <head>
    if (inject_css) {
        char css_inj[256]; snprintf(css_inj, sizeof(css_inj), "<link rel=\"stylesheet\" href=\"/skins/%s/styles.css\">", skin_id);
        char *headpos = strstr(buf, "</head>");
        if (headpos) {
            size_t prefix = headpos - buf;
            size_t injlen = strlen(css_inj);
            size_t newlen = len + injlen;
            char *nb = malloc(newlen + 1);
            if (nb) {
                memcpy(nb, buf, prefix);
                memcpy(nb + prefix, css_inj, injlen);
                memcpy(nb + prefix + injlen, headpos, len - prefix);
                nb[newlen] = '\0';
                send_http_response_len(client_fd, "200 OK", "text/html", nb, newlen, NULL);
                free(nb);
                free(buf);
                return 1;
            }
        }
    }
    // No injection or failed, send raw
    const char *mime = guess_mime(path);
    send_http_response_len(client_fd, "200 OK", mime, buf, len, NULL);
    free(buf);
    return 1;
}

/* Serve an asset from disk (path relative to ASSETS_DIR). Returns 1 if served,
 * 0 if not found, -1 on error. */
static int serve_asset_from_disk(int client_fd, const char *relpath) __attribute__((unused));
static int serve_asset_from_disk(int client_fd, const char *relpath) {
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", ASSETS_DIR, relpath);
    return serve_file(client_fd, path);
}

/* Serve an asset from the active skin if present; returns 1 if served, 0
 * if not found. */
static int serve_skin_asset(int client_fd, const char *skin_id, const char *relpath) {
    if (!skin_id || !skin_id[0]) return 0;
    char path[1024];
    // remove leading slash if present in relpath
    const char *r = relpath[0] == '/' ? relpath + 1 : relpath;
    snprintf(path, sizeof(path), "%s/%s/%s", SKINS_DIR, skin_id, r);
    LOG_VERBOSE("serve_skin_asset: trying path=%s\n", path);
    int rc = serve_file(client_fd, path);
    LOG_VERBOSE("serve_skin_asset: rc=%d\n", rc);
    return rc;
}

// Return 1 if skin's index.html appears to be a full dashboard replacement.
// We detect this by checking for a few known DOM markers from the default UI.
static int skin_index_is_full(const char *skin_id) {
    if (!skin_id || !skin_id[0]) return 0;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s/index.html", SKINS_DIR, skin_id);
    size_t len; char *buf = read_file_alloc(path, &len);
    if (!buf) return 0;
    int full = 0;
    // Look for default UI markers: topbar, cards-grid, or title h1
    if (strstr(buf, "class='topbar'") || strstr(buf, "class=\"topbar\"") || strstr(buf, "class='cards-grid'") || strstr(buf, "class=\"cards-grid\"") || strstr(buf, "<h1>") || strstr(buf, "class='status-card'")) {
        full = 1;
    }
    free(buf);
    return full;
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

// Forward declarations
static void cache_cpu_freq_paths(void);

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

char excluded_types_config[512] = "int3400,int3402,int3403,int3404,int3405,int3406,int3407";

static void parse_config_fp(FILE *fp) {
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
                int val = atoi(value);
                if (val >= 50 && val <= 110) {
                    temp_max = val;
                    LOG_VERBOSE("Config: temp_max = %d\n", temp_max);
                } else {
                    LOG_VERBOSE("Config: temp_max %d out of range (50-110), using default 95\n", val);
                }
            } else if (strcmp(key, "safe_min") == 0) {
                safe_min = atoi(value);
                LOG_VERBOSE("Config: safe_min = %d\n", safe_min);
            } else if (strcmp(key, "safe_max") == 0) {
                safe_max = atoi(value);
                LOG_VERBOSE("Config: safe_max = %d\n", safe_max);
            } else if (strcmp(key, "sensor") == 0) {
                snprintf(temp_path, sizeof(temp_path), "%s", value);
                temp_path[sizeof(temp_path) - 1] = '\0';
                LOG_VERBOSE("Config: sensor = %s\n", temp_path);
            } else if (strcmp(key, "thermal_zone") == 0) {
                int val = atoi(value);
                if (val >= -1 && val <= 100) {
                    thermal_zone = val;
                    LOG_VERBOSE("Config: thermal_zone = %d\n", thermal_zone);
                } else {
                    LOG_VERBOSE("Config: thermal_zone %d out of range (-1-100), using default -1\n", val);
                }
            } else if (strcmp(key, "avg_temp") == 0) {
                int val = atoi(value);
                if (val == 0 || val == 1) {
                    use_avg_temp = val;
                    LOG_VERBOSE("Config: avg_temp = %d\n", use_avg_temp);
                } else {
                    LOG_VERBOSE("Config: avg_temp %d invalid (0 or 1), using default 0\n", val);
                }
            } else if (strcmp(key, "web_port") == 0) {
                int val = atoi(value);
                if (val == 0 || (val >= 1024 && val <= 65535)) {
                    web_port = val;
                    LOG_VERBOSE("Config: web_port = %d\n", web_port);
                } else {
                    LOG_VERBOSE("Config: web_port %d out of range (0 or 1024-65535), using default 0\n", val);
                }
            } else if (strcmp(key, "skin") == 0) {
                snprintf(active_skin, sizeof(active_skin), "%s", value);
                LOG_VERBOSE("Config: active skin = %s\n", active_skin);
            } else if (strcmp(key, "excluded_types") == 0) {
                snprintf(excluded_types_config, sizeof(excluded_types_config), "%s", value);
                LOG_VERBOSE("Config: excluded_types = %s\n", excluded_types_config);
            } else {
                LOG_VERBOSE("Config: Unknown key '%s' at line %d\n", key, line_num);
            }
        }
    }
}

void load_config_file() {
    // Try to load system config first
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp) {
        parse_config_fp(fp);
        fclose(fp);
    } else {
        LOG_VERBOSE("No config file found at %s, using defaults\n", CONFIG_FILE);
    }

    // Try runtime override (e.g., /var/lib)
    FILE *fp2 = fopen(VARLIB_CONFIG, "r");
    if (fp2) {
        LOG_VERBOSE("Loading runtime config override: %s\n", VARLIB_CONFIG);
        parse_config_fp(fp2);
        fclose(fp2);
    }

    // Try loading user config override (~/.config/cpu_throttle.conf)
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (home) {
        char usercfg[512];
        snprintf(usercfg, sizeof(usercfg), "%s/.config/cpu_throttle.conf", home);
        FILE *fpu = fopen(usercfg, "r");
        if (fpu) {
            LOG_VERBOSE("Loading user config override: %s\n", usercfg);
            parse_config_fp(fpu);
            fclose(fpu);
        }
    }
    // Normalize excluded_types after loading to ensure consistent format
    if (excluded_types_config[0]) {
        char normalized[512]; normalized[0] = '\0';
        normalize_excluded_types(normalized, sizeof(normalized), excluded_types_config);
        snprintf(excluded_types_config, sizeof(excluded_types_config), "%s", normalized);
    }
}

static char saved_config_path[512] = "";
// Forward declaration
static void normalize_excluded_types(char *out, size_t out_sz, const char *in);

int save_config_file() {
    FILE *fp = fopen(CONFIG_FILE, "w");
    if (!fp) {
        LOG_ERROR("Failed to open config file for writing: %s, trying user config\n", CONFIG_FILE);
        // If we're running as root, try to write to runtime /var/lib path as fallback
        if (geteuid() == 0) {
            // ensure /var/lib/cpu_throttle exists
            mkdir("/var/lib/cpu_throttle", 0755);
            FILE *fpvar = fopen(VARLIB_CONFIG, "w");
            if (fpvar) {
                fprintf(fpvar, "# CPU Throttle Configuration (runtime)\n");
                fprintf(fpvar, "temp_max=%d\n", temp_max);
                fprintf(fpvar, "safe_min=%d\n", safe_min);
                fprintf(fpvar, "safe_max=%d\n", safe_max);
                fprintf(fpvar, "sensor=%s\n", temp_path);
                fprintf(fpvar, "thermal_zone=%d\n", thermal_zone);
                fprintf(fpvar, "avg_temp=%d\n", use_avg_temp);
                fprintf(fpvar, "web_port=%d\n", web_port);
                fprintf(fpvar, "skin=%s\n", active_skin);
                fprintf(fpvar, "excluded_types=%s\n", excluded_types_config);
                fclose(fpvar);
                LOG_INFO("Configuration saved to runtime config %s\n", VARLIB_CONFIG);
                strncpy(saved_config_path, VARLIB_CONFIG, sizeof(saved_config_path)-1);
                saved_config_path[sizeof(saved_config_path)-1] = '\0';
                return 0;
            }
        }
        // try to write user config instead
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
        if (home) {
            char usercfg[512];
            snprintf(usercfg, sizeof(usercfg), "%s/.config/cpu_throttle.conf", home);
            fp = fopen(usercfg, "w");
            if (!fp) {
                LOG_ERROR("Failed to open user config for writing: %s\n", usercfg);
                return -1;
            }
            fprintf(fp, "# CPU Throttle Configuration (user override)\n");
            fprintf(fp, "temp_max=%d\n", temp_max);
            fprintf(fp, "safe_min=%d\n", safe_min);
            fprintf(fp, "safe_max=%d\n", safe_max);
            fprintf(fp, "sensor=%s\n", temp_path);
            fprintf(fp, "thermal_zone=%d\n", thermal_zone);
            fprintf(fp, "avg_temp=%d\n", use_avg_temp);
            fprintf(fp, "web_port=%d\n", web_port);
            fprintf(fp, "skin=%s\n", active_skin);
            fprintf(fp, "excluded_types=%s\n", excluded_types_config);
            fclose(fp);
            LOG_INFO("Configuration saved to user config %s\n", usercfg);
            strncpy(saved_config_path, usercfg, sizeof(saved_config_path)-1);
            saved_config_path[sizeof(saved_config_path)-1] = '\0';
            return 0;
        }
        return -1;
    }
    
    fprintf(fp, "# CPU Throttle Configuration\n");
    fprintf(fp, "temp_max=%d\n", temp_max);
    fprintf(fp, "safe_min=%d\n", safe_min);
    fprintf(fp, "safe_max=%d\n", safe_max);
    fprintf(fp, "sensor=%s\n", temp_path);
    fprintf(fp, "thermal_zone=%d\n", thermal_zone);
    fprintf(fp, "avg_temp=%d\n", use_avg_temp);
    fprintf(fp, "web_port=%d\n", web_port);
    fprintf(fp, "skin=%s\n", active_skin);
    fprintf(fp, "excluded_types=%s\n", excluded_types_config);
    
    fclose(fp);
    LOG_INFO("Configuration saved to %s\n", CONFIG_FILE);
    strncpy(saved_config_path, CONFIG_FILE, sizeof(saved_config_path)-1);
    saved_config_path[sizeof(saved_config_path)-1] = '\0';
    return 0;
}

// Check whether a skin directory exists under SKINS_DIR
static int skin_exists(const char *id) {
    if (!id || !id[0]) return 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", SKINS_DIR, id);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    return 0;
}

// Return 1 if skin has allow_extra_js true in manifest
static int skin_allows_extra_js(const char *id) {
    if (!id || !id[0]) return 0;
    char mpath[1024]; snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", SKINS_DIR, id);
    FILE *fp = fopen(mpath, "r"); if (!fp) return 0;
    char buf[4096]; size_t r = fread(buf, 1, sizeof(buf)-1, fp); buf[r] = '\0'; fclose(fp);
    char *p = strstr(buf, "\"allow_extra_js\""); if (!p) return 0;
    char *q = strchr(p, ':'); if (!q) return 0; while (*q && (*q == ':' || isspace((unsigned char)*q))) q++;
    if (strncmp(q, "true", 4) == 0) return 1; else return 0;
}

/* Install a skin archive from a temporary path. This performs a secure extraction using
 * execvp to avoid shell injection, and copies the extracted content into SKINS_DIR/<id>.
 * The caller must enforce admin constraints and check request authentication as needed. */
static int install_skin_archive_from_file(const char *archive_path, char *out_id, size_t id_sz) {
    if (!archive_path || !out_id) return -1;
    out_id[0] = '\0';

    // Validate archive_path: only allow safe characters to prevent injection
    for (const char *p = archive_path; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '/' && *p != '.' && *p != '-' && *p != '_') {
            LOG_ERROR("Invalid characters in archive path: %s\n", archive_path);
            return -1;
        }
        }

    char staging_template[] = "/tmp/burn2cool_skin_XXXXXX";
    char *staging = mkdtemp(staging_template);
    if (!staging) return -1;
    // Determine archive type by magic
    FILE *af = fopen(archive_path, "rb");
    if (!af) { rmdir(staging); return -1; }
    unsigned char sig[4]; size_t r = fread(sig, 1, sizeof(sig), af); fclose(af);
    int use_tar = 0, use_zip = 0;
    if (r >= 2 && sig[0] == 0x1F && sig[1] == 0x8B) use_tar = 1; // gzip
    if (r >= 2 && sig[0] == 'P' && sig[1] == 'K') use_zip = 1; // zip

    int rc = -1;
    // Try common extraction patterns: gzip tar, plain tar, unzip using execvp for security
    if (use_tar) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("tar", "tar", "-xzf", archive_path, "-C", staging, NULL);
            _exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            rc = -1;
        }
        if (rc != 0) {
            // try plain tar (uncompressed)
            pid_t pid2 = fork();
            if (pid2 == 0) {
                execlp("tar", "tar", "-xf", archive_path, "-C", staging, NULL);
                _exit(1);
            } else if (pid2 > 0) {
                int status;
                waitpid(pid2, &status, 0);
                rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            } else {
                rc = -1;
            }
        }
    } else if (use_zip) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("unzip", "unzip", "-q", archive_path, "-d", staging, NULL);
            _exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            rc = -1;
        }
    } else {
        // unknown: try gzip tar first, then plain tar, then unzip
        pid_t pid = fork();
        if (pid == 0) {
            execlp("tar", "tar", "-xzf", archive_path, "-C", staging, NULL);
            _exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        } else {
            rc = -1;
        }
        if (rc != 0) {
            pid_t pid2 = fork();
            if (pid2 == 0) {
                execlp("tar", "tar", "-xf", archive_path, "-C", staging, NULL);
                _exit(1);
            } else if (pid2 > 0) {
                int status;
                waitpid(pid2, &status, 0);
                rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            } else {
                rc = -1;
            }
        }
        if (rc != 0) {
            pid_t pid3 = fork();
            if (pid3 == 0) {
                execlp("unzip", "unzip", "-q", archive_path, "-d", staging, NULL);
                _exit(1);
            } else if (pid3 > 0) {
                int status;
                waitpid(pid3, &status, 0);
                rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            } else {
                rc = -1;
            }
        }
    }
        if (rc != 0) { /* failed to extract */
        LOG_ERROR("install_skin_archive_from_file: failed to extract archive %s (rc=%d)\n", archive_path, rc);
        // cleanup
            remove_path_recursive(staging);
        return -1;
    }
    // After extraction, check whether the staging area contains a single
    // top-level directory. If so, use that directory as the source so that
    // archives that wrap the content in a top-level folder (e.g., 'example/')
    // don't create nested paths like <id>/<id>/... when copied to SKINS_DIR.
    // This logic runs before 'cp -a'.
    char src_path[1024];
    {
        DIR *sd = opendir(staging);
        if (sd) {
            struct dirent *entry;
            int top_count = 0;
            char candidate[1024] = {0};
            while ((entry = readdir(sd)) != NULL) {
                if (entry->d_name[0] == '.') continue; // only count non-hidden entries
                top_count++;
                if (top_count == 1) snprintf(candidate, sizeof(candidate), "%s", entry->d_name);
            }
            closedir(sd);
            if (top_count == 1) {
                // It's a single top-level dir, use it as source
                snprintf(src_path, sizeof(src_path), "%s/%s", staging, candidate);
                LOG_VERBOSE("install_skin_archive_from_file: top-level single dir detected: %s\n", src_path);
            } else {
                // Multiple entries or none, copy entire staging
                snprintf(src_path, sizeof(src_path), "%s", staging);
            }
        } else {
            snprintf(src_path, sizeof(src_path), "%s", staging);
        }
    }

    // Find manifest
    char manifest_path[1024];
    // Find manifest.json inside staging (no shell usage)
    if (find_file_in_dir(staging, "manifest.json", 3, manifest_path, sizeof(manifest_path)) != 0) {
        remove_path_recursive(staging);
        return -1;
    }
    // Trim newline
    char *nl = strchr(manifest_path, '\n'); if (nl) *nl = '\0';
    // Parse manifest
    char idbuf[256] = {0}, namebuf[256] = {0}; int allow_js = 0;
    parse_skin_manifest(manifest_path, idbuf, sizeof(idbuf), namebuf, sizeof(namebuf), &allow_js);
    if (!idbuf[0]) {
        // fallback to directory name containing manifest
        char *dir = strdup(manifest_path);
        if (!dir) { remove_path_recursive(staging); return -1; }
        char *d = dirname(dir);
        if (d) snprintf(idbuf, sizeof(idbuf), "%s", basename(d));
        free(dir);
    }
    if (!idbuf[0]) { remove_path_recursive(staging); return -1; }
    // Install to SKINS_DIR/<id>
    char dest[1024]; snprintf(dest, sizeof(dest), "%s/%s", SKINS_DIR, idbuf);
    // Perform installation steps securely using execvp
    // Step 1: mkdir -p dest
    pid_t pid_mkdir = fork();
    if (pid_mkdir == 0) {
        execlp("mkdir", "mkdir", "-p", dest, NULL);
        _exit(1);
    } else if (pid_mkdir > 0) {
        int status;
        waitpid(pid_mkdir, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            LOG_ERROR("install_skin_archive_from_file: mkdir failed\n");
            rc = -1;
        }
    } else {
        LOG_ERROR("install_skin_archive_from_file: fork failed\n");
        rc = -1;
    }
    if (rc == 0) {
        // Step 2: rm -rf dest/*
        // Remove existing contents of the destination directory (no shell)
        if (remove_path_recursive(dest) != 0) {
            LOG_ERROR("install_skin_archive_from_file: rm failed for %s\n", dest);
            rc = -1;
        }
    }
    if (rc == 0) {
        // Step 3: cp -a src_path/. dest
        char src_dot[1024];
        snprintf(src_dot, sizeof(src_dot), "%s/.", src_path);
        pid_t pid_cp = fork();
        if (pid_cp == 0) {
            execlp("cp", "cp", "-a", src_dot, dest, NULL);
            _exit(1);
        } else if (pid_cp > 0) {
            int status;
            waitpid(pid_cp, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                LOG_ERROR("install_skin_archive_from_file: cp failed\n");
                rc = -1;
            }
        } else {
            LOG_ERROR("install_skin_archive_from_file: fork failed\n");
            rc = -1;
        }
    }
    if (rc == 0) {
        // Step 4: chown -R root:root dest; only attempt when running as root
        if (geteuid() == 0) {
            pid_t pid_chown = fork();
            if (pid_chown == 0) {
                execlp("chown", "chown", "-R", "root:root", dest, NULL);
                _exit(1);
            } else if (pid_chown > 0) {
                int status;
                waitpid(pid_chown, &status, 0);
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    LOG_ERROR("install_skin_archive_from_file: chown failed\n");
                    rc = -1;
                }
            } else {
                LOG_ERROR("install_skin_archive_from_file: fork failed\n");
                rc = -1;
            }
        } else {
            // Running as non-root: skip chown step and keep ownership as-is
            LOG_VERBOSE("install_skin_archive_from_file: non-root, skipping chown for %s\n", dest);
        }
    }
    // Cleanup staging
    remove_path_recursive(staging);
    if (rc != 0) { LOG_ERROR("install_skin_archive_from_file: failed to copy/perm dest %s (rc=%d)\n", dest, rc); return -1; }
    // Success
    snprintf(out_id, id_sz, "%s", idbuf);
    return 0;
}

// Primitive manifest parsing: look for "name" or "id" keys
// Trim whitespace in-place
static void trim_whitespace(char *s) {
    if (!s) return;
    // trim left
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    // trim right
    size_t l = strlen(s); while (l && isspace((unsigned char)s[l-1])) s[--l] = '\0';
}

// Primitive manifest parsing: look for "name" or "id" keys
static void parse_skin_manifest(const char *manifest_path, char *out_id, size_t id_sz, char *out_name, size_t name_sz, int *allow_extra_js) {
    out_id[0] = '\0'; out_name[0] = '\0'; if (allow_extra_js) *allow_extra_js = 0;
    FILE *fp = fopen(manifest_path, "r"); if (!fp) return;
    char buf[4096]; size_t r = fread(buf, 1, sizeof(buf)-1, fp); buf[r] = '\0'; fclose(fp);
    // crude parsing
    char *p = strstr(buf, "\"id\"");
    if (p) { char *q = strchr(p, ':'); if (q) { char *s = strchr(q, '"'); if (s) { s++; char *e = strchr(s, '"'); if (e) { size_t len = e - s; if (len >= id_sz) len = id_sz - 1; memcpy(out_id, s, len); out_id[len] = '\0'; } } } }
    trim_whitespace(out_id);
    p = strstr(buf, "\"name\"");
    if (p) { char *q = strchr(p, ':'); if (q) { char *s = strchr(q, '"'); if (s) { s++; char *e = strchr(s, '"'); if (e) { size_t len = e - s; if (len >= name_sz) len = name_sz - 1; memcpy(out_name, s, len); out_name[len] = '\0'; } } } }
    trim_whitespace(out_name);
    if (allow_extra_js) { p = strstr(buf, "\"allow_extra_js\""); if (p) { char *q = strchr(p, ':'); if (q) { while (*q && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' || *q == ':')) q++; if (strncmp(q, "true", 4) == 0) *allow_extra_js = 1; } } }
}

void build_skins_json(char *buf, size_t bufsz) {
    char tmp[4096]; size_t used = 0;
    /* track seen skin ids to prevent duplicates when scanning multiple locations */
    // store normalized (lowercase, trimmed) id values
    char seen_ids[256][256]; size_t seen_count = 0;
    DIR *d = opendir(SKINS_DIR); if (!d) { snprintf(buf, bufsz, "{\"skins\":[]}"); return; }
    struct dirent *ent; used += snprintf(tmp + used, sizeof(tmp) - used, "{\"skins\":["); int first = 1;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        // ensure it's a directory
        char path[1024]; snprintf(path, sizeof(path), "%s/%s", SKINS_DIR, ent->d_name);
        struct stat st; if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char manifest_path[1024]; size_t pathlen = strlen(path);
        if (pathlen + sizeof("/manifest.json") >= sizeof(manifest_path)) continue;
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", path);
        char id[256]; char name[256]; int allow_js = 0; parse_skin_manifest(manifest_path, id, sizeof(id), name, sizeof(name), &allow_js);
        if (!id[0]) { snprintf(id, sizeof(id), "%s", ent->d_name); }
        if (!name[0]) { snprintf(name, sizeof(name), "%s", id); }
        /* normalize id for dedupe: lowercase and trim */
        char normalized[256]; snprintf(normalized, sizeof(normalized), "%s", id);
        for (char *p = normalized; *p; ++p) *p = tolower((unsigned char)*p);
        trim_whitespace(normalized);
        /* skip duplicate entries by normalized id (case-insensitive) */
        int already = 0;
        for (size_t si = 0; si < seen_count; ++si) {
            if (strcmp(seen_ids[si], normalized) == 0) { already = 1; break; }
        }
        if (already) {
            LOG_VERBOSE("build_skins_json: skipping duplicate id: %s (normalized=%s) at path=%s\n", id, normalized, path);
            continue;
        }
        /* record id */
        snprintf(seen_ids[seen_count], sizeof(seen_ids[0]), "%s", normalized);
        seen_ids[seen_count][sizeof(seen_ids[0]) - 1] = '\0';
        seen_count++;
        if (!first) used += snprintf(tmp + used, sizeof(tmp) - used, ",");
        int is_active = (active_skin[0] && strcmp(id, active_skin) == 0) ? 1 : 0;
        used += snprintf(tmp + used, sizeof(tmp) - used, "{\"id\":\"%s\",\"name\":\"%s\",\"allow_extra_js\":%s,\"active\":%s}", id, name, allow_js ? "true" : "false", is_active ? "true" : "false");
        LOG_VERBOSE("build_skins_json: added skin id=%s name=%s active=%d\n", id, name, is_active);
        first = 0;
        if (used + 200 > sizeof(tmp)) break;
    }
    closedir(d);
    used += snprintf(tmp + used, sizeof(tmp) - used, "]}");
    if (used >= bufsz) { snprintf(buf, bufsz, "{\"skins\":[]}"); } else { strcpy(buf, tmp); }
}

int read_avg_cpu_temp(void);
int detect_cpu_thermal_zone(void);
void set_thermal_zone_path(int zone);
// Decide which thermal zone types should be excluded from average calculations
static int is_excluded_thermal_type(const char *lower_type) {
    if (!lower_type) return 0;
    // Check configured excluded types first (comma separated list)
    if (excluded_types_config[0]) {
        char tmp[512];
        strncpy(tmp, excluded_types_config, sizeof(tmp)-1);
        tmp[sizeof(tmp)-1] = '\0';
        char *tok = strtok(tmp, ",");
        while (tok) {
            // normalize token to lowercase
            char t[256];
            strncpy(t, tok, sizeof(t)-1);
            t[sizeof(t)-1] = '\0';
            for (char *p = t; *p; ++p) *p = tolower(*p);
            // exact or substring match in the lower_type
            if (strcmp(t, lower_type) == 0 || strstr(lower_type, t)) return 1;
            tok = strtok(NULL, ",");
        }
    }
    // Fallback to known defaults if no config provided
    if (strstr(lower_type, "int3400") || strstr(lower_type, "int3402") || strstr(lower_type, "int3403") || strstr(lower_type, "int3404") || strstr(lower_type, "int3405") || strstr(lower_type, "int3406") || strstr(lower_type, "int3407")) {
        return 1;
    }
    return 0;
}

int read_temp() {
    if (use_avg_temp) {
        return read_avg_cpu_temp();
    }
    FILE *fp = fopen(temp_path, "r");
    if (!fp) return -1;
    int temp_raw;
    if (fscanf(fp, "%d", &temp_raw) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return temp_raw / 1000;
}

int read_freq_value(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val;
    if (fscanf(fp, "%d", &val) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return val;
}

void set_max_freq_all_cpus(int freq) {
    if (!cpu_freq_paths) {
        cache_cpu_freq_paths();
    }
    for (int i = 0; i < num_cpus; i++) {
        if (!dry_run) {
            FILE *fp = fopen(cpu_freq_paths[i], "w");
            if (fp) {
                fprintf(fp, "%d", freq);
                fclose(fp);
            }
        }
    }
}

int clamp(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

// Normalize excluded types CSV: trim whitespace, lowercase tokens, dedupe and rejoin
static void normalize_excluded_types(char *out, size_t out_sz, const char *in) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;
    char tmp[512]; strncpy(tmp, in, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
    char *tok = strtok(tmp, ",");
    char seen[32][128]; int seen_count = 0;
    int first = 1;
    while (tok) {
        // Trim leading/trailing whitespace
        char *s = tok;
        while (*s && isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s) - 1;
        while (e >= s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
        if (*s) {
            // lowercase
            char lower[128]; size_t li = 0;
            for (size_t i = 0; s[i] && li + 1 < sizeof(lower); ++i) lower[li++] = (char)tolower((unsigned char)s[i]);
            lower[li] = '\0';
            // dedupe
            int dup = 0;
            for (int i = 0; i < seen_count; ++i) { if (strcmp(seen[i], lower) == 0) { dup = 1; break; } }
            if (!dup) {
                if (seen_count < (int)(sizeof(seen)/sizeof(seen[0]))) strncpy(seen[seen_count++], lower, sizeof(seen[0])-1);
                if (!first) {
                    strncat(out, ",", out_sz - strlen(out) - 1);
                }
                strncat(out, lower, out_sz - strlen(out) - 1);
                first = 0;
            }
        }
        tok = strtok(NULL, ",");
    }
}

// Cache CPU frequency paths to avoid repeated directory scans
void cache_cpu_freq_paths() {
    DIR *dir = opendir(CPUFREQ_PATH);
    struct dirent *entry;
    if (!dir) return;

    // Count CPUs first
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            count++;
        }
    }
    rewinddir(dir);

    // Allocate array
    cpu_freq_paths = malloc(count * sizeof(char *));
    if (!cpu_freq_paths) {
        closedir(dir);
        return;
    }

    // Store paths
    int i = 0;
    while ((entry = readdir(dir)) != NULL && i < count) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            char *path = malloc(512);
            if (path) {
                snprintf(path, 512, "%s/%s/cpufreq/scaling_max_freq", CPUFREQ_PATH, entry->d_name);
                cpu_freq_paths[i++] = path;
            }
        }
    }
    num_cpus = i;
    closedir(dir);
}

// Normalize excluded types CSV: trim whitespace, lowercase tokens, dedupe and rejoin
static void normalize_excluded_types(char *out, size_t out_sz, const char *in);

// Free cached CPU frequency paths
void free_cpu_cache() {
    if (cpu_freq_paths) {
        for (int i = 0; i < num_cpus; i++) {
            free(cpu_freq_paths[i]);
        }
        free(cpu_freq_paths);
        cpu_freq_paths = NULL;
        num_cpus = 0;
    }
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
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    
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
    // generate username for current process
    uid_t uid = geteuid();
    char uname[64] = "";
    struct passwd *pw = getpwuid(uid);
    if (pw) strncpy(uname, pw->pw_name, sizeof(uname)-1);
    else snprintf(uname, sizeof(uname), "uid:%d", (int)uid);

    snprintf(buffer, size,
             "{"
             "\"temperature\":%d,"
             "\"frequency\":%d,"
             "\"safe_min\":%d,"
             "\"safe_max\":%d,"
             "\"temp_max\":%d,"
             "\"sensor\":\"%s\","
             "\"thermal_zone\":%d,"
             "\"use_avg_temp\":%s,"
             "\"running_user\":\"%s\""
             "}",
             current_temp, current_freq, safe_min, safe_max, temp_max, temp_path, thermal_zone, use_avg_temp ? "true" : "false", uname);
}

void build_metrics_json(char *buffer, size_t size) {
    char *buf = buffer;
    size_t remaining = size - 1;
    int written = snprintf(buf, remaining, "{\"cpu_frequencies\":[");
    buf += written;
    remaining -= written;

    for (int i = 0; i < num_cpus && remaining > 10; i++) {
        char freq_path[512];
        // Replace scaling_max_freq with scaling_cur_freq
        snprintf(freq_path, sizeof(freq_path), "%s/cpufreq/scaling_cur_freq", strstr(cpu_freq_paths[i], "/cpufreq/scaling_max_freq") ? 
                 (char*)cpu_freq_paths[i] : cpu_freq_paths[i]);
        // Simpler: assume the path ends with scaling_max_freq, replace it
        strcpy(freq_path, cpu_freq_paths[i]);
        char *max_pos = strstr(freq_path, "scaling_max_freq");
        if (max_pos) strcpy(max_pos, "scaling_cur_freq");
        int freq = read_freq_value(freq_path);
        written = snprintf(buf, remaining, "%d%s", freq, (i < num_cpus - 1) ? "," : "");
        buf += written;
        remaining -= written;
    }
    snprintf(buf, remaining, "]}");
}

void build_zones_json(char *buffer, size_t size) {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) {
        snprintf(buffer, size, "{\"zones\":[]}");
        return;
    }
    struct dirent *entry;
    char *buf = buffer;
    size_t remaining = size - 1;
    // Collect zones first
    zone_entry_t zones[128];
    int zcount = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            int zone_num = atoi(entry->d_name + 12);
            char type_path[512];
            snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", entry->d_name);
            FILE *fp = fopen(type_path, "r");
            char type[256] = "unknown";
            if (fp) {
                if (fgets(type, sizeof(type), fp)) {
                    type[strcspn(type, "\n")] = 0;
                }
                fclose(fp);
            }
            char temp_path_test[512];
            snprintf(temp_path_test, sizeof(temp_path_test), "/sys/class/thermal/%s/temp", entry->d_name);
            FILE *temp_fp = fopen(temp_path_test, "r");
            int temp_c = -1;
            if (temp_fp) {
                int temp_raw;
                if (fscanf(temp_fp, "%d", &temp_raw) == 1) {
                    temp_c = temp_raw / 1000;
                }
                fclose(temp_fp);
            }
            int excluded = 0;
            char lower_type[256]; snprintf(lower_type, sizeof(lower_type), "%s", type); for (char *p = lower_type; *p; ++p) *p = tolower(*p);
            if (is_excluded_thermal_type(lower_type)) excluded = 1;
            if (zcount < (int)(sizeof(zones) / sizeof(zones[0]))) {
                zones[zcount].zone_num = zone_num;
                size_t copy_len = strlen(type);
                if (copy_len >= sizeof(zones[zcount].type)) copy_len = sizeof(zones[zcount].type)-1;
                memcpy(zones[zcount].type, type, copy_len);
                zones[zcount].type[copy_len] = '\0';
                zones[zcount].temp_c = temp_c;
                // mark excluded by negating temp to -100 to signal it for JSON (special value)
                if (excluded) zones[zcount].temp_c = -12345;
                zcount++;
            }
        }
    }
    closedir(dir);
    // Sort zones by zone number
    if (zcount > 1) {
        extern int zone_entry_cmp(const void *a, const void *b);
        qsort(zones, zcount, sizeof(zones[0]), zone_entry_cmp);
    }
    // Build JSON
    buf += snprintf(buf, remaining, "{\"zones\":[");
    remaining -= (buf - buffer);
    for (int i = 0; i < zcount; ++i) {
        if (i > 0) { buf += snprintf(buf, remaining, ","); remaining -= (buf - buffer); }
        int excluded = (zones[i].temp_c == -12345);
        int temp_val = excluded ? -1 : zones[i].temp_c;
        if (excluded) {
            buf += snprintf(buf, remaining, "{\"zone\":%d,\"type\":\"%s\",\"temp\":null,\"excluded\":true}", zones[i].zone_num, zones[i].type);
        } else {
            buf += snprintf(buf, remaining, "{\"zone\":%d,\"type\":\"%s\",\"temp\":%d,\"excluded\":false}", zones[i].zone_num, zones[i].type, temp_val);
        }
        remaining -= (buf - buffer);
    }
    buf += snprintf(buf, remaining, "]}");
}

void build_limits_json(char *buffer, size_t size) {
    char sensor_tmp[512];
    snprintf(sensor_tmp, sizeof(sensor_tmp), "%s", temp_path);
    sensor_tmp[sizeof(sensor_tmp)-1] = '\0';
    /* Some compilers warn about potential format truncation here because
     * temp_path may be long; the buffer should be large enough (>= 2048 in
     * callers), and we defensively silence the specific warning for this
     * controlled use. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(buffer, size,
             "{"
             "\"cpu_min_freq\":%d,"
             "\"cpu_max_freq\":%d,"
             "\"temp_sensor\":\"%s\""
             "}",
             cpu_min_freq, cpu_max_freq, sensor_tmp);
#pragma GCC diagnostic pop
}

// Minimal base64 decode helper (ignores invalid characters)
static int base64_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, unsigned char *out, size_t *out_len) {
    size_t ilen = strlen(in);
    size_t oidx = 0;
    int val = 0, valb = -8;
    for (size_t i = 0; i < ilen; ++i) {
        char ch = in[i];
        if (ch == '=') break; // padding: stop processing
        int c = base64_char_val(ch);
        if (c < 0) continue; // skip whitespace or invalid chars
        val = (val << 6) + c;
        valb += 6;
        if (valb >= 0) {
            if (out) out[oidx] = (unsigned char)((val >> valb) & 0xFF);
            oidx++;
            valb -= 8;
        }
    }
    if (out_len) *out_len = oidx;
    return 0;
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
            snprintf(name, sizeof(name), "%s", ent->d_name);
            name[sizeof(name)-1] = '\0';
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';
            // read content
            char content[4096];
                if (read_profile_file(name, content, sizeof(content)) == 0) {
                // escape content for JSON
                // Build a sanitized preview of the profile content (avoid embedding binary)
                const size_t MAX_PREVIEW = 512;
                char escaped[1024];
                size_t ei = 0;
                for (size_t i = 0; i < sizeof(content) && content[i] && ei < sizeof(escaped) - 10 && i < MAX_PREVIEW; i++) {
                    unsigned char ch = (unsigned char)content[i];
                    if (ch == '"') { escaped[ei++] = '\\'; escaped[ei++] = '"'; }
                    else if (ch == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; }
                    else if (ch == '\r') { escaped[ei++] = '\\'; escaped[ei++] = 'r'; }
                    else if (ch == '\t') { escaped[ei++] = '\\'; escaped[ei++] = 't'; }
                    else if (ch == '\\') { escaped[ei++] = '\\'; escaped[ei++] = '\\'; }
                    else if (ch >= 0x20 && ch < 0x7f) { escaped[ei++] = ch; }
                    else {
                        // Non-printable or high-bit char -> encode as \u00NN (4-digit unicode escape for compatibility)
                        if (ei + 6 < sizeof(escaped) - 1) {
                            unsigned int v = ch;
                            escaped[ei++] = '\\'; escaped[ei++] = 'u';
                            escaped[ei++] = '0'; escaped[ei++] = '0';
                            unsigned char hi = (v >> 4) & 0xF;
                            unsigned char lo = v & 0xF;
                            escaped[ei++] = (hi < 10) ? ('0' + hi) : ('A' + hi - 10);
                            escaped[ei++] = (lo < 10) ? ('0' + lo) : ('A' + lo - 10);
                        }
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

// Profile listing is global-only; per-directory listing omitted.

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

// Write profile raw content, no interpretation of \n escapes
int write_profile_file_raw(const char *name, const char *buf, size_t len) {
    if (ensure_profile_dir() != 0) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.config", get_profile_dir(), name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t written = fwrite(buf, 1, len, fp);
    fclose(fp);
    if (written != len) return -1;
    chmod(path, 0644);
    return 0;
}

// Per-directory profile helpers removed; the server uses global profile I/O helpers.

// Embedded dashboard: removed to avoid duplicate runtime/data (use generated headers or assets/)
// HTML/JS/CSS fallbacks removed; use generated headers or serve assets from disk instead.

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

// Extract JSON boolean value for a key (accepts true/false without quotes or quoted "true"/"false")
static int extract_json_bool(const char *body, const char *key, int *out) {
    const char *k = strstr(body, key);
    if (!k) return -1;
    const char *col = strchr(k, ':');
    if (!col) return -1;
    const char *p = col + 1;
    // skip whitespace
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') {
        p++;
        if (strncmp(p, "true", 4) == 0 && (p[4] == '"' || p[4] == '\0')) { if (out) *out = 1; return 0; }
        if (strncmp(p, "false", 5) == 0 && (p[5] == '"' || p[5] == '\0')) { if (out) *out = 0; return 0; }
        return -1;
    }
    if (strncmp(p, "true", 4) == 0) { if (out) *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { if (out) *out = 0; return 0; }
    return -1;
}

// IPv4 -> /proc/net/tcp helper removed (unused).

// Per-connection UID lookup removed; web UI operates globally.

// Per-connection UID lookup helper removed (unused).

// Per-client profile dir resolver removed; global profile dir is used.

// Parse HTTP request and route to handlers
void handle_http_request(int client_fd, const char *request) {
    char method[16], path[256];
    /* Limit copied sizes to prevent stack overflow from unbounded tokens */
    sscanf(request, "%15s %255s", method, path);
    
    LOG_VERBOSE("HTTP %s %s\n", method, path);
    
    char response[2048]; int rc = -1;
    // Serve skin files under /skins/<id>/<file>
    if (strncmp(path, "/skins/", 7) == 0) {
        const char *p = path + 7;
        const char *slash = strchr(p, '/');
        if (slash) {
            char sid[256] = {0}; size_t len = (size_t)(slash - p); if (len >= sizeof(sid)) len = sizeof(sid)-1; memcpy(sid, p, len); sid[len] = '\0';
            const char *rel = slash + 1; // e.g., "extra.js"
            if (strcmp(rel, "extra.js") == 0 || strcmp(rel, "styles.css") == 0 || strcmp(rel, "index.html") == 0 || strcmp(rel, "favicon.ico") == 0 || strcmp(rel, "preview.png") == 0) {
                if (skin_has_file(sid, rel)) {
                    char pathbuf[1024]; snprintf(pathbuf, sizeof(pathbuf), "%s/%s/%s", SKINS_DIR, sid, rel);
                    serve_file(client_fd, pathbuf);
                    return;
                }
            }
        }
        send_http_response(client_fd, "404 Not Found", "text/plain", "Not found");
        return;
    }
    
    // Serve bundled static assets
                if (strcmp(path, "/favicon.ico") == 0) {
                    if (active_skin[0] && serve_skin_asset(client_fd, active_skin, "/favicon.ico")) return;
            #ifdef USE_ASSET_HEADERS
                send_http_response_len(client_fd, "200 OK", "image/x-icon", ASSET_FAVICON, ASSET_FAVICON_LEN, NULL);
            #else
                if (serve_asset_from_disk(client_fd, "/favicon.ico")) return;
                send_http_response(client_fd, "404 Not Found", "text/plain", "Not found");
            #endif
                return;
                }

                if (strcmp(path, "/main.js") == 0) {
            #ifdef USE_ASSET_HEADERS
                send_http_response_len(client_fd, "200 OK", "application/javascript", ASSET_MAIN_JS, ASSET_MAIN_JS_LEN, NULL);
            #else
                if (serve_asset_from_disk(client_fd, "/main.js")) return;
                send_http_response(client_fd, "404 Not Found", "text/plain", "Not found");
            #endif
                return;
                }
            if (strcmp(path, "/styles.css") == 0) {
                if (active_skin[0] && serve_skin_asset(client_fd, active_skin, "/styles.css")) return;
        #ifdef USE_ASSET_HEADERS
            send_http_response_len(client_fd, "200 OK", "text/css", ASSET_STYLES_CSS, ASSET_STYLES_CSS_LEN, NULL);
        #else
            if (serve_asset_from_disk(client_fd, "/styles.css")) return;
            send_http_response(client_fd, "404 Not Found", "text/plain", "Not found");
        #endif
            return;
            }

    // Route API requests
        if (strcmp(path, "/") == 0) {
                if (active_skin[0]) {
                    if (skin_index_is_full(active_skin)) {
                        if (serve_skin_asset(client_fd, active_skin, "/index.html")) return;
                        // fallback to default if serving failed
                    } else {
                        // Serve default index but inject skin css/extra.js when available
                        if (serve_file_with_skin_extra(client_fd, "assets/index.html", active_skin)) return;
                        // fallback to default below if injection failed
                    }
                }
        #ifdef USE_ASSET_HEADERS
            if (active_skin[0]) {
                // CSS injection into compiled index head
                if (skin_has_file(active_skin, "styles.css")) {
                    const char *headneedle = "</head>";
                    char *headpos = memmem_shim(ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN, headneedle, strlen(headneedle));
                    if (headpos) {
                        size_t prefix = (size_t)(headpos - (char*)ASSET_INDEX_HTML);
                        const char *css_fmt = "<link rel=\"stylesheet\" href=\"/skins/%s/styles.css\">";
                        char css_inj[256]; snprintf(css_inj, sizeof(css_inj), css_fmt, active_skin);
                        size_t csslen = strlen(css_inj);
                        size_t newlen = ASSET_INDEX_HTML_LEN + csslen;
                        char *nb = malloc(newlen);
                        if (nb) {
                            memcpy(nb, ASSET_INDEX_HTML, prefix);
                            memcpy(nb + prefix, css_inj, csslen);
                            memcpy(nb + prefix + csslen, ASSET_INDEX_HTML + prefix, ASSET_INDEX_HTML_LEN - prefix);
                            send_http_response_len(client_fd, "200 OK", "text/html", nb, newlen, NULL);
                            free(nb);
                            return;
                        }
                    }
                }
                // inject extra.js into compiled index (if allowed)
                if (skin_has_file(active_skin, "extra.js") && skin_allows_extra_js(active_skin)) {
                    const char *bodyneedle = "</body>";
                    char *bodypos = memmem_shim(ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN, bodyneedle, strlen(bodyneedle));
                    if (bodypos) {
                        size_t prefix = (size_t)(bodypos - (char*)ASSET_INDEX_HTML);
                        const char *injection_fmt = "<script src=\"/skins/%s/extra.js\"></script>";
                        char injection[256]; snprintf(injection, sizeof(injection), injection_fmt, active_skin);
                        size_t injlen = strlen(injection);
                        size_t newlen = ASSET_INDEX_HTML_LEN + injlen;
                        char *nb = malloc(newlen);
                        if (nb) {
                            memcpy(nb, ASSET_INDEX_HTML, prefix);
                            memcpy(nb + prefix, injection, injlen);
                            memcpy(nb + prefix + injlen, ASSET_INDEX_HTML + prefix, ASSET_INDEX_HTML_LEN - prefix);
                            send_http_response_len(client_fd, "200 OK", "text/html", nb, newlen, NULL);
                            free(nb);
                            return; // served
                        }
                    }
                }
            }
            send_http_response_len(client_fd, "200 OK", "text/html", ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN, NULL);
        #else
            if (serve_file_with_skin_extra(client_fd, "assets/index.html", active_skin)) return;
            send_http_response(client_fd, "404 Not Found", "text/plain", "Not found");
        #endif
            }
    else if (strcmp(path, "/api/status") == 0 && strcmp(method, "GET") == 0) {
        build_status_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/metrics") == 0 && strcmp(method, "GET") == 0) {
        build_metrics_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/limits") == 0 && strcmp(method, "GET") == 0) {
        build_limits_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/zones") == 0 && strcmp(method, "GET") == 0) {
        build_zones_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/skins") == 0 && strcmp(method, "GET") == 0) {
        build_skins_json(response, sizeof(response));
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strcmp(path, "/api/skins/upload") == 0 && strcmp(method, "POST") == 0) {
        const char *body_start = strstr(request, "\r\n\r\n");
        if (!body_start) { send_http_response(client_fd, "400 Bad Request", "text/plain", "Missing body\n"); return; }
        body_start += 4;
        // Locate the archive JSON value and allocate an appropriately sized buffer so large uploads succeed
        const char *k = strstr(body_start, "\"archive\"");
        if (!k) { send_http_response(client_fd, "400 Bad Request", "text/plain", "Missing archive base64\n"); return; }
        const char *col = strchr(k, ':'); if (!col) { send_http_response(client_fd, "400 Bad Request", "text/plain", "Invalid archive field\n"); return; }
        const char *first_quote = strchr(col, '"'); if (!first_quote) { send_http_response(client_fd, "400 Bad Request", "text/plain", "Invalid archive payload\n"); return; }
        first_quote++;
        const char *end_quote = first_quote;
        while (*end_quote && !(*end_quote == '"' && *(end_quote - 1) != '\\')) end_quote++;
        if (*end_quote != '"') { send_http_response(client_fd, "400 Bad Request", "text/plain", "Invalid archive payload\n"); return; }
        size_t b64_len = (size_t)(end_quote - first_quote);
        // Upper bound for base64 length: decoded size must be <= 10MB
        size_t max_decoded_est = (b64_len * 3) / 4 + 4;
        if (max_decoded_est > 10 * 1024 * 1024) { send_http_response(client_fd, "413 Payload Too Large", "text/plain", "Payload too large\n"); return; }
        char *b64 = malloc(b64_len + 1);
        if (!b64) { send_http_response(client_fd, "500 Internal Server Error", "text/plain", "Memory allocation failed\n"); return; }
        memcpy(b64, first_quote, b64_len);
        b64[b64_len] = '\0';
        size_t max_decoded = (b64_len * 3) / 4 + 4;
        if (max_decoded > 10 * 1024 * 1024) { send_http_response(client_fd, "413 Payload Too Large", "text/plain", "Payload too large\n"); return; }
        unsigned char *decoded = malloc(max_decoded);
        if (!decoded) { send_http_response(client_fd, "500 Internal Server Error", "text/plain", "Memory allocation failed\n"); return; }
        size_t dlen = 0;
        if (base64_decode(b64, decoded, &dlen) != 0) { free(decoded); free(b64); send_http_response(client_fd, "400 Bad Request", "text/plain", "Invalid base64\n"); return; }
        // Write to temp file
        char tmpfile_template[] = "/tmp/burn2cool_skin_XXXXXX";
        LOG_VERBOSE("upload_skin: created tmp template: %s\n", tmpfile_template);
        int fd = mkstemp(tmpfile_template);
        if (fd < 0) { send_http_response(client_fd, "500 Internal Server Error", "text/plain", "Failed to create temp file\n"); return; }
        ssize_t w = write(fd, decoded, dlen);
        close(fd);
        if ((size_t)w != dlen) { unlink(tmpfile_template); LOG_ERROR("upload_skin: failed to write decoded file %s (w=%zd,dlen=%zu)\n", tmpfile_template, w, dlen); send_http_response(client_fd, "500 Internal Server Error", "text/plain", "Failed to write temp file\n"); return; }
        LOG_VERBOSE("upload_skin: wrote temp file %s (decoded %zu bytes)\n", tmpfile_template, dlen);
        // install
        char installed_id[256] = {0};
        if (install_skin_archive_from_file(tmpfile_template, installed_id, sizeof(installed_id)) != 0) {
            LOG_ERROR("upload_skin: install_skin_archive_from_file failed for %s\n", tmpfile_template);
            unlink(tmpfile_template);
            send_http_response(client_fd, "500 Internal Server Error", "text/plain", "Skin install failed\n");
            return;
        }
        // optionally activate if 'activate' flag is present
        int activate_bool = 0;
        if (extract_json_bool(body_start, "\"activate\"", &activate_bool) == 0) {
            if (activate_bool) { snprintf(active_skin, sizeof(active_skin), "%s", installed_id); save_config_file(); }
        }
        free(decoded); free(b64);
        unlink(tmpfile_template);
        snprintf(response, sizeof(response), "{\"ok\":true,\"installed\":\"%s\"}", installed_id);
        send_http_response(client_fd, "201 Created", "application/json", response);
        return;
    }
    else if (strcmp(path, "/api/skins/default") == 0 && strcmp(method, "POST") == 0) {
        // Reset to default (clear active skin)
        active_skin[0] = '\0';
        save_config_file();
        snprintf(response, sizeof(response), "{\"ok\":true,\"active\":null}");
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strncmp(path, "/api/skins/", 11) == 0 && strcmp(method, "POST") == 0) {
        // Expect POST /api/skins/<id>/activate
        const char *p = path + 11;
        const char *slash = strchr(p, '/');
        if (!slash) { send_http_response(client_fd, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid skins route\"}"); }
        else {
            char id[256]; size_t idlen = (size_t)(slash - p); if (idlen >= sizeof(id)) idlen = sizeof(id)-1; memcpy(id, p, idlen); id[idlen] = '\0';
            const char *action = slash + 1;
            if (strcmp(action, "activate") == 0) {
                if (!skin_exists(id)) {
                    send_http_response(client_fd, "404 Not Found", "application/json", "{\"ok\":false,\"error\":\"skin not found\"}");
                } else {
                    // activate skin
                    snprintf(active_skin, sizeof(active_skin), "%s", id);
                    save_config_file();
                    snprintf(response, sizeof(response), "{\"ok\":true,\"active\":\"%s\"}", active_skin);
                    send_http_response(client_fd, "200 OK", "application/json", response);
                }
            } else if (strcmp(action, "deactivate") == 0) {
                if (!skin_exists(id)) {
                    send_http_response(client_fd, "404 Not Found", "application/json", "{\"ok\":false,\"error\":\"skin not found\"}");
                } else {
                    if (strcmp(active_skin, id) == 0) {
                        active_skin[0] = '\0'; save_config_file();
                        snprintf(response, sizeof(response), "{\"ok\":true,\"active\":null}");
                        send_http_response(client_fd, "200 OK", "application/json", response);
                    } else {
                        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"skin not active\"}");
                        send_http_response(client_fd, "400 Bad Request", "application/json", response);
                    }
                }
            } else {
                if (strcmp(action, "remove") == 0) {
                    // remove the skin directory from SKINS_DIR
                    if (!skin_exists(id)) {
                        send_http_response(client_fd, "404 Not Found", "application/json", "{\"ok\":false,\"error\":\"skin not found\"}");
                    } else {
                        if (strcmp(active_skin, id) == 0) { active_skin[0] = '\0'; save_config_file(); }
                        char dest[4096]; snprintf(dest, sizeof(dest), "%s/%s", SKINS_DIR, id);
                        rc = remove_path_recursive(dest);
                        snprintf(response, sizeof(response), "{\"ok\":true,\"removed\":\"%s\"}", id);
                        send_http_response(client_fd, "200 OK", "application/json", response);
                    }
                } else {
                    send_http_response(client_fd, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
                }
            }
        }
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
    else if (strcmp(path, "/api/daemon/restart") == 0 && strcmp(method, "POST") == 0) {
        should_restart = 1;
        should_exit = 1; // exit loop; exec will be handled after cleanup
        snprintf(response, sizeof(response), "{\"status\":\"restarting\"}");
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
        // Build profiles JSON into a dynamically-sized buffer to avoid truncation
        size_t bufsize = 16384;
        char *listbuf = malloc(bufsize);
        if (!listbuf) {
            send_http_response(client_fd, "500 Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"malloc failed\"}");
            return;
        }
        build_profiles_list_json(listbuf, bufsize);
        // ensure response buffer large enough
        size_t resp_size = strlen(listbuf) + 64;
        char *resp = malloc(resp_size);
        if (!resp) {
            free(listbuf);
            send_http_response(client_fd, "500 Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"malloc failed\"}");
            return;
        }
        snprintf(resp, resp_size, "{\"ok\":true,\"profiles\":%s}", listbuf);
        send_http_response(client_fd, "200 OK", "application/json", resp);
        free(listbuf);
        free(resp);
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
                            snprintf(tmp, sizeof(tmp), "%s", body);
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
    else if (strcmp(path, "/api/settings/excluded-types") == 0 && strcmp(method, "GET") == 0) {
        snprintf(response, sizeof(response), "{\"excluded_types\":\"%s\"}", excluded_types_config);
        send_http_response(client_fd, "200 OK", "application/json", response);
    }
    else if (strncmp(path, "/api/settings/", 14) == 0 && strcmp(method, "POST") == 0) {
        // Extract setting name (safe-max, safe-min, temp-max)
        const char *setting = path + 14;
        
        // Parse JSON body {\"value\":123}
        const char *body_start = strstr(request, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int value = 0;
            sscanf(body_start, "{\"value\":%d}", &value);
            
            if (strcmp(setting, "safe-max") == 0) {
                safe_max = value;
                save_config_file();
                snprintf(response, sizeof(response), "{\"status\":\"ok\",\"safe_max\":%d}", safe_max);
            }
            else if (strcmp(setting, "safe-min") == 0) {
                safe_min = value;
                save_config_file();
                snprintf(response, sizeof(response), "{\"status\":\"ok\",\"safe_min\":%d}", safe_min);
            }
            else if (strcmp(setting, "temp-max") == 0) {
                if (value >= 50 && value <= 110) {
                    temp_max = value;
                    save_config_file();
                    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"temp_max\":%d}", temp_max);
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"temp_max must be 50-110\"}");
                }
            }
            else if (strcmp(setting, "thermal-zone") == 0) {
                if (value >= -1 && value <= 100) {  // -1 for auto, or zone number
                    thermal_zone = value;
                    // Re-detect temp_path if auto
                    if (thermal_zone == -1) {
                        int detected = detect_cpu_thermal_zone();
                        thermal_zone = detected;
                    }
                    save_config_file();
                    snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", thermal_zone);
                    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"thermal_zone\":%d}", thermal_zone);
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"thermal_zone must be -1 (auto) or 0-100\"}");
                }
            }
            else if (strcmp(setting, "excluded-types") == 0) {
                // parse a string value in JSON body {"value":"int3400,int3402"}
                char valbuf[512] = {0};
                const char *vpos = strstr(body_start, "\"value\"");
                if (vpos) {
                    // find the ':' after the key and then the opening quote
                    const char *colon = strchr(vpos, ':');
                    if (colon) {
                        const char *p = colon + 1;
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p == '"') {
                            const char *start_q = p + 1;
                            const char *end_q = strchr(start_q, '"');
                            if (end_q) {
                                size_t len = end_q - start_q;
                                if (len >= sizeof(valbuf)) len = sizeof(valbuf) - 1;
                                memcpy(valbuf, start_q, len);
                                valbuf[len] = '\0';
                            }
                        } else {
                            // value not quoted - try to read until non-token char (comma/brace/whitespace)
                            const char *start = p;
                            while (*start && isspace((unsigned char)*start)) start++;
                            const char *end = start;
                            while (*end && !isspace((unsigned char)*end) && *end != ',' && *end != '}') end++;
                            size_t len = end - start;
                            if (len >= sizeof(valbuf)) len = sizeof(valbuf) - 1;
                            memcpy(valbuf, start, len);
                            valbuf[len] = '\0';
                        }
                    }
                }
                if (valbuf[0]) {
                    for (char *p = valbuf; *p; ++p) *p = tolower((unsigned char)*p);
                    if (strcmp(valbuf, "none") == 0 || strcmp(valbuf, "clear") == 0) {
                        excluded_types_config[0] = '\0';
                        int sr = save_config_file();
                        if (sr == 0) snprintf(response, sizeof(response), "{\"status\":\"ok\",\"excluded_types\":\"\",\"saved\":true,\"saved_to\":\"%s\"}", saved_config_path);
                        else snprintf(response, sizeof(response), "{\"status\":\"ok\",\"excluded_types\":\"\",\"saved\":false,\"message\":\"failed to write config\"}");
                    } else {
                        char normalized[512]; normalized[0] = '\0';
                        normalize_excluded_types(normalized, sizeof(normalized), valbuf);
                        snprintf(excluded_types_config, sizeof(excluded_types_config), "%s", normalized);
                        int sr = save_config_file();
                        if (sr == 0) snprintf(response, sizeof(response), "{\"status\":\"ok\",\"excluded_types\":\"%s\",\"saved\":true,\"saved_to\":\"%s\"}", excluded_types_config, saved_config_path);
                        else snprintf(response, sizeof(response), "{\"status\":\"ok\",\"excluded_types\":\"%s\",\"saved\":false,\"message\":\"failed to write config\"}", excluded_types_config);
                    }
                } else {
                    snprintf(response, sizeof(response), "{\"status\":\"error\",\"message\":\"invalid excluded-types payload\"}");
                }
            }
            else if (strcmp(setting, "use-avg-temp") == 0) {
                use_avg_temp = value ? 1 : 0;
                int sr = save_config_file();
                if (sr == 0) snprintf(response, sizeof(response), "{\"status\":\"ok\",\"use_avg_temp\":%s,\"saved\":true,\"saved_to\":\"%s\"}", use_avg_temp ? "true" : "false", saved_config_path);
                else snprintf(response, sizeof(response), "{\"status\":\"ok\",\"use_avg_temp\":%s,\"saved\":false,\"message\":\"failed to write config\"}", use_avg_temp ? "true" : "false");
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
        // Read entire HTTP request into dynamically sized buffer. This handles
        // larger POST bodies (skins upload) where the request may not fit into
        // a single read() call.
        size_t cap = 4096; size_t total = 0;
        char *buffer = malloc(cap);
        if (!buffer) { close(client_fd); continue; }
        while (1) {
            ssize_t bytes = read(client_fd, buffer + total, cap - total - 1);
            if (bytes <= 0) break; // EOF or error
            total += (size_t)bytes;
            buffer[total] = '\0';
            // If we have headers, try to determine Content-Length and stop when body is fully read
            char *body_start = strstr(buffer, "\r\n\r\n");
            if (body_start) {
                size_t headers_len = (size_t)(body_start + 4 - buffer);
                int content_len = 0;
                const char *cl = strstr(buffer, "Content-Length:");
                if (cl) { cl += strlen("Content-Length:"); while (*cl && isspace((unsigned char)*cl)) cl++; content_len = atoi(cl); }
                if (content_len > 0) {
                    if (total >= headers_len + (size_t)content_len) break; // whole body received
                } else {
                    // no content-length -> treat as complete request (typical for GET)
                    break;
                }
            }
            // grow buffer when required
            if (cap - total < 4096) { cap *= 2; char *nb = realloc(buffer, cap); if (!nb) break; buffer = nb; }
        }
        if (total > 0) {
            buffer[total] = '\0'; handle_http_request(client_fd, buffer);
        }
        free(buffer);
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

        char buffer[16384];
        ssize_t n = 0;
        size_t total = 0;
        // Read until no more data (non-blocking) or EOF
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        while (1) {
            n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
            if (n > 0) {
                total += n;
                if (total >= sizeof(buffer) - 1) break;
                continue;
            }
            if (n == 0) break; // EOF
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { perror("recv"); break; }
        }
        fcntl(client_fd, F_SETFL, flags);
        if (total > 0) {
            buffer[total] = '\0';
            n = (ssize_t)total;
        } else {
            buffer[0] = '\0'; n = 0;
        }
        if (n > 0) {
            buffer[n] = '\0';
            // Trim trailing whitespace/newlines so commands like "list-skins\n" match
            size_t blen = strlen(buffer);
            while (blen && isspace((unsigned char)buffer[blen-1])) { buffer[--blen] = '\0'; }
            LOG_INFO("Received command: '%s'\n", buffer);

            // If the incoming data looks like an HTTP request, pass it to the HTTP handler
            if (strncmp(buffer, "GET ", 4) == 0 || strncmp(buffer, "POST ", 5) == 0 ||
                strncmp(buffer, "HEAD ", 5) == 0) {
                handle_http_request(client_fd, buffer);
                close(client_fd);
                continue;
            }

            // Parse command
            char cmd[64], arg[192]; int rc = -1;
            char response[512];

            // Find first space to split cmd and arg
            char *space = strchr(buffer, ' ');
            if (space) {
                size_t cmd_len = space - buffer;
                if (cmd_len > 63) cmd_len = 63;
                // Copy command from buffer; cmd_len already bounded by 63
                memcpy(cmd, buffer, cmd_len);
                cmd[cmd_len] = '\0';
                snprintf(arg, sizeof(arg), "%s", space + 1);
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
                    save_config_file();
                    snprintf(response, sizeof(response), "OK: safe_max set to %d kHz\n", safe_max);
                }
                else if (strcmp(cmd, "set-safe-min") == 0 && sscanf(arg, "%d", &safe_min) == 1) {
                    if (safe_min < min_freq) safe_min = min_freq;
                    if (safe_min > max_freq_limit) safe_min = max_freq_limit;
                    save_config_file();
                    snprintf(response, sizeof(response), "OK: safe_min set to %d kHz\n", safe_min);
                }
                else if (strcmp(cmd, "set-temp-max") == 0 && sscanf(arg, "%d", &temp_max) == 1) {
                    if (temp_max < 50 || temp_max > 110) {
                        snprintf(response, sizeof(response), "ERROR: temp_max must be 50-110°C\n");
                    } else {
                        save_config_file();
                        snprintf(response, sizeof(response), "OK: temp_max set to %d°C\n", temp_max);
                    }
                }
                else if (strcmp(cmd, "set-thermal-zone") == 0 && sscanf(arg, "%d", &thermal_zone) == 1) {
                    if (thermal_zone < -1 || thermal_zone > 100) {
                        snprintf(response, sizeof(response), "ERROR: thermal_zone must be -1 (auto) or 0-100\n");
                    } else {
                        if (thermal_zone == -1) {
                            int detected = detect_cpu_thermal_zone();
                            thermal_zone = detected;
                        }
                        set_thermal_zone_path(thermal_zone);
                        save_config_file();
                        snprintf(response, sizeof(response), "OK: thermal_zone set to %d\n", thermal_zone);
                    }
                }
                else if (strcmp(cmd, "set-use-avg-temp") == 0 && sscanf(arg, "%d", &use_avg_temp) == 1) {
                    use_avg_temp = use_avg_temp ? 1 : 0;
                    int sr = save_config_file();
                        if (sr == 0) snprintf(response, sizeof(response), "OK: use_avg_temp set to %d (saved to %s)\n", use_avg_temp, saved_config_path);
                        else snprintf(response, sizeof(response), "OK: use_avg_temp set to %d (not saved)\n", use_avg_temp);
                }
                else if (strcmp(cmd, "set-excluded-types") == 0) {
                    if (arg[0]) {
                        // allow case-insensitive tokens, trim spaces and normalize csv
                        char normalized[512]; normalized[0] = '\0';
                        normalize_excluded_types(normalized, sizeof(normalized), arg);
                        if (strcmp(normalized, "none") == 0 || strcmp(normalized, "clear") == 0) {
                            excluded_types_config[0] = '\0';
                            int sr = save_config_file();
                            if (sr == 0) snprintf(response, sizeof(response), "OK: excluded types cleared (saved to %s)\n", saved_config_path);
                            else snprintf(response, sizeof(response), "OK: excluded types cleared (not saved)\n");
                        } else if (normalized[0] == '\0') {
                            snprintf(response, sizeof(response), "ERROR: missing excluded types\n");
                        } else {
                            strncpy(excluded_types_config, normalized, sizeof(excluded_types_config)-1);
                            excluded_types_config[sizeof(excluded_types_config)-1] = '\0';
                            int sr = save_config_file();
                            if (sr == 0) snprintf(response, sizeof(response), "OK: excluded types set to %.480s (saved to %s)\n", excluded_types_config, saved_config_path);
                            else snprintf(response, sizeof(response), "OK: excluded types set to %.480s (not saved)\n", excluded_types_config);
                        }
                    } else {
                        snprintf(response, sizeof(response), "ERROR: missing excluded types\n");
                    }
                }
                else if (strcmp(cmd, "version") == 0) {
                    snprintf(response, sizeof(response), "{\"version\":\"%s\"}\n", DAEMON_VERSION);
                }
                else if (strcmp(cmd, "limits") == 0) {
                    build_limits_json(response, sizeof(response));
                }
                else if (strcmp(cmd, "zones") == 0) {
                    build_zones_json(response, sizeof(response));
                }
                else if (strcmp(cmd, "quit") == 0) {
                    should_exit = 1;
                    snprintf(response, sizeof(response), "OK: shutting down\n");
                }
                else if (strcmp(cmd, "restart") == 0) {
                    should_restart = 1;
                    should_exit = 1;
                    snprintf(response, sizeof(response), "OK: restarting\n");
                }
                else if (strcmp(cmd, "get-profile") == 0) {
                    char body[4096];
                    if (read_profile_file(arg, body, sizeof(body)) == 0) {
                        // send the raw profile content directly (may contain newlines)
                        send(client_fd, body, strlen(body), 0);
                        close(client_fd);
                        continue; // skip sending the default response
                    } else {
                        snprintf(response, sizeof(response), "ERROR: not found\n");
                    }
                }
                else if (strcmp(cmd, "get-excluded-types") == 0) {
                    // Return the raw CSV for excluded types (may be empty)
                    snprintf(response, sizeof(response), "%s", excluded_types_config);
                }
                else if (strcmp(cmd, "write-profile-base64") == 0) {
                    // arg has "<name> <base64>"
                    char pname[256] = {0};
                    char *space2 = strchr(arg, ' ');
                    if (!space2) {
                        snprintf(response, sizeof(response), "ERROR: missing arguments\n");
                    } else {
                        size_t namelen = (size_t)(space2 - arg);
                        if (namelen >= sizeof(pname)) namelen = sizeof(pname)-1;
                        memcpy(pname, arg, namelen);
                        pname[namelen] = '\0';
                        char *b64 = space2 + 1;
                        unsigned char decoded[8192];
                        size_t dlen = 0;
                        base64_decode(b64, decoded, &dlen);
                        LOG_VERBOSE("Decoded profile content (len=%zu): %s\n", dlen, decoded);
                        if (dlen >= sizeof(decoded)) dlen = sizeof(decoded) - 1;
                        decoded[dlen] = '\0';
                        // Treat decoded as text
                        if (ensure_profile_dir() == 0 && write_profile_file(pname, (const char*)decoded) == 0) {
                            snprintf(response, sizeof(response), "OK: profile %s written\n", pname);
                        } else {
                            snprintf(response, sizeof(response), "ERROR: write failed\n");
                        }
                    }
                }
                else if (strcmp(cmd, "put-profile") == 0) {
                    // Expect header: "put-profile <name> <len>\n" then raw bytes of length <len>
                    char pname[256] = {0};
                    size_t plen = 0;
                    int hdr_len = 0;
                    int parsed = sscanf(buffer, "%63s %255s %zu %n", cmd, pname, &plen, &hdr_len);
                    if (parsed < 3) {
                        snprintf(response, sizeof(response), "ERROR: invalid header\n");
                    } else {
                        // attempt to compute how many payload bytes were already read
                        size_t body_start = (size_t)hdr_len;
                        size_t have = total > body_start ? total - body_start : 0;
                        // allocate buffer to hold full payload
                        if (plen > 1024 * 1024 * 10) { // limit to 10MB
                            snprintf(response, sizeof(response), "ERROR: payload too large\n");
                            // Drain the remaining payload bytes (client will still send them)
                            size_t drained = (size_t)have;
                            while (drained < plen) {
                                ssize_t r = recv(client_fd, buffer, sizeof(buffer), 0);
                                if (r <= 0) {
                                    break;
                                }
                                drained += (size_t)r;
                            }
                        } else {
                            char *payload = malloc(plen + 1);
                                if (!payload) {
                                snprintf(response, sizeof(response), "ERROR: malloc failed\n");
                                // Drain the remaining payload to avoid client broken-pipe
                                size_t drained = (size_t)have;
                                while (drained < plen) {
                                    ssize_t r = recv(client_fd, buffer, sizeof(buffer), 0);
                                    if (r <= 0) {
                                        break;
                                    }
                                    drained += (size_t)r;
                                }
                            } else {
                                if (have > 0) memcpy(payload, buffer + body_start, have);
                                // Read remaining bytes if any
                                while (have < plen) {
                                    ssize_t r = recv(client_fd, payload + have, plen - have, 0);
                                    if (r <= 0) { break; }
                                    have += (size_t)r;
                                }
                                if (have == plen) {
                                    // write raw payload to profile
                                    if (ensure_profile_dir() == 0 && write_profile_file_raw(pname, payload, plen) == 0) {
                                        snprintf(response, sizeof(response), "OK: profile %s written\n", pname);
                                    } else {
                                        snprintf(response, sizeof(response), "ERROR: write failed\n");
                                    }
                                } else {
                                    snprintf(response, sizeof(response), "ERROR: incomplete payload\n");
                                }
                                free(payload);
                            }
                        }
                    }
                }
                else if (strcmp(cmd, "put-skin") == 0) {
                    // Expect header: put-skin <name> <len>\n then raw bytes
                    char sname[256] = {0}; size_t slen = 0; int hdr_len = 0;
                    int parsed = sscanf(buffer, "%63s %255s %zu %n", cmd, sname, &slen, &hdr_len);
                    if (parsed < 3) { snprintf(response, sizeof(response), "ERROR: invalid header\n"); }
                    else {
                        size_t body_start = (size_t)hdr_len;
                        size_t have = total > body_start ? total - body_start : 0;
                        if (slen > 50 * 1024 * 1024) { snprintf(response, sizeof(response), "ERROR: payload too large\n"); }
                        else {
                            // write raw payload to temp file (mkstemp requires XXXXXX at end)
                            char tmp_template[] = "/tmp/burn2cool_skin_XXXXXX";
                            int fd = mkstemp(tmp_template);
                            if (fd < 0) {
                                snprintf(response, sizeof(response), "ERROR: cannot create tmp file\n");
                                size_t discarded = (size_t)have;
                                while (discarded < slen) {
                                    ssize_t r = recv(client_fd, buffer, sizeof(buffer), 0);
                                    if (r <= 0) break;
                                    discarded += (size_t)r;
                                }
                            } else {
                                // write already-read bytes
                                size_t written = 0;
                                if (have > 0) {
                                    ssize_t w = write(fd, buffer + body_start, have);
                                    if (w < 0) { close(fd); unlink(tmp_template); snprintf(response, sizeof(response), "ERROR: write failed\n"); goto putskin_done; }
                                    written += (size_t)w;
                                }
                                // read remaining
                                while (written < slen) {
                                    ssize_t r;
                                    do {
                                        r = recv(client_fd, buffer, sizeof(buffer), 0);
                                    } while (r == -1 && errno == EINTR);
                                    if (r <= 0) { close(fd); unlink(tmp_template); snprintf(response, sizeof(response), "ERROR: receive failed\n"); goto putskin_done; }
                                    ssize_t w = write(fd, buffer, r);
                                    if (w < 0) { close(fd); unlink(tmp_template); snprintf(response, sizeof(response), "ERROR: write failed\n"); goto putskin_done; }
                                    written += (size_t)w;
                                }
                                close(fd);
                                // install
                                char installed_id[256] = {0};
                                if (install_skin_archive_from_file(tmp_template, installed_id, sizeof(installed_id)) != 0) {
                                    LOG_ERROR("put-skin: install_skin_archive_from_file failed for %s\n", tmp_template);
                                    snprintf(response, sizeof(response), "ERROR: install failed\n");
                                } else {
                                    // Return installed id in textual form so cli can parse it
                                    snprintf(response, sizeof(response), "OK: installed %s\n", installed_id);
                                }
                                unlink(tmp_template);
                            }
                        }
                    }
                    
                putskin_done: ;
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
                else if (strcmp(cmd, "list-skins") == 0) {
                    if (strcmp(arg, "json") == 0) {
                        build_skins_json(response, sizeof(response));
                    } else {
                        DIR *d = opendir(SKINS_DIR);
                        if (!d) {
                            snprintf(response, sizeof(response), "ERROR: cannot open skins directory\n");
                        } else {
                            struct dirent *ent;
                            char *ptr = response; size_t remaining = sizeof(response);
                            while ((ent = readdir(d)) && remaining > 2) {
                                if (ent->d_name[0] == '.') continue;
                                char full[1024]; snprintf(full, sizeof(full), "%s/%s", SKINS_DIR, ent->d_name);
                                struct stat st; if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                                int written = snprintf(ptr, remaining, "%s\n", ent->d_name);
                                if (written > 0 && (size_t)written < remaining) { ptr += written; remaining -= written; } else break;
                            }
                            closedir(d);
                        }
                    }
                }
                else if (strcmp(cmd, "activate-skin") == 0) {
                    if (!skin_exists(arg)) {
                        snprintf(response, sizeof(response), "ERROR: skin not found\n");
                    } else {
                        strncpy(active_skin, arg, sizeof(active_skin)-1); active_skin[sizeof(active_skin)-1] = '\0';
                        save_config_file();
                        snprintf(response, sizeof(response), "OK: skin %s activated\n", active_skin);
                    }
                }
                else if (strcmp(cmd, "deactivate-skin") == 0) {
                    if (!skin_exists(arg)) {
                        snprintf(response, sizeof(response), "ERROR: skin not found\n");
                    } else {
                        if (strcmp(active_skin, arg) == 0) {
                            active_skin[0] = '\0'; save_config_file();
                            snprintf(response, sizeof(response), "OK: skin %s deactivated\n", arg);
                        } else {
                            snprintf(response, sizeof(response), "ERROR: skin %s not active\n", arg);
                        }
                    }
                }
                else if (strcmp(cmd, "remove-skin") == 0) {
                    if (!skin_exists(arg)) {
                        snprintf(response, sizeof(response), "ERROR: skin not found\n");
                    } else {
                        // If this skin is active, clear it
                        if (strcmp(active_skin, arg) == 0) { active_skin[0] = '\0'; save_config_file(); }
                        char dest[4096]; snprintf(dest, sizeof(dest), "%s/%s", SKINS_DIR, arg);
                        rc = remove_path_recursive(dest);
                        snprintf(response, sizeof(response), "OK: skin %s removed\n", arg);
                    }
                }
                else if (strcmp(cmd, "load-profile") == 0) {
                    char body[4096];
                    if (read_profile_file(arg, body, sizeof(body)) == 0) {
                        // apply key=values
                        char tmp[4096];
                        snprintf(tmp, sizeof(tmp), "%s", body);
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
                    snprintf(lower_type, sizeof(lower_type), "%s", type);
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
                                    // Skip excluded policy/dummy devices from avg calculation
                                    if (is_excluded_thermal_type(lower_type)) {
                                        fclose(temp_fp);
                                        continue;
                                    }
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
    if (use_avg_temp) avg -= 5;
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
                    snprintf(lower_type, sizeof(lower_type), "%s", type);
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
    printf("  --test               Run unit tests and exit\n");
    printf("  --help               Show this help message\n");
    printf("\nConfig file: %s (optional)\n", CONFIG_FILE);
    printf("Supported keys: temp_max, safe_min, safe_max, sensor, thermal_zone, avg_temp, web_port\n");
    printf("\nWeb Interface:\n");
    printf("  Use --web-port (without argument) for default port %d\n", DEFAULT_WEB_PORT);
    printf("  Use --web-port <port> for custom port (1024-65535)\n");
}

int run_tests() {
    printf("Running unit tests...\n");

    // Test base64_decode
    const char *b64_input = "SGVsbG8gV29ybGQ="; // "Hello World"
    unsigned char decoded[20];
    size_t dlen;
    base64_decode(b64_input, decoded, &dlen);
    decoded[dlen] = '\0';
    if (strcmp((char*)decoded, "Hello World") == 0) {
        printf("✓ base64_decode test passed\n");
    } else {
        printf("✗ base64_decode test failed: got '%s'\n", decoded);
        return 1;
    }

    // Test clamp
    if (clamp(5, 0, 10) == 5 && clamp(-1, 0, 10) == 0 && clamp(15, 0, 10) == 10) {
        printf("✓ clamp test passed\n");
    } else {
        printf("✗ clamp test failed\n");
        return 1;
    }

    // Test read_temp (only if sensor exists)
    int temp = read_temp();
    if (temp >= 0) {
        printf("✓ read_temp test passed (temp: %d°C)\n", temp);
    } else {
        printf("⚠ read_temp test skipped (no sensor available)\n");
    }

    printf("All tests completed.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    char *log_path = NULL;
    saved_argv = argv; // keep argv for potential execv on restart
    // Load config file first (CLI args will override)
    load_config_file();
    log_level = LOGLEVEL_VERBOSE; // Override config for debugging
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logfile = fopen(argv[++i], "a");
            log_path = argv[i];
            if (!logfile) {
                fprintf(stderr, "Error: Cannot open log file: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            strncpy(temp_path, argv[++i], sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path)-1] = '\0';
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
        } else if (strcmp(argv[i], "--test") == 0) {
            return run_tests();
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
    const int poll_timeout_ms = POLL_TIMEOUT_MS; // wake up periodically to handle tasks

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
        if (elapsed_ms >= TEMP_READ_INTERVAL_MS) {
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

            int new_freq = max_freq;
            int throttle_start = temp_max - THROTTLE_START_OFFSET; // Start throttling THROTTLE_START_OFFSET°C below temp_max for gentler curve
            int hysteresis = HYSTERESIS; // °C hysteresis to prevent oscillations

            // Calculate target frequency
            int target_freq = max_freq;
            if (temp >= temp_max) {
                target_freq = safe_min > 0 ? safe_min : min_freq; // Don't go below safe_min
            } else if (temp >= throttle_start) {
                // Linear scaling from max_freq at throttle_start to 50% of max_freq at temp_max
                int temp_range = temp_max - throttle_start;
                int freq_range = max_freq / 2; // Scale down to 50% max_freq, not to min_freq
                int temp_above_start = temp - throttle_start;
                target_freq = max_freq - (freq_range * temp_above_start) / temp_range;
                if (target_freq < safe_min && safe_min > 0) target_freq = safe_min;
            }

            // Apply hysteresis: only change if temp deviates significantly from last throttle point
            if (abs(temp - last_throttle_temp) >= hysteresis || last_throttle_temp == 0) {
                new_freq = target_freq;
                last_throttle_temp = temp;
            } else {
                new_freq = current_freq; // keep current frequency
            }

            if (safe_min > 0 && temp < temp_max && new_freq < safe_min) new_freq = safe_min;

            current_freq = new_freq;
            if (abs(new_freq - last_freq) > (max_freq - min_freq) / 10) {
                set_max_freq_all_cpus(new_freq);
                rotate_log_file(log_path);
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
    if (should_restart) {
        LOG_INFO("Restarting daemon...\n");
        // Re-exec the same binary with original arguments if available
        if (saved_argv) {
            execv(saved_argv[0], saved_argv);
            perror("execv");
        }
        return 1;
    }
    LOG_INFO("Shutting down gracefully...\n");
    free_cpu_cache();
    return 0;
}