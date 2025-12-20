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

// Sensors mode: flattened list of HWMon sensors and thermal zones for TUI
typedef struct sensor_entry { int kind; /*0=hwmon,1=zone*/ char name[256]; char path[512]; int temp; int excluded; int zone; char type[64]; } sensor_entry_t;
static sensor_entry_t sensors_arr[512];
static int sensors_count = 0;
static int sensors_sel = 0;
static int sensors_offset = 0;
// Filtered display indices (indexes into sensors_arr) and count
static int sensors_display_idx[512];
static int sensors_display_count = 0;

// Parse combined sensors JSON into sensors_arr for the TUI
static void parse_sensors_json(const char *json) {
    // reset
    sensors_count = 0;
    if (!json || *json == '\0') return;
    const char *p = json;
    // find hwmons array
    const char *hw = strstr(p, "\"hwmons\":[");
    if (hw) {
        const char *a = strchr(hw, '[');
        if (a) {
            const char *q = a + 1;
            while (*q && *q != ']') {
                // Look for sensor objects within device
                const char *dev_start = strchr(q, '{'); if (!dev_start) break;
                // find matching '}' for this device (handle nested braces)
                const char *dev_end = dev_start; int depth = 1; for (const char *t = dev_start + 1; *t && depth > 0; ++t) { if (*t == '{') depth++; else if (*t == '}') depth--; dev_end = t; }
                if (depth != 0) break;
                // crude parse for id and name
                char dev_id[128] = ""; char dev_name[128] = "";
                const char *id_pos = strstr(dev_start, "\"id\":"); if (id_pos) { const char *v = strchr(id_pos, ':'); if (v) { const char *s = strchr(v, '"'); if (s) { s++; const char *e = strchr(s, '"'); if (e) { int len = (int)(e - s); if (len >= (int)sizeof(dev_id)) len = (int)sizeof(dev_id)-1; snprintf(dev_id, sizeof(dev_id), "%.*s", len, s); } } } }
                const char *name_pos = strstr(dev_start, "\"name\":"); if (name_pos) { const char *v = strchr(name_pos, ':'); if (v) { const char *s = strchr(v, '"'); if (s) { s++; const char *e = strchr(s, '"'); if (e) { int len = (int)(e - s); if (len >= (int)sizeof(dev_name)) len = (int)sizeof(dev_name)-1; snprintf(dev_name, sizeof(dev_name), "%.*s", len, s); } } } }
                // Find sensors array inside device
                const char *sensors_a = strstr(dev_start, "\"sensors\":"); if (sensors_a) {
                    const char *sa = strchr(sensors_a, '['); if (!sa) { q = dev_end + 1; continue; }
                    const char *s = sa + 1;
                    while (*s && *s != ']') {
                        const char *obj = strchr(s, '{'); if (!obj) break;
                        const char *objend = obj; int d = 1; for (const char *t2 = obj + 1; *t2 && d > 0; ++t2) { if (*t2 == '{') d++; else if (*t2 == '}') d--; objend = t2; }
                        if (d != 0) break;
                        // extract label, path, temp, excluded
                        char label[256] = ""; char path[512] = ""; int temp = -1; int excluded = 0;
                        const char *lab = strstr(obj, "\"label\":"); if (lab) { const char *v = strchr(lab, ':'); if (v) { const char *ss = strchr(v, '"'); if (ss) { ss++; const char *ee = strchr(ss, '"'); if (ee) { int len = (int)(ee - ss); if (len >= (int)sizeof(label)) len = (int)sizeof(label)-1; snprintf(label, sizeof(label), "%.*s", len, ss); } } } }
                        const char *pp = strstr(obj, "\"path\":"); if (pp) { const char *v = strchr(pp, ':'); if (v) { const char *ss = strchr(v, '"'); if (ss) { ss++; const char *ee = strchr(ss, '"'); if (ee) { int len = (int)(ee - ss); if (len >= (int)sizeof(path)) len = (int)sizeof(path)-1; snprintf(path, sizeof(path), "%.*s", len, ss); } } } }
                        const char *tp = strstr(obj, "\"temp\":"); if (tp) { tp = strchr(tp, ':'); if (tp) { tp++; temp = atoi(tp); } }
                        const char *ex = strstr(obj, "\"excluded\":true"); if (ex) excluded = 1;
                        // build entry
                        if (sensors_count < (int)(sizeof(sensors_arr)/sizeof(sensors_arr[0]))) {
                            sensors_arr[sensors_count].kind = 0;
                            snprintf(sensors_arr[sensors_count].name, sizeof(sensors_arr[sensors_count].name), "%s %s", dev_name[0] ? dev_name : dev_id, label[0] ? label : "(sensor)");
                            snprintf(sensors_arr[sensors_count].path, sizeof(sensors_arr[sensors_count].path), "%s", path);
                            sensors_arr[sensors_count].temp = temp;
                            sensors_arr[sensors_count].excluded = excluded;
                            sensors_arr[sensors_count].zone = -1;
                            sensors_arr[sensors_count].type[0] = '\0';
                            sensors_count++;
                        }
                        s = objend + 1;
                    }
                }
                q = dev_end + 1;
            }
        }
    }
    // parse zones array
    const char *z = strstr(p, "\"zones\":[");
    if (z) {
        const char *za = strchr(z, '['); if (za) {
            const char *q = za + 1;
            while (*q && *q != ']') {
                const char *obj = strchr(q, '{'); if (!obj) break; const char *objend = strchr(obj, '}'); if (!objend) break;
                int zone = -1; char type[64] = ""; int temp = -1; int excluded = 0;
                const char *zonep = strstr(obj, "\"zone\":"); if (zonep) { zonep = strchr(zonep, ':'); if (zonep) zone = atoi(zonep+1); }
                const char *tp = strstr(obj, "\"type\":"); if (tp) { const char *ss = strchr(tp, '"'); if (ss) { ss++; const char *ee = strchr(ss, '"'); if (ee) { int len = (int)(ee - ss); if (len >= (int)sizeof(type)) len = (int)sizeof(type)-1; snprintf(type, sizeof(type), "%.*s", len, ss); } } }
                const char *tval = strstr(obj, "\"temp\":"); if (tval) { const char *ss = tval; ss = strchr(ss, ':'); if (ss) ss++; temp = atoi(ss); }
                const char *ex = strstr(obj, "\"excluded\":true"); if (ex) excluded = 1;
                if (sensors_count < (int)(sizeof(sensors_arr)/sizeof(sensors_arr[0]))) {
                    sensors_arr[sensors_count].kind = 1;
                    snprintf(sensors_arr[sensors_count].name, sizeof(sensors_arr[sensors_count].name), "Zone %d (%s)", zone, type);
                    snprintf(sensors_arr[sensors_count].path, sizeof(sensors_arr[sensors_count].path), "/sys/class/thermal/thermal_zone%d/temp", zone);
                    sensors_arr[sensors_count].temp = temp;
                    sensors_arr[sensors_count].excluded = excluded;
                    sensors_arr[sensors_count].zone = zone;
                    snprintf(sensors_arr[sensors_count].type, sizeof(sensors_arr[sensors_count].type), "%s", type);
                    sensors_count++;
                }
                q = objend + 1;
            }
        }
    }
}

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

// Pretty print just the first-level key:value pairs of a JSON object
// Strips JSON punctuation (braces, quotes, trailing commas) and returns a buffer
char* pretty_print_status_kv(const char *json) {
    static char buf[4096];
    char *out = buf; buf[0] = '\0';
    if (!json || *json == '\0') return buf;
    const char *p = json;
    // skip to first opening '{'
    while (*p && *p != '{') p++;
    if (!p || *p != '{') return buf;
    p++; // move inside object
    while (*p && *p != '}') {
        // skip whitespace & commas
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == '}') break;
        // get key (assume "key")
        if (*p == '"') {
            p++; const char *kstart = p; char key[128] = "";
            while (*p && *p != '"') p++;
            if (*p == '"') {
                size_t klen = (size_t)(p - kstart);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                snprintf(key, sizeof(key), "%.*s", (int)klen, kstart);
                p++; // after closing quote
                // skip spaces and colon
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == ':') { p++; }
                while (*p && isspace((unsigned char)*p)) p++;
                // value: could be string, number, object, array
                if (*p == '"') {
                    p++; const char *vstart = p; while (*p && *p != '"') p++; size_t vlen = (size_t)(p - vstart); char val[256] = "";
                    if (vlen >= (int)sizeof(val)) { vlen = sizeof(val) - 1; }
                    snprintf(val, sizeof(val), "%.*s", (int)vlen, vstart);
                        p++; // skip closing quote
                    out += sprintf(out, "%s: %s\n", key, val);
                } else if (*p == '{') {
                    // nested object, skip until matching '}' and show placeholder
                    int depth = 1; p++;
                    while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
                    out += sprintf(out, "%s: {...}\n", key);
                } else if (*p == '[') {
                    // array: skip until matching ']'
                    int depth = 1; p++;
                    while (*p && depth > 0) { if (*p == '[') depth++; else if (*p == ']') depth--; p++; }
                    out += sprintf(out, "%s: [...]\n", key);
                } else {
                    // number or bareword: read until comma or closing brace
                    const char *vstart = p; while (*p && *p != ',' && *p != '}') p++; size_t vlen = (size_t)(p - vstart); char val[256] = "";
                    // strip whitespace at end
                    while (vlen > 0 && isspace((unsigned char)vstart[vlen-1])) vlen--;
                    // trim trailing commas if any
                    if (vlen > 0 && vstart[vlen-1] == ',') vlen--;
                    if (vlen >= (int)sizeof(val)) { vlen = sizeof(val) - 1; }
                    snprintf(val, sizeof(val), "%.*s", (int)vlen, vstart);
                    // trim leading whitespace
                    char *vp = val; while (*vp && isspace((unsigned char)*vp)) vp++;
                    out += sprintf(out, "%s: %s\n", key, vp);
                }
            } else break; // no matching key end
        } else {
            // Not a quoted key: skip until next comma or brace
            while (*p && *p != ',' && *p != '}') p++;
            if (*p == ',') p++;
        }
    }
    // ensure buffer terminates
    out[0] = buf[0];
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
                                snprintf(zones_arr[zones_count].type, sizeof(zones_arr[zones_count].type), "%s", type);
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

// First-level status JSON to nice key: value representation for the TUI status pane
char* format_status_kv(const char* json) {
    static char buf[4096]; char temp[128]; buf[0] = '\0';
    if (!json || !json[0]) return buf;
    // Extract a numeric or string value following a key
    char *p;
    // temperature
    p = strstr(json, "\"temperature\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[64] = ""; size_t i = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof(val)-1) val[i++] = *p++; }
            else { while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) { val[i++] = *p++; } }
            val[i] = '\0';
            if (i) { snprintf(temp, sizeof(temp), "temperature: %s °C", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // frequency
    p = strstr(json, "\"frequency\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[64] = ""; size_t i = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof(val)-1) val[i++] = *p++; }
            else { while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) { val[i++] = *p++; } }
            val[i] = '\0';
            if (i) { snprintf(temp, sizeof(temp), "frequency: %s kHz", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // safe_min
    p = strstr(json, "\"safe_min\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[64] = ""; size_t i = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof(val)-1) val[i++] = *p++; }
            else { while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) { val[i++] = *p++; } }
            val[i] = '\0'; if (i) { snprintf(temp, sizeof(temp), "safe_min: %s kHz", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // safe_max
    p = strstr(json, "\"safe_max\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[64] = ""; size_t i = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof(val)-1) val[i++] = *p++; }
            else { while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) { val[i++] = *p++; } }
            val[i] = '\0'; if (i) { snprintf(temp, sizeof(temp), "safe_max: %s kHz", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // running_user
    p = strstr(json, "\"running_user\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[128] = ""; size_t i = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof(val)-1) val[i++] = *p++; }
            else { while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) { val[i++] = *p++; } }
            val[i] = '\0'; if (i) { snprintf(temp, sizeof(temp), "running_user: %s", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // use_avg_temp
    p = strstr(json, "\"use_avg_temp\"");
    if (p) {
        p = strchr(p, ':'); if (p) p++; while (p && *p && isspace((unsigned char)*p)) p++;
        if (p) {
            char val[16] = ""; size_t i = 0; while (*p && *p != ',' && *p != '}' && i < sizeof(val)-1) val[i++] = *p++; val[i] = '\0';
            if (i) { snprintf(temp, sizeof(temp), "use_avg_temp: %s", val); strncat(buf, temp, sizeof(buf)-strlen(buf)-1); strncat(buf, "\n", sizeof(buf)-strlen(buf)-1); }
        }
    }
    // fallback: if nothing was extracted, clean up the raw JSON slightly
    if (buf[0] == '\0') {
        char tmp[4096], out[4096]; snprintf(tmp, sizeof(tmp), "%s", json);
        for (char *q = tmp; *q; ++q) { if (*q == '{' || *q == '}' || *q == '"') *q = ' '; }
        for (char *q = tmp; *q; ++q) { if (*q == ',') *q = ' '; }
        // collapse whitespace
        char *s = tmp; char *d = out; while (*s && (d - out) < (int)sizeof(out)-1) {
            if (isspace((unsigned char)*s)) { *d++ = ' '; while (isspace((unsigned char)*s)) s++; }
            else *d++ = *s++;
        }
        *d = '\0'; snprintf(buf, sizeof(buf), "%s", out);
    }
    return buf;
}

// Shared state updated by poller/worker threads
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t last_msg_lock = PTHREAD_MUTEX_INITIALIZER;
static char status_buf[4096] = "(no status yet)";
static time_t status_ts = 0;
static char limits_buf[4096] = "(no limits yet)";
static time_t limits_ts = 0;
// Sensors JSON (combined hwmons+zones)
static char sensors_buf[16384] = "(no sensors yet)";
static time_t sensors_ts = 0;
static char skins_buf[4096] = "(no skins yet)";
static time_t skins_ts = 0;
static char last_msg[256] = "";
static volatile int keep_running = 1;
static volatile int help_visible = 0;
static int help_offset = 0;
static volatile int show_raw_status = 0; // toggle to show full raw JSON status in popup
static int data_horiz_offset = 0; // horizontal scroll for data panes
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
// Limits selection state
static int limits_sel = 0;
static int limits_offset = 0;
static int limits_count = 0;


// Helper: get current value of use_avg_temp from status_buf; returns 0 or 1 or -1 if not available
static int get_use_avg_temp(void) {
    pthread_mutex_lock(&state_lock);
    char local[4096]; snprintf(local, sizeof(local), "%s", status_buf);
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
    char local[4096]; snprintf(local, sizeof(local), "%s", status_buf);
    pthread_mutex_unlock(&state_lock);
    char *p = strstr(local, "\"running_user\"");
    if (!p) return -1;
    p = strchr(p, ':'); if (!p) return -1; p++;
    while (*p && isspace((unsigned char)*p)) p++;
    // accept "name" or plain value
    if (*p == '"') {
        p++;
        char *q = strchr(p, '"'); if (!q) return -1;
        size_t len = q - p;
        if (len >= outsz) len = outsz - 1;
        snprintf(out, outsz, "%.*s", (int)len, p);
        // trim whitespace
        while (len > 0 && isspace((unsigned char)out[len-1])) { out[--len] = '\0'; }
        while (*out && isspace((unsigned char)*out)) { memmove(out, out+1, strlen(out)); }
        return 0;
    } else {
        char *q = p; size_t len = 0; while (*q && !isspace((unsigned char)*q) && *q != ',' && *q != '}') { q++; len++; }
        if (len >= outsz) len = outsz - 1;
        snprintf(out, outsz, "%.*s", (int)len, p);
        // trim trailing whitespace
        while (len > 0 && isspace((unsigned char)out[len-1])) { out[--len] = '\0'; }
        while (*out && isspace((unsigned char)*out)) { memmove(out, out+1, strlen(out)); }
        return 0;
    }
}

static int get_web_port_from_status(void) {
    pthread_mutex_lock(&state_lock);
    char local[4096]; snprintf(local, sizeof(local), "%s", status_buf);
    pthread_mutex_unlock(&state_lock);
    char *p = strstr(local, "\"web_port\"");
    if (!p) return 0;
    p = strchr(p, ':'); if (!p) return 0; p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int val = atoi(p);
    return val;
}

// Print a window line with optional horizontal scroll and ellipsis for non-selected rows
static void mvwprintw_scrollable(WINDOW *win, int row, int col, int width, const char *s, int offset, int selected) {
    size_t len = strlen(s);
        if ((int)len <= width) { 
            mvwprintw(win, row, col, "%s", s);
            wmove(win, row, col + width); wclrtoeol(win);
        return;
    }
        if (!selected) {
        // show left anchored with ellipsis
        if (width > 3) {
                char buf[512]; int blen = width - 3; if (blen < 0) blen = 0; if (blen > (int)sizeof(buf)-1) blen = (int)sizeof(buf)-1; snprintf(buf, sizeof(buf), "%.*s", blen, s);
            mvwprintw(win, row, col, "%s...", buf);
            wmove(win, row, col + width); wclrtoeol(win);
        } else {
            mvwprintw(win, row, col, "%.*s", width, s);
            wmove(win, row, col + width); wclrtoeol(win);
        }
        return;
    }
    // selected: show substring starting at offset, add ellipsis if truncated on either end
    if (offset < 0) offset = 0;
    int ellWidth = 3; // '...'
    int leftEll = (offset > 0) ? 1 : 0;
    // provisional avail conservatively assumes no right ellipsis
    int avail = width - (leftEll ? ellWidth : 0);
    if (avail < 1) avail = 1;
    // if even with left ellipsis width we still have characters after offset, maybe right ell
    int rightEll = ((offset + avail) < (int)len) ? 1 : 0;
    // recompute avail with both ellipses considered
    avail = width - (leftEll ? ellWidth : 0) - (rightEll ? ellWidth : 0);
    if (avail < 1) avail = 1;
    int maxOffset = (int)len - avail;
    if (maxOffset < 0) maxOffset = 0;
    if (offset > maxOffset) offset = maxOffset;
    const char *start = s + offset;
    char buf2[4096]; int av = avail; if (av < 0) av = 0; if (av > (int)sizeof(buf2)-1) av = (int)sizeof(buf2)-1; snprintf(buf2, sizeof(buf2), "%.*s", av, start);
    int x = col;
    if (leftEll) { mvwprintw(win, row, x, "..."); x += 3; }
    mvwprintw(win, row, x, "%s", buf2); x += avail;
    if (rightEll) { mvwprintw(win, row, x, "..."); }
    wmove(win, row, col + width); wclrtoeol(win);
}

// Count how many wrapped lines a single paragraph string will produce for given width
static int wrapped_count(const char *s, int width) {
    if (!s || width <= 0) return 0;
    int count = 0;
    const char *p = s;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++; // skip spaces
        if (!*p) break;
        int line_len = 0;
        const char *q = p;
        while (*q && !isspace((unsigned char)*q)) q++; // advance to end of first word
        // If the first word is longer than the width, it will occupy ceil(len/width) pieces
        if ((int)(q - p) > width) {
            int leftover = (int)(q - p);
            count += (leftover + width - 1) / width;
            // skip the very long word
            while (*p && !isspace((unsigned char)*p)) p++;
            continue;
        }
        // else, try to accumulate words into the current line
        line_len = 0;
        const char *wp = p;
        while (wp && *wp) {
            // look for next word
            const char *r = wp; int wlen = 0; while (*r && !isspace((unsigned char)*r)) { wlen++; r++; }
            if (line_len == 0) {
                if (wlen > width) { // split long word
                    count += (wlen + width - 1) / width;
                    // move wp to after the word
                    while (*wp && !isspace((unsigned char)*wp)) wp++;
                } else { line_len = wlen; wp = r; while (*wp && isspace((unsigned char)*wp)) wp++; }
            } else {
                if (line_len + 1 + wlen <= width) { line_len += 1 + wlen; wp = r; while (*wp && isspace((unsigned char)*wp)) wp++; }
                else break;
            }
            if (!*wp) break;
        }
        count++;
        // advance p to after the line we just counted (wp)
        if (wp == p) { // safeguard to avoid infinite loop
            while (*p && !isspace((unsigned char)*p)) p++;
        } else p = wp;
    }
    return count;
}

// Retrieve the 'target' wrapped line (0-indexed) for paragraph s using width, write to out
static void get_wrapped_line(const char *s, int width, int target, char *out, size_t out_sz) {
    if (!s || width <= 0 || !out || out_sz == 0) { if (out && out_sz) out[0] = '\0'; return; }
    const char *p = s; int idx = 0; out[0] = '\0';
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        int line_len = 0;
        const char *wp = p; const char *r = wp;
        while (r && *r) {
            int wlen = 0; const char *qq = r; while (*qq && !isspace((unsigned char)*qq)) { wlen++; qq++; }
            if (line_len == 0) {
                if (wlen > width) {
                    // This long word slices into pieces
                    int pieces = (wlen + width - 1) / width;
                    int piece = 0; const char *lp = r;
                    while (piece < pieces) {
                        int take = (wlen > width) ? width : wlen;
                                if (idx == target) {
                                    int len = (take < (int)out_sz - 1) ? take : (int)out_sz - 1;
                                    snprintf(out, out_sz, "%.*s", len, lp); return;
                        }
                        idx++; piece++; lp += take; wlen -= take;
                    }
                    // skip the rest of the long word
                    while (*r && !isspace((unsigned char)*r)) r++;
                    while (*r && isspace((unsigned char)*r)) r++;
                    wp = r; line_len = 0; continue;
                } else { line_len = wlen; r = qq; while (*r && isspace((unsigned char)*r)) r++; }
            } else {
                if (line_len + 1 + wlen <= width) { line_len += 1 + wlen; r = qq; while (*r && isspace((unsigned char)*r)) r++; }
                else break;
            }
            if (!*r) break;
        }
        // If current concatenation builds a line from wp to r
        if (idx == target) {
            int len = (int)(r - wp);
            if (len >= (int)out_sz) len = (int)out_sz - 1;
            snprintf(out, out_sz, "%.*s", len, wp); return;
        }
        idx++;
        p = r;
    }
    out[0] = '\0';
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
        snprintf(orig, bufsz, "%s", buf);
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
                snprintf(buf, bufsz, "%s", orig);
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
static void *worker_start_daemon(void *v) __attribute__((unused));
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
    snprintf(last_msg, sizeof(last_msg), "%.255s", tmp);
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
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
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
        char buf[512]; snprintf(buf, sizeof(buf), "%s", a->cmd + 10);
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
    snprintf(a->cmd, sizeof(a->cmd), "%s", cmd);
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
        snprintf(status_buf, sizeof(status_buf), "%s", r);
        status_buf[sizeof(status_buf)-1] = '\0';
        free(r);
        status_ts = time(NULL);
        set_last_msg("Status refreshed");
    } else {
        snprintf(status_buf, sizeof(status_buf), "(daemon unreachable)");
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

// spawn a worker to fetch sensors (hwmons + zones) now and update state (non-blocking)
static void *worker_fetch_sensors(void *v) {
    (void)v;
    char *r = send_unix_command("sensors json");
    pthread_mutex_lock(&state_lock);
    if (r) {
        snprintf(sensors_buf, sizeof(sensors_buf), "%s", r);
        sensors_buf[sizeof(sensors_buf)-1] = '\0';
        free(r);
        sensors_ts = time(NULL);
        set_last_msg("Sensors refreshed");
    } else {
        snprintf(sensors_buf, sizeof(sensors_buf), "(daemon unreachable)");
        sensors_buf[sizeof(sensors_buf)-1] = '\0';
    }
    pthread_mutex_unlock(&state_lock);
    return NULL;
}

static void spawn_fetch_sensors_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_fetch_sensors, NULL);
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
    snprintf(id, sizeof(id), "%.*s", (int)len, id_start);
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
    snprintf(name, sizeof(name), "%.255s", base);
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
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
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
    snprintf(a->name, sizeof(a->name), "%s", name);
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_load_profile, a); pthread_attr_destroy(&attr);
}

static void spawn_default_skin_async(void) {
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, worker_default_skin, NULL); pthread_attr_destroy(&attr);
}

static void spawn_install_skin_async(const char *path) {
    struct install_arg *a = calloc(1, sizeof(*a)); if (!a) return;
    snprintf(a->path, sizeof(a->path), "%s", path);
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
    char base[512]; snprintf(base, sizeof(base), "%s", tmp);
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
            snprintf(names[i], sizeof(names[i]), "%.255s", line);
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
            snprintf(status_buf, sizeof(status_buf), "%s", r);
            status_buf[sizeof(status_buf)-1] = '\0';
            free(r);
            status_ts = time(NULL);
        } else {
            snprintf(status_buf, sizeof(status_buf), "(daemon unreachable)");
            status_buf[sizeof(status_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        // Update limits
        r = send_unix_command("limits json");
        pthread_mutex_lock(&state_lock);
        if (r) {
            snprintf(limits_buf, sizeof(limits_buf), "%s", r);
            limits_buf[sizeof(limits_buf)-1] = '\0';
            free(r);
            limits_ts = time(NULL);
        } else {
            snprintf(limits_buf, sizeof(limits_buf), "(daemon unreachable)");
            limits_buf[sizeof(limits_buf)-1] = '\0';
        }
        pthread_mutex_unlock(&state_lock);

        // Update sensors (combined hwmons + zones)
        r = send_unix_command("sensors json");
        pthread_mutex_lock(&state_lock);
        if (r) {
            snprintf(sensors_buf, sizeof(sensors_buf), "%s", r);
            free(r);
            sensors_ts = time(NULL);
        } else {
            snprintf(sensors_buf, sizeof(sensors_buf), "(daemon unreachable)");
        }
        pthread_mutex_unlock(&state_lock);

        // Update skins
        r = send_unix_command("list-skins");
        pthread_mutex_lock(&state_lock);
        if (r) {
            snprintf(skins_buf, sizeof(skins_buf), "%s", r);
            free(r);
            skins_ts = time(NULL);
        } else {
            snprintf(skins_buf, sizeof(skins_buf), "(daemon unreachable)");
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
    snprintf(cmd, sizeof(cmd), "load-profile %.114s", name);
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
    // Show the help pane by default when the terminal width is large enough
    if (width >= 100) help_visible = 1;
    WINDOW *status = newwin(7, width-2, 1, 1);
    int data_w = help_visible ? (width - 44) : (width - 2);
    if (data_w < 20) data_w = 20;
    WINDOW *data = newwin(height-13, data_w, 9, 1);
    // Make help window match the data window height so it can show all lines; hidden = 0 width
    WINDOW *helpwin = NULL;
    if (help_visible) helpwin = newwin(height-13, width - data_w - 3, 9, data_w + 2);
    WINDOW *inputwin = newwin(3, width-2, height-6, 1);
    // footer window occupies last two lines: Msg at row 0, keys at row 1
    WINDOW *footerwin = newwin(2, width, height-2, 0);
    mvprintw(0,2,"cpu_throttle TUI");
    mvprintw(0,20,"Press 'h' for help | Tab: switch mode");
    refresh();

    // start background poller
    pthread_t poller;
    if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
        // failed to start poller, continue but UI will show no updates
        pthread_mutex_lock(&state_lock); snprintf(status_buf, sizeof(status_buf), "(poller failed)"); pthread_mutex_unlock(&state_lock);
    }

    char profs[256][256]; int prof_count = 0; int sel = 0; int offset = 0;
    char display_profs[256][256]; int display_count = 0;
    timeout(200); // responsive UI: 200ms

    // (using prompt_input and prompt_char helpers defined above)

    while (1) {
        // recreate/resized windows on change (help visible toggles size)
        int new_data_w = help_visible ? (width - 44) : (width - 2);
        if (new_data_w < 20) new_data_w = 20;
        // recalc terminal size on each loop (in case of resize)
        getmaxyx(stdscr, height, width);
        if (new_data_w != getmaxx(data)) {
            delwin(data);
            data = newwin(height-13, new_data_w, 9, 1);
            if (inputwin) {
                wresize(inputwin, 3, width-2);
                mvwin(inputwin, height-6, 1);
            } else {
                inputwin = newwin(3, width-2, height-6, 1);
            }
            if (footerwin) {
                wresize(footerwin, 2, width);
                mvwin(footerwin, height-2, 0);
            } else {
                footerwin = newwin(2, width, height-2, 0);
            }
            // Ensure the main screen is repainted and clear any remnants from previous window sizes
            touchwin(stdscr);
            refresh();
        }
        // (recreate help if needed)
        if (help_visible) {
            if (!helpwin) helpwin = newwin(height-13, width - new_data_w - 3, 9, new_data_w + 2);
            else wresize(helpwin, height-13, width - new_data_w - 3);
        } else {
            if (helpwin) { delwin(helpwin); helpwin = NULL; touchwin(stdscr); refresh(); }
        }
        werase(status); box(status, 0,0);
        mvwprintw(status, 0,2," Status ");
        // display cached status (updated by poller)
        pthread_mutex_lock(&state_lock);
        char local_status[4096]; snprintf(local_status, sizeof(local_status), "%s", status_buf);
        time_t ts = status_ts;
        pthread_mutex_unlock(&state_lock);
        char local_msg[256];
        pthread_mutex_lock(&last_msg_lock); snprintf(local_msg, sizeof(local_msg), "%.200s", last_msg); pthread_mutex_unlock(&last_msg_lock);
        // print response lines
        int cur_avg = get_use_avg_temp();
        // Prepare pretty printed status if JSON, and compute columns dynamically
        char pretty_status[4096]; pretty_status[0] = '\0';
        char status_summary[4096]; status_summary[0] = '\0';
        if (local_status[0] == '{' || local_status[0] == '[') {
            // Keep pretty JSON for raw popup
            snprintf(pretty_status, sizeof(pretty_status), "%s", pretty_print_json(local_status));
            // Use key/value formatted string with units for the left pane
            snprintf(status_summary, sizeof(status_summary), "%s", format_status_kv(local_status));
        } else {
            snprintf(pretty_status, sizeof(pretty_status), "%s", local_status);
            snprintf(status_summary, sizeof(status_summary), "%s", local_status);
        }
        int maxx_status = getmaxx(status);
        int h_status = getmaxy(status);
        int desired_right_col_width = (maxx_status >= 100) ? 36 : (maxx_status >= 80 ? 28 : 20);
        int min_left_width = 20; int min_right_width = 12;
        int right_col_width = desired_right_col_width;
        if (right_col_width > maxx_status/3) right_col_width = maxx_status/3;
        int left_max_width = maxx_status - right_col_width - 6;
        if (left_max_width < min_left_width) {
            // adjust right_col_width to give left column some space
            right_col_width = maxx_status - min_left_width - 6;
            if (right_col_width < min_right_width) right_col_width = min_right_width;
            left_max_width = maxx_status - right_col_width - 6;
            if (left_max_width < 0) left_max_width = 0;
        }
        int right_col = 2 + left_max_width + 2;
        // left pane may vary in height; reserve one line for 'Updated' and one for the bottom border
        int max_left_lines = h_status - 3; if (max_left_lines < 1) max_left_lines = 1;
        int line = 1; char *saveptr = NULL; char *p = strtok_r(status_summary, "\n", &saveptr);
        while (p && line <= max_left_lines) {
            size_t plen = strlen(p);
            size_t offset = 0;
            while (offset < plen && line <= max_left_lines) {
                mvwprintw(status, line, 2, "%.*s", left_max_width, p + offset);
                offset += left_max_width;
                line++;
            }
            p = strtok_r(NULL, "\n", &saveptr);
        }
        int updated_row = max_left_lines + 1;
        if (ts != 0) mvwprintw(status, updated_row, 2, "Updated: %ld sec ago", (long)(time(NULL)-ts));
        else mvwprintw(status, updated_row, 2, "No update yet");

        // Now print the right-column values to ensure they have priority and won't be overwritten by left column
        char avg_buf[64]; snprintf(avg_buf, sizeof(avg_buf), "Avg temp usage: %s", cur_avg < 0 ? "n/a" : (cur_avg ? "true" : "false"));
        if (1 <= max_left_lines) mvwprintw(status, 1, right_col, "%.*s", right_col_width, avg_buf);
        char run_user[128] = "";
        if (get_running_user(run_user, sizeof(run_user)) == 0) {
            const char *label = "Daemon user: ";
            int right_space = right_col_width - (int)strlen(label);
            if (right_space <= 0) right_space = 4; // fallback minimal space
            if (2 <= max_left_lines) mvwprintw(status, 2, right_col, "%s%.*s", label, right_space, run_user);
            if (strcmp(run_user, "root") != 0) {
                if (3 <= max_left_lines) mvwprintw(status, 3, right_col, "%.*s", right_space, "Warning: daemon not running as root; settings may only be saved per user");
            }
        }
        // (left column printed above)
        // (sparklines disabled)
        wrefresh(status);
        // redraw header (prevents artefacts on top line when windows resize)
        mvprintw(0,2,"cpu_throttle TUI"); mvprintw(0,20,"Press 'h' for help | Tab: switch mode"); clrtoeol();
        // If user toggles raw status view, show a popup and wait for keypress
        if (show_raw_status) {
            int raw_h = getmaxy(stdscr) - 4; if (raw_h > 30) raw_h = 30; if (raw_h < 5) raw_h = 5;
            int raw_w = getmaxx(stdscr) - 4; if (raw_w < 40) raw_w = getmaxx(stdscr) - 2;
            int raw_y = 2; int raw_x = 2;
            WINDOW *rawwin = newwin(raw_h, raw_w, raw_y, raw_x);
            box(rawwin, 0, 0);
            mvwprintw(rawwin, 0, 2, " Status JSON (press any key to close) ");
            // print the pretty_status lines into the popup
            int row = 1; char bufcopy[4096]; snprintf(bufcopy, sizeof(bufcopy), "%s", pretty_status);
            char *save = NULL; char *linep = strtok_r(bufcopy, "\n", &save);
            while (linep && row < raw_h - 1) {
                mvwprintw(rawwin, row, 2, "%.*s", raw_w - 4, linep);
                row++; linep = strtok_r(NULL, "\n", &save);
            }
            wrefresh(rawwin);
            wgetch(rawwin); // wait for any key
            delwin(rawwin);
            show_raw_status = 0; // automatically close
        }

        // determine web UI presence
        int web_port = get_web_port_from_status();
        int webui_enabled = (web_port > 0) ? 1 : 0;
        // data window - depends on current_mode
        werase(data); box(data,0,0);
        const char *all_mode_names[] = {"Limits", "Sensors", "Profiles", "Skins"};
        const char *mode_names[4];
        int local_mode_count = 0;
        mode_names[local_mode_count++] = all_mode_names[0];
        mode_names[local_mode_count++] = all_mode_names[1];
        mode_names[local_mode_count++] = all_mode_names[2];
        if (webui_enabled) mode_names[local_mode_count++] = all_mode_names[3];
        // allow the mode count to be dynamic
        int mode_count_local = local_mode_count;
        if (current_mode >= mode_count_local) current_mode = mode_count_local - 1;
        mvwprintw(data,0,2," %s ", mode_names[current_mode]);
        int page_lines = (height-13) - 2;
            if (current_mode == 0) { // Limits
            pthread_mutex_lock(&state_lock);
            char local_limits[4096]; snprintf(local_limits, sizeof(local_limits), "%s", limits_buf);
            time_t ts = limits_ts;
            pthread_mutex_unlock(&state_lock);
            char *pretty = format_limits_zones(local_limits, 1);
            int line=1; char *saveptr = NULL; char *p = strtok_r(pretty, "\n", &saveptr);
            // Build list of lines to track selection index
            char lines_buf[256][256]; limits_count = 0;
            while (p && limits_count < (int)(sizeof(lines_buf)/sizeof(lines_buf[0]))) {
                snprintf(lines_buf[limits_count], sizeof(lines_buf[0]), "%s", p);
                limits_count++; p = strtok_r(NULL, "\n", &saveptr);
            }
            if (limits_sel >= limits_count) limits_sel = limits_count > 0 ? limits_count - 1 : 0;
            if (limits_sel < 0) limits_sel = 0;
            if (limits_sel < limits_offset) limits_offset = limits_sel;
            if (limits_sel >= limits_offset + page_lines) limits_offset = limits_sel - page_lines + 1;
            for (int i = limits_offset; i < limits_count && line <= page_lines; ++i) {
                int row = line;
                int sel = (i == limits_sel);
                if (sel) wattron(data, A_REVERSE);
                mvwprintw_scrollable(data, row, 2, getmaxx(data)-4, lines_buf[i], data_horiz_offset, sel);
                if (sel) wattroff(data, A_REVERSE);
                line++;
            }
            (void)ts;
        } else if (current_mode == 1) { // Sensors
            pthread_mutex_lock(&state_lock);
            char local_sensors[16384]; snprintf(local_sensors, sizeof(local_sensors), "%s", sensors_buf);
            time_t ts = sensors_ts;
            pthread_mutex_unlock(&state_lock);
            sensors_count = 0; // reset before parsing
            parse_sensors_json(local_sensors);
            // Determine sensor_source from status so we can filter the list (auto shows both)
            pthread_mutex_lock(&state_lock);
            char local_status[4096]; snprintf(local_status, sizeof(local_status), "%s", status_buf);
            pthread_mutex_unlock(&state_lock);
            char cur_src[32] = "";
            char *ps = strstr(local_status, "\"sensor_source\"");
            if (ps) {
                ps = strchr(ps, ':'); if (ps) { ps++; while (*ps && isspace((unsigned char)*ps)) ps++; if (*ps == '"') { ps++; char *q = strchr(ps, '"'); if (q) { int len = (int)(q - ps); if (len >= (int)sizeof(cur_src)) len = (int)sizeof(cur_src)-1; snprintf(cur_src, sizeof(cur_src), "%.*s", len, ps); } } else { char *q = ps; int i = 0; while (*q && !isspace((unsigned char)*q) && *q != ',' && i < (int)sizeof(cur_src)-1) cur_src[i++] = *q++; cur_src[i] = '\0'; } }
            }
            int filter = 0; // 0=all,1=hwmon only,2=thermal only
            if (strcmp(cur_src, "hwmon") == 0) filter = 1;
            else if (strcmp(cur_src, "thermal") == 0) filter = 2;
            // Build filtered display index
            sensors_display_count = 0;
            for (int i = 0; i < sensors_count && sensors_display_count < (int)(sizeof(sensors_display_idx)/sizeof(sensors_display_idx[0])); ++i) {
                if (filter == 0 || (filter == 1 && sensors_arr[i].kind == 0) || (filter == 2 && sensors_arr[i].kind == 1)) {
                    sensors_display_idx[sensors_display_count++] = i;
                }
            }
            // Debugging info: show counts and first few entries in footer
            {
                char dbg[256] = ""; char tmp[128]; snprintf(dbg, sizeof(dbg), "Sensors total=%d filtered=%d src=%s", sensors_count, sensors_display_count, cur_src[0] ? cur_src : "auto");
                for (int k = 0; k < sensors_display_count && k < 4; ++k) { int idx = sensors_display_idx[k]; snprintf(tmp, sizeof(tmp), " %s", sensors_arr[idx].name); strncat(dbg, tmp, sizeof(dbg)-strlen(dbg)-1); }
                set_last_msg("%s", dbg);
            }
            // Write full filtered list to /tmp/tui-sensors.txt for inspection
            {
                FILE *f = fopen("/tmp/tui-sensors.txt", "w");
                if (f) {
                    for (int i = 0; i < sensors_display_count; ++i) {
                        int idx = sensors_display_idx[i];
                        fprintf(f, "%d: kind=%d name=%s path=%s temp=%d excluded=%d\n", i, sensors_arr[idx].kind, sensors_arr[idx].name, sensors_arr[idx].path, sensors_arr[idx].temp, sensors_arr[idx].excluded);
                    }
                    fclose(f);
                }
            }
            // Display parsed sensors array with interactive selection (filtered)
            if (sensors_display_count == 0) {
                mvwprintw(data,1,2,"(no sensors)");
            } else {
                if (sensors_sel < 0) sensors_sel = 0;
                if (sensors_sel >= sensors_display_count) sensors_sel = sensors_display_count - 1;
                if (sensors_sel < sensors_offset) sensors_offset = sensors_sel;
                if (sensors_sel >= sensors_offset + page_lines) sensors_offset = sensors_sel - page_lines + 1;
                for (int i = sensors_offset; i < sensors_display_count && i < sensors_offset + page_lines; ++i) {
                    int idx = sensors_display_idx[i];
                    int row = i - sensors_offset + 1;
                    if (i == sensors_sel) wattron(data, A_REVERSE);
                    char linebuf[512];
                    if (sensors_arr[idx].kind == 0) {
                        snprintf(linebuf, sizeof(linebuf), "HWMon: %s : %d°C", sensors_arr[idx].name, sensors_arr[idx].temp);
                    } else {
                        snprintf(linebuf, sizeof(linebuf), "Zone %d (%s): %s %d°C", sensors_arr[idx].zone, sensors_arr[idx].type, sensors_arr[idx].excluded ? "Excluded" : "Included", sensors_arr[idx].temp);
                    }
                    mvwprintw_scrollable(data, row, 2, getmaxx(data)-4, linebuf, data_horiz_offset, (i == sensors_sel));
                    if (i == sensors_sel) wattroff(data, A_REVERSE);
                }
            }
            (void)ts;
        } else if (current_mode == 3) { // Skins
            pthread_mutex_lock(&state_lock);
            char local_skins[4096]; snprintf(local_skins, sizeof(local_skins), "%s", skins_buf);
            time_t ts = skins_ts;
            pthread_mutex_unlock(&state_lock);
            // parse skins list
            skins_count = 0;
            char *saveptr = NULL; char *p = strtok_r(local_skins, "\n", &saveptr);
            while (p && skins_count < 256) {
                snprintf(skins_list[skins_count], sizeof(skins_list[0]), "%.255s", p);
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
                mvwprintw_scrollable(data, row, 2, getmaxx(data)-4, skins_list[i], data_horiz_offset, (i == skins_sel));
                if (i==skins_sel) wattroff(data, A_REVERSE);
            }
            mvwprintw(data, page_lines+1, 2, "Skins: %d", skins_count);
            (void)ts;
        } else if (current_mode == 2) { // Profiles
            prof_count = read_profiles((char (*)[256])profs, 256);
            // apply filter into display_profs
            display_count = 0;
            for (int i = 0; i < prof_count; ++i) {
                if (profile_filter[0] == '\0' || strstr(profs[i], profile_filter)) {
                    snprintf(display_profs[display_count], sizeof(display_profs[0]), "%.255s", profs[i]); display_count++;
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
                mvwprintw_scrollable(data, row, 2, getmaxx(data)-4, display_profs[i], data_horiz_offset, (i == sel));
                if (i==sel) wattroff(data, A_REVERSE);
            }
            mvwprintw(data, page_lines+1, 2, "Profiles: %d (filter: %s)", display_count, profile_filter[0] ? profile_filter : "-");
        }
        wrefresh(data);

        if (help_visible) {
            // paginated, wrapped help content: core + mode-specific
            const char *core_lines[] = {
                "h/H/? : toggle help",
                "Tab : switch mode (Limits/Sensors/Skins/Profiles)",
                "f : Show full-line popup (press any key to dismiss)",
                "r : Manual refresh (fetch data from daemon)",
                "R : Toggle raw status JSON (popup)",
                "S : Start/Restart daemon (systemctl restart, may require sudo)",
                "D : Stop daemon (send quit to daemon)",
                "q : quit",
                "Space/PageDown : next page | PageUp/p : previous page",
            };
            const char *limits_lines[] = {
                "Limits mode commands:",
                "s : set safe_max",
                "m : set safe_min",
                "t : set temp_max",
            };
            const char *zones_lines[] = {
                "Sensors mode commands:",
                "UP/DN : select sensor",
                "x : toggle exclude for selected sensor",
                "o : cycle sensor source (auto->hwmon->thermal)",
                "v : toggle use average temp",
                "Left/Right: horiz scroll selected line | f: view full line",
            };
            const char *profiles_lines[] = {
                "Profiles mode commands:",
                "l : load selected profile",
                "c : create profile",
                "e : edit selected profile",
                "d : delete selected profile",
                "/ : filter profiles | g : clear filter",
            };
            const char *skins_lines[] = {
                "Skins mode commands:",
                "i : install skin archive",
                "x : delete selected skin",
                "d : reset to default skin",
                "a : activate skin | u : deactivate skin",
            };
            const char **mode_lines = NULL; int mode_count = 0;
            if (current_mode == 0) { mode_lines = limits_lines; mode_count = sizeof(limits_lines)/sizeof(limits_lines[0]); }
            else if (current_mode == 1) { mode_lines = zones_lines; mode_count = sizeof(zones_lines)/sizeof(zones_lines[0]); }
            else if (current_mode == 2) { mode_lines = profiles_lines; mode_count = sizeof(profiles_lines)/sizeof(profiles_lines[0]); }
            else if (current_mode == 3) { mode_lines = skins_lines; mode_count = sizeof(skins_lines)/sizeof(skins_lines[0]); }

            werase(helpwin);
            box(helpwin,0,0); mvwprintw(helpwin,0,2," Help (%s) ", mode_names[current_mode]);
            int hh = getmaxy(helpwin); int hw = getmaxx(helpwin) - 4; if (hw < 10) hw = 10;
            int page_lines = hh - 3;
            int total_wrapped = 0;
            for (int i = 0; i < (int)(sizeof(core_lines)/sizeof(core_lines[0])); ++i) total_wrapped += wrapped_count(core_lines[i], hw);
            for (int i = 0; i < mode_count; ++i) total_wrapped += wrapped_count(mode_lines[i], hw);
            if (help_offset < 0) help_offset = 0;
            if (help_offset >= total_wrapped) help_offset = (total_wrapped - 1) / page_lines * page_lines;
            int cur_idx = 0; int row = 1;
            for (int i = 0; i < (int)(sizeof(core_lines)/sizeof(core_lines[0])) && row <= page_lines; ++i) {
                int nwr = wrapped_count(core_lines[i], hw);
                for (int j = 0; j < nwr && row <= page_lines; ++j) {
                    if (cur_idx >= help_offset) { char out[512]; get_wrapped_line(core_lines[i], hw, j, out, sizeof(out)); mvwprintw(helpwin, row, 2, "%s", out); row++; }
                    cur_idx++;
                }
            }
            for (int i = 0; i < mode_count && row <= page_lines; ++i) {
                int nwr = wrapped_count(mode_lines[i], hw);
                for (int j = 0; j < nwr && row <= page_lines; ++j) {
                    if (cur_idx >= help_offset) { char out[512]; get_wrapped_line(mode_lines[i], hw, j, out, sizeof(out)); mvwprintw(helpwin, row, 2, "%s", out); row++; }
                    cur_idx++;
                }
            }
            if (help_offset + page_lines < total_wrapped) mvwprintw(helpwin, hh-1, 2, "-- more: Space/PageDown --");
            else mvwprintw(helpwin, hh-1, 2, "-- end --");
            wrefresh(helpwin);
        }
        // footer with last message: render in footer window so it's independent of other windows
        werase(footerwin);
        {
            char msg[256]; snprintf(msg, sizeof(msg), "Msg: %.200s", local_msg);
            mvwprintw(footerwin, 0, 1, "%.*s", width - 2, msg);
        }
            if (current_mode == 2) { // Profiles
            char ks[512]; snprintf(ks, sizeof(ks), "  Keys: h help | Tab mode | l load | c create | e edit | d delete | / filter | g clear | q quit | R raw status | S start | Left/Right | f");
            const char* ks_short = "  Keys: h help | Tab mode | l load | q quit | f";
            if ((int)strlen(ks) <= width) mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks);
            else mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks_short);
        } else if (current_mode == 0) { // Limits
            char ks[512]; snprintf(ks, sizeof(ks), "  Keys: h help | Tab mode | s/m/t set limits | D stop | S start | q quit | R raw status | Left/Right | f");
            const char* ks_short = "  Keys: h help | Tab mode | s/t set limits | q quit | f";
            if ((int)strlen(ks) <= width) mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks);
            else mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks_short);
        } else if (current_mode == 3) { // Skins
            char ks[512]; snprintf(ks, sizeof(ks), "  Keys: h help | Tab mode | i install | x delete | d default | a activate | u deactivate | q quit | R raw status | S start | Left/Right | f");
            const char* ks_short = "  Keys: h help | Tab mode | i install | x delete | q quit | f";
            if ((int)strlen(ks) <= width) mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks);
            else mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks_short);
        } else if (current_mode == 1) { // Sensors
            char ks[512]; snprintf(ks, sizeof(ks), "  Keys: h help | Tab mode | UP/DN nav | x toggle exclude | o cycle source | v toggle avg temp | q quit | R raw status | S start | Left/Right | f");
            const char* ks_short = "  Keys: h help | Tab mode | r refresh | UP/DN | q quit | f";
            if ((int)strlen(ks) <= width) mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks);
            else mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks_short);
        } else {
            char ks[512]; snprintf(ks, sizeof(ks), "  Keys: h help | Tab mode | r refresh | D stop | S start | q quit | R raw status | Left/Right | f");
            const char* ks_short = "  Keys: h help | Tab mode | q quit | f";
            if ((int)strlen(ks) <= width) mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks);
            else mvwprintw(footerwin, 1, 1, "%.*s", width - 2, ks_short);
        }
        // draw a faint horizontal line above the footer to separate it from the main UI
        mvhline(height-3, 0, ACS_HLINE, width);
        refresh();
        wrefresh(footerwin);

        int ch = getch();
        if (ch == 'q') { break; }
        else if (ch == '\t') { // Tab to switch mode
            current_mode = (current_mode + 1) % mode_count;
            sel = 0; offset = 0; data_horiz_offset = 0; // reset selection and horizontal scroll
            // force redraw of the entire screen so the new mode clears remnants
            touchwin(stdscr);
            refresh();
        }
        else if (ch == 'r') { /* just loop to refresh */ }
        else if (ch == 'h' || ch == 'H' || ch == '?') {
            help_visible = !help_visible;
            if (!help_visible && helpwin) {
                werase(helpwin);
                wrefresh(helpwin);
                delwin(helpwin);
                helpwin = NULL;
                // ensure underlying screen is repainted to remove artefacts left by the help window
                touchwin(stdscr);
                refresh();
            }
        }
        else if (help_visible && (ch == ' ' || ch == KEY_NPAGE || ch == 'n')) {
            // next page
            help_offset += (getmaxy(helpwin) - 3);
            continue;
        } else if (help_visible && (ch == KEY_PPAGE || ch == 'p')) {
            // previous page
            help_offset -= (getmaxy(helpwin) - 3);
            if (help_offset < 0) help_offset = 0;
            continue;
        }
        else if (current_mode == 2 && ch == KEY_UP) { if (sel>0) sel--; data_horiz_offset = 0; }
        else if (current_mode == 2 && ch == KEY_DOWN) { if (sel<display_count-1) sel++; data_horiz_offset = 0; }
        else if (current_mode == 3 && ch == KEY_UP) { if (skins_sel>0) skins_sel--; data_horiz_offset = 0; }
        else if (current_mode == 3 && ch == KEY_DOWN) { if (skins_sel<skins_count-1) skins_sel++; data_horiz_offset = 0; }
        else if (current_mode == 1 && ch == KEY_UP) { if (sensors_sel>0) sensors_sel--; data_horiz_offset = 0; }
        else if (current_mode == 1 && ch == KEY_DOWN) { if (sensors_sel<sensors_count-1) sensors_sel++; data_horiz_offset = 0; }
        else if (current_mode == 0 && ch == KEY_UP) { if (limits_sel>0) limits_sel--; data_horiz_offset = 0; }
        else if (current_mode == 0 && ch == KEY_DOWN) { if (limits_sel < limits_count - 1) limits_sel++; data_horiz_offset = 0; }
        else if (ch == KEY_LEFT) { if (data_horiz_offset > 0) data_horiz_offset -= 4; if (data_horiz_offset < 0) data_horiz_offset = 0; }
        else if (ch == KEY_RIGHT) { data_horiz_offset += 4; }
        else if (ch == 'f') {
            // show full content for selected line in data pane as popup
            if (current_mode == 1 && sensors_count > 0) {
                char buf[1024]; if (sensors_arr[sensors_sel].kind == 0) snprintf(buf, sizeof(buf), "HWMon: %s\nPath: %s\nTemp: %d°C\nExcluded: %s", sensors_arr[sensors_sel].name, sensors_arr[sensors_sel].path, sensors_arr[sensors_sel].temp, sensors_arr[sensors_sel].excluded ? "yes" : "no"); else snprintf(buf, sizeof(buf), "Zone %d (%s)\nPath: %s\nTemp: %d°C\nExcluded: %s", sensors_arr[sensors_sel].zone, sensors_arr[sensors_sel].type, sensors_arr[sensors_sel].path, sensors_arr[sensors_sel].temp, sensors_arr[sensors_sel].excluded ? "yes" : "no");
                int pw = getmaxx(stdscr)-4; int len = (int)strlen(buf) + 4; if (pw > len) pw = len; int ph = 8;
                WINDOW *pwin = newwin(ph, pw, (getmaxy(stdscr)-ph)/2, (getmaxx(stdscr)-pw)/2);
                box(pwin, 0,0); mvwprintw(pwin,1,2, "%s", buf); mvwprintw(pwin,ph-2,2, "press any key"); wrefresh(pwin); wgetch(pwin); delwin(pwin);
            } else if (current_mode == 2 && display_count > 0) {
                char *s = display_profs[sel]; int pw = getmaxx(stdscr)-4; int len = (int)strlen(s) + 4; if (pw > len) pw = len; int ph = 6;
                WINDOW *pwin = newwin(ph, pw, (getmaxy(stdscr)-ph)/2, (getmaxx(stdscr)-pw)/2);
                box(pwin,0,0); mvwprintw(pwin,1,2, "%s", s); mvwprintw(pwin,ph-2,2, "press any key"); wrefresh(pwin); wgetch(pwin); delwin(pwin);
            } else if (current_mode == 3 && skins_count > 0) {
                char *s = skins_list[skins_sel]; int pw = getmaxx(stdscr)-4; int len = (int)strlen(s) + 4; if (pw > len) pw = len; int ph = 6;
                WINDOW *pwin = newwin(ph, pw, (getmaxy(stdscr)-ph)/2, (getmaxx(stdscr)-pw)/2);
                box(pwin,0,0); mvwprintw(pwin,1,2, "%s", s); mvwprintw(pwin,ph-2,2, "press any key"); wrefresh(pwin); wgetch(pwin); delwin(pwin);
            } else if (current_mode == 0) {
                pthread_mutex_lock(&state_lock);
                char local_limits[4096]; snprintf(local_limits, sizeof(local_limits), "%s", limits_buf);
                pthread_mutex_unlock(&state_lock);
                char *pretty = format_limits_zones(local_limits, 1);
                // If limits selection exists show selected line, else show full pretty output
                if (limits_count > 0 && limits_sel >= 0 && limits_sel < limits_count) {
                    char bufsel[512] = "";
                    char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", pretty); char *sp = NULL; char *pp = strtok_r(tmp, "\n", &sp);
                    int idx = 0;
                    while (pp) {
                        if (idx == limits_sel) { snprintf(bufsel, sizeof(bufsel), "%s", pp); break; }
                        idx++; pp = strtok_r(NULL, "\n", &sp);
                    }
                    if (bufsel[0] == '\0') snprintf(bufsel, sizeof(bufsel), "%s", "(not available)");
                    int pw = getmaxx(stdscr)-4; int len = (int)strlen(bufsel) + 4; if (len < 20) len = 20; if (pw > len) pw = len; int ph = 6;
                    WINDOW *pwin = newwin(ph, pw, (getmaxy(stdscr)-ph)/2, (getmaxx(stdscr)-pw)/2);
                    box(pwin,0,0); mvwprintw(pwin,1,2, "%s", bufsel); mvwprintw(pwin,ph-2,2, "press any key"); wrefresh(pwin); wgetch(pwin); delwin(pwin);
                } else {
                    int pw = getmaxx(stdscr)-4; if (pw > 80) pw = 80; int tmpcalc = (int)strlen(pretty) / ((pw - 4) > 0 ? (pw - 4) : 1) + 4; if (tmpcalc > 30) tmpcalc = 30; int ph = tmpcalc;
                    WINDOW *pwin = newwin(ph, pw, (getmaxy(stdscr)-ph)/2, (getmaxx(stdscr)-pw)/2);
                    box(pwin,0,0);
                    int r = 1; char tmp2[4096]; snprintf(tmp2, sizeof(tmp2), "%s", pretty); char *sp = NULL; char *pp = strtok_r(tmp2, "\n", &sp);
                    while (pp && r < ph-1) { mvwprintw(pwin, r, 2, "%.*s", pw-4, pp); pp = strtok_r(NULL, "\n", &sp); r++; }
                    mvwprintw(pwin, ph-2, 2, "press any key"); wrefresh(pwin); wgetch(pwin); delwin(pwin);
                }
            }
        }
        else if (current_mode == 1 && ch == KEY_NPAGE) { int page = getmaxy(data) - 3; if (page < 1) page = 1; sensors_sel += page; if (sensors_sel >= sensors_display_count) sensors_sel = sensors_display_count - 1; }
        else if (current_mode == 1 && ch == KEY_PPAGE) { int page = getmaxy(data) - 3; if (page < 1) page = 1; sensors_sel -= page; if (sensors_sel < 0) sensors_sel = 0; }
        else if (current_mode == 1 && (ch == 'x' || ch == 'X')) {
            if (sensors_display_count == 0) { set_last_msg("No sensors to toggle"); }
            else {
                int vis_idx = sensors_sel;
                if (vis_idx < 0) {
                    vis_idx = 0;
                }
                if (vis_idx >= sensors_display_count) {
                    vis_idx = sensors_display_count - 1;
                }
                int idx = sensors_display_idx[vis_idx];
                // Determine token to toggle: zone.type for zones, or sensor name for hwmon
                char token[256]; if (sensors_arr[idx].kind == 1) snprintf(token, sizeof(token), "%s", sensors_arr[idx].type); else snprintf(token, sizeof(token), "%s", sensors_arr[idx].name);
                // trim and lowercase
                char *s = token; while (*s && isspace((unsigned char)*s)) s++; char *e = s + strlen(s) - 1; while (e > s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
                for (char *p = s; *p; ++p) *p = tolower((unsigned char)*p);
                if (!s || !*s) { set_last_msg("Empty token" ); continue; }
                // Fetch existing excluded-types CSV from daemon so we don't clobber tokens unknown to this device
                char existing_csv[4096] = "";
                char *resp = send_unix_command("get-excluded-types");
                if (resp) {
                    snprintf(existing_csv, sizeof(existing_csv), "%s", resp);
                    free(resp);
                }
                // Remember previous excluded state
                int was_excluded = sensors_arr[idx].excluded;
                // Parse CSV into list and dedupe
                char tokens[128][256]; int token_count = 0;
                if (existing_csv[0]) {
                    char tmp[2048]; snprintf(tmp, sizeof(tmp), "%s", existing_csv);
                    char *tok = strtok(tmp, ",");
                    while (tok) {
                        // trim and lowercase
                        char tk[256]; snprintf(tk, sizeof(tk), "%s", tok);
                        char *tk_s = tk; while (*tk_s && isspace((unsigned char)*tk_s)) tk_s++; char *tk_e = tk_s + strlen(tk_s) - 1; while (tk_e > tk_s && isspace((unsigned char)*tk_e)) { *tk_e = '\0'; tk_e--; }
                        for (char *p = tk_s; *p; ++p) *p = tolower((unsigned char)*p);
                        if (tk_s && *tk_s) {
                            int found = 0; for (int i = 0; i < token_count; ++i) { if (strcmp(tokens[i], tk_s) == 0) { found = 1; break; } }
                            if (!found && token_count < (int)(sizeof(tokens)/sizeof(tokens[0]))) { snprintf(tokens[token_count], sizeof(tokens[0]), "%s", tk_s); token_count++; }
                        }
                        tok = strtok(NULL, ",");
                    }
                }
                // check if token is present; toggle: remove if present, add if not
                int present = 0; for (int i = 0; i < token_count; ++i) { if (strcmp(tokens[i], s) == 0) { present = 1; /* remove */ for (int k = i; k + 1 < token_count; ++k) snprintf(tokens[k], sizeof(tokens[0]), "%s", tokens[k+1]); token_count--; break; } }
                if (!present) {
                    if (token_count < (int)(sizeof(tokens)/sizeof(tokens[0]))) { snprintf(tokens[token_count], sizeof(tokens[0]), "%s", s); token_count++; }
                }
                // If we were excluded but didn't find an exact token to remove, attempt substring matches
                if (was_excluded && !present) {
                    int removed_any = 0;
                    for (int i = 0; i < token_count; ++i) {
                        if (strstr(s, tokens[i])) {
                            // remove tokens[i]
                            for (int k = i; k + 1 < token_count; ++k) snprintf(tokens[k], sizeof(tokens[0]), "%s", tokens[k+1]);
                            token_count--; i--; removed_any = 1;
                        }
                    }
                    if (removed_any) present = 1;
                }
                // rebuild csv
                char csv[4096] = "";
                for (int i = 0; i < token_count; ++i) {
                    if (i) strncat(csv, ",", sizeof(csv)-strlen(csv)-1);
                    strncat(csv, tokens[i], sizeof(csv)-strlen(csv)-1);
                }
                // apply optimistic UI change to sensor
                sensors_arr[idx].excluded = present ? 0 : 1;
                // send to daemon
                char cmd[8192];
                if (csv[0] == '\0') snprintf(cmd, sizeof(cmd), "set-excluded-types none");
                else snprintf(cmd, sizeof(cmd), "set-excluded-types %s", csv);
                spawn_cmd_async(cmd);
                // refresh sensors and status so UI shows persisted change and saved value
                spawn_fetch_sensors_async();
                spawn_fetch_status_async();
                set_last_msg("Toggled excluded for %s", s);
            }
        }
        else if (current_mode == 1 && (ch == 'o' || ch == 'O')) {
            // cycle sensor source: auto -> hwmon -> thermal -> auto
            // get current sensor_source from status
            pthread_mutex_lock(&state_lock);
            char local_status[4096]; snprintf(local_status, sizeof(local_status), "%s", status_buf);
            pthread_mutex_unlock(&state_lock);
            char *p = strstr(local_status, "\"sensor_source\"");
            char cur_src[32] = "";
            if (p) { p = strchr(p, ':'); if (p) { p++; while (*p && isspace((unsigned char)*p)) p++; if (*p == '"') { p++; char *q = strchr(p, '"'); if (q) { int len = (int)(q - p); if (len >= (int)sizeof(cur_src)) len = (int)sizeof(cur_src)-1; snprintf(cur_src, sizeof(cur_src), "%.*s", len, p); } } else { // bare token
                        char *q = p; int i = 0; while (*q && !isspace((unsigned char)*q) && *q != ',' && i < (int)sizeof(cur_src)-1) cur_src[i++] = *q++; cur_src[i] = '\0'; } } }
            const char *next = "hwmon";
            if (strcmp(cur_src, "hwmon") == 0) next = "thermal";
            else if (strcmp(cur_src, "thermal") == 0) next = "auto";
            else next = "hwmon"; // default from auto/unknown
            char cmd[128]; snprintf(cmd, sizeof(cmd), "set-sensor-source %s", next);
            spawn_cmd_async(cmd);
            spawn_fetch_sensors_async(); spawn_fetch_status_async();
            set_last_msg("Set sensor source to %s", next);
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
                    snprintf(status_buf, sizeof(status_buf), "%s", r);
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
        else if (current_mode == 2 && ch == 'l') {
            if (display_count>0) {
                {
                    char lp[256]; snprintf(lp, sizeof(lp), "Loading profile %s...", display_profs[sel]);
                    wrefresh(footerwin);
                }
                spawn_load_profile_async(display_profs[sel]);
                set_last_msg("Loading...");
            }
        }
        else if (current_mode == 2 && ch == 'c') {
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
        else if (current_mode == 2 && ch == 'e') {
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
                            if (strcmp(key, "safe_min") == 0) snprintf(cur_smin, sizeof(cur_smin), "%s", val);
                            else if (strcmp(key, "safe_max") == 0) snprintf(cur_smax, sizeof(cur_smax), "%s", val);
                            else if (strcmp(key, "temp_max") == 0) snprintf(cur_tmax, sizeof(cur_tmax), "%s", val);
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
        else if (current_mode == 2 && ch == '/') {
            // filter profiles
            char f[128] = {0};
            if (prompt_input(inputwin, "Filter profiles (substring, empty to clear): ", f, sizeof(f)) >= 0) {
                snprintf(profile_filter, sizeof(profile_filter), "%s", f);
                sel = 0; offset = 0;
                set_last_msg("Filter applied");
            }
        }
        else if (current_mode == 2 && ch == 'g') {
            profile_filter[0] = '\0'; sel = 0; offset = 0; set_last_msg("Filter cleared");
        }
        else if (current_mode == 2 && ch == 'd') {
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
        else if (current_mode == 3 && ch == 'i') {
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
        else if (current_mode == 3 && ch == 'd') {
            spawn_default_skin_async();
            set_last_msg("Resetting to default skin...");
            refresh();
        }
        else if (current_mode == 3 && ch == 'a') {
            if (skins_count > 0) {
                char cmd[512]; snprintf(cmd, sizeof(cmd), "activate-skin %s", skins_list[skins_sel]);
                spawn_cmd_async(cmd);
                set_last_msg("Activating skin %s...", skins_list[skins_sel]);
            } else {
                set_last_msg("No skins to activate");
            }
            refresh();
        }
        else if (current_mode == 3 && ch == 'u') {
            if (skins_count > 0) {
                char cmd[512]; snprintf(cmd, sizeof(cmd), "deactivate-skin %s", skins_list[skins_sel]);
                spawn_cmd_async(cmd);
                set_last_msg("Deactivating skin %s...", skins_list[skins_sel]);
            } else {
                set_last_msg("No skins to deactivate");
            }
            refresh();
        }
        else if (current_mode == 3 && ch == 'x') {
            // delete selected skin
            if (skins_count > 0) {
                help_visible = 0;
                int a = 0;
                size_t need = (size_t)snprintf(NULL, 0, "Delete skin %s? (y/N): ", skins_list[skins_sel]) + 1;
                char *qprompt = malloc(need);
                if (qprompt) {
                    snprintf(qprompt, need, "Delete skin %s? (y/N): ", skins_list[skins_sel]);
                    a = prompt_char(inputwin, qprompt);
                    free(qprompt);
                } else {
                    char qbuf[128]; snprintf(qbuf, sizeof(qbuf), "Delete skin %.100s? (y/N): ", skins_list[skins_sel]);
                    a = prompt_char(inputwin, qbuf);
                }
                if (a == 'y' || a == 'Y') {
                    char cmd[512]; snprintf(cmd, sizeof(cmd), "remove-skin %s", skins_list[skins_sel]);
                    spawn_cmd_async(cmd);
                    set_last_msg("Removing skin...");
                } else {
                    set_last_msg("Cancelled");
                }
                refresh();
            }
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

        else if (ch == 'R') {
            // toggle raw status popup (handled after status refresh)
            show_raw_status = 1;
            continue;
        }

    }

    // stop poller and cleanup windows
    keep_running = 0;
    pthread_join(poller, NULL);
    if (status) delwin(status);
    if (data) delwin(data);
    if (helpwin) delwin(helpwin);
    if (inputwin) delwin(inputwin);
    if (footerwin) delwin(footerwin);
    // ensure the terminal is cleaned up and cursor restored
    curs_set(1);
    clear(); refresh();
    endwin();
    // newline to avoid leftovers on the prompt line
    printf("\n"); fflush(stdout);
    return 0;
}
