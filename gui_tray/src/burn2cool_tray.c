/* Renamed from tray.c -> burn2cool_tray.c to reflect project naming.
 * Content copied from previous tray.c; build system updated to compile
 * this source file.
 */
#include "appindicator_include.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <libnotify/notify.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/socket.h>
#include <sys/un.h>
#if __has_include("../include/favicon_ico.h")
#include "../include/favicon_ico.h"
#define HAVE_EMBEDDED_FAVICON 1
#endif

#define DEFAULT_PORT 8086
#define DEFAULT_HOST "http://localhost"
/* update when releasing new versions */
#define APP_VERSION "v0.3.0"

typedef struct {
    char *data;
    size_t size;
} MemoryChunk;

static int http_port = DEFAULT_PORT;
static AppIndicator *indicator = NULL;
static GtkWidget *profiles_menu = NULL;
static GtkWidget *status_item = NULL;
static struct json_object *locale_root = NULL;
static char current_lang[8] = "en";
static GtkWidget *profiles_label_widget = NULL;
static GtkWidget *open_web_widget = NULL;
static GtkWidget *quit_widget = NULL;
static GtkWidget *language_menu = NULL;
static GtkWidget *language_label_widget = NULL;
static GtkWidget *about_widget = NULL;
static char config_path_override[PATH_MAX] = ""; // if set, use this file as config

// forward declarations
static void on_language_activate(GtkMenuItem *item, gpointer user_data);
static void populate_profiles_menu(void);
/* Overview window (implemented in overview.c) */
extern void overview_set_port(int port);
extern void overview_show(GtkWindow *parent);

static void on_show_overview(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    overview_set_port(http_port);
    overview_show(NULL);
}

// Read a local file into a malloc'd buffer (caller must free)
static char *read_file_to_string(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// Get localized string by dotted key path (e.g. "menu.refresh"). Returns fallback if not found.
static const char *get_loc(const char *path, const char *fallback) {
    if (!locale_root || !path) return fallback;
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    char *saveptr = NULL;
    char *tok = strtok_r(tmp, ".", &saveptr);
    struct json_object *cur = locale_root;
    while (tok) {
        struct json_object *next = NULL;
        if (!json_object_object_get_ex(cur, tok, &next)) return fallback;
        cur = next;
        tok = strtok_r(NULL, ".", &saveptr);
    }
    if (json_object_is_type(cur, json_type_string)) return json_object_get_string(cur);
    return fallback;
}

// Try to load a locale JSON file from i18n/<lang>.json (relative to current working dir).
static struct json_object *load_locale(const char *lang) {
    char path[PATH_MAX];
    FILE *f;
    struct json_object *root = NULL;

    snprintf(path, sizeof(path), "/usr/local/share/burn2cool_tray/i18n/%s.json", lang);
    char *content = read_file_to_string(path);
    if (!content) {
        // fallback to en
        if (strcmp(lang, "en") != 0) {
            snprintf(path, sizeof(path), "/usr/local/share/burn2cool_tray/i18n/en.json");
            content = read_file_to_string(path);
        }
    }
    if (!content) return;
    struct json_object *j = json_tokener_parse(content);
    free(content);
    if (!j) return;
    if (locale_root) json_object_put(locale_root);
    locale_root = j;
}

// Determine preferred language from CPU_THROTTLE_LANG or LANG env
static void load_locale_from_env(void) {
    const char *env = getenv("CPU_THROTTLE_LANG");
    char code[8];
    if (!env) env = getenv("LANG");
    if (!env) {
        strncpy(code, "en", sizeof(code));
    } else {
        // LANG may be like en_US.UTF-8; extract leading alpha part
        size_t i = 0;
        while (env[i] && env[i] != '_' && env[i] != '.' && i < sizeof(code)-1) {
            code[i] = env[i];
            i++;
        }
        code[i] = '\0';
        if (i == 0) strncpy(code, "en", sizeof(code));
    }
    load_locale(code);
    strncpy(current_lang, code, sizeof(current_lang)-1);
    current_lang[sizeof(current_lang)-1] = '\0';
}

// user config file helpers --------------------------------------------------
// config path: ~/.config/cpu_throttle_gui/.config
static const char *user_config_rel_dir = ".config/cpu_throttle_gui";
static const char *user_config_file = ".config"; // placed inside that dir

static int ensure_user_config_dir(char *out_path, size_t out_sz) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", home, user_config_rel_dir);
    // create if missing
    struct stat st = {0};
    if (stat(dirpath, &st) != 0) {
        if (mkdir(dirpath, 0700) != 0 && errno != EEXIST) return -1;
    }
    if (out_path) {
        size_t need = strlen(dirpath) + 1 + strlen(user_config_file) + 1;
        if (need > out_sz) return -1;
        snprintf(out_path, out_sz, "%s/%s", dirpath, user_config_file);
    }
    return 0;
}

static char *read_user_config_lang(void) {
    char path[PATH_MAX];
    if (ensure_user_config_dir(path, sizeof(path)) != 0) return NULL;
    // path now like /home/user/.config/cpu_throttle_gui/.config
    char *content = read_file_to_string(path);
    if (!content) return NULL;
    struct json_object *j = json_tokener_parse(content);
    free(content);
    if (!j) return NULL;
    struct json_object *jlang = NULL;
    if (json_object_object_get_ex(j, "lang", &jlang) && json_object_is_type(jlang, json_type_string)) {
        const char *s = json_object_get_string(jlang);
        char *ret = strdup(s);
        json_object_put(j);
        return ret;
    }
    json_object_put(j);
    return NULL;
}

static int write_user_config_lang(const char *lang) {
    if (!lang) return -1;
    char path[PATH_MAX];
    if (ensure_user_config_dir(path, sizeof(path)) != 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\"lang\": \"%s\"}\n", lang);
    fclose(f);
    return 0;
}

// Populate language menu by scanning i18n/*.json
static void populate_language_menu(void) {
    if (!language_menu) return;
    // clear existing
    GList *children = gtk_container_get_children(GTK_CONTAINER(language_menu));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    DIR *d = opendir("/usr/local/share/burn2cool_tray/i18n");
    if (!d) {
        GtkWidget *item = gtk_menu_item_new_with_label("(no locales)");
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(language_menu), item);
        gtk_widget_show_all(language_menu);
        return;
    }
    struct dirent *ent;
    GSList *group = NULL;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len > 5 && strcmp(name + len - 5, ".json") == 0) {
            // extract code (file without extension)
            char code[64];
            snprintf(code, sizeof(code), "%.*s", (int)(len - 5), name);
            // try to read meta.name from file
            char path[256];
            snprintf(path, sizeof(path), "/usr/local/share/burn2cool_tray/i18n/%s", name);
            char *content = read_file_to_string(path);
            const char *display = code;
            char *label_copy = NULL;
            if (content) {
                struct json_object *j = json_tokener_parse(content);
                if (j && json_object_is_type(j, json_type_object)) {
                    struct json_object *meta = NULL, *mname = NULL;
                    if (json_object_object_get_ex(j, "meta", &meta) && json_object_object_get_ex(meta, "name", &mname) && json_object_is_type(mname, json_type_string)) {
                        display = json_object_get_string(mname);
                    }
                    json_object_put(j);
                }
                // copy the display string so it survives after freeing the parsed JSON
                label_copy = strdup(display);
                free(content);
            }
            // create radio menu item
            GtkWidget *mi = NULL;
            const char *label_for_item = label_copy ? label_copy : display;
            if (group == NULL) {
                mi = gtk_radio_menu_item_new_with_label(NULL, label_for_item);
                group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mi));
            } else {
                mi = gtk_radio_menu_item_new_with_label(group, label_for_item);
                group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mi));
            }
            // store code pointer via g_object_set_data_full
            g_object_set_data_full(G_OBJECT(mi), "locale_code", g_strdup(code), g_free);
            // set active if matches current_lang
            if (strcmp(code, current_lang) == 0) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
            // connect activate handler
            g_signal_connect(mi, "activate", G_CALLBACK(on_language_activate), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(language_menu), mi);
            if (label_copy) free(label_copy);
        }
    }
    closedir(d);
    gtk_widget_show_all(language_menu);
}

// Handler for language selection radio items
static void on_language_activate(GtkMenuItem *item, gpointer user_data) {
    (void)user_data;
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) return;
    const char *code = (const char*)g_object_get_data(G_OBJECT(item), "locale_code");
    if (!code) return;
    load_locale(code);
    strncpy(current_lang, code, sizeof(current_lang)-1);
    current_lang[sizeof(current_lang)-1] = '\0';
    // update static labels
    const char *init_lbl = get_loc("menu.status_initializing", "Status: Initializing...");
    if (status_item) gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), init_lbl);
    if (profiles_label_widget) gtk_menu_item_set_label(GTK_MENU_ITEM(profiles_label_widget), get_loc("menu.profiles_label", "Profiles"));
    if (open_web_widget) gtk_menu_item_set_label(GTK_MENU_ITEM(open_web_widget), get_loc("menu.open_web_ui", "Open Web UI"));
    if (quit_widget) gtk_menu_item_set_label(GTK_MENU_ITEM(quit_widget), get_loc("menu.quit", "Quit"));
    // repopulate dynamic menus
    populate_profiles_menu();
    populate_language_menu();
    // persist selection to per-user config
    if (config_path_override[0]) {
        // write JSON with single key to override file path
        FILE *f = fopen(config_path_override, "w");
        if (f) {
            fprintf(f, "{\"lang\": \"%s\"}\n", code);
            fclose(f);
        }
    } else {
        write_user_config_lang(code);
    }
    // show a brief notification that language changed
    const char *title = get_loc("messages.language_changed_title", "Language changed");
    const char *body_fmt = get_loc("messages.language_changed_body", "Language switched to %s");
    char body[256];
    // lookup display name from loaded locale meta (fall back to code)
    const char *display_name = NULL;
    if (locale_root) {
        struct json_object *meta = NULL, *mname = NULL;
        if (json_object_object_get_ex(locale_root, "meta", &meta) && json_object_object_get_ex(meta, "name", &mname) && json_object_is_type(mname, json_type_string)) {
            display_name = json_object_get_string(mname);
        }
    }
    if (!display_name) display_name = code;
    snprintf(body, sizeof(body), body_fmt, display_name);
    NotifyNotification *n = notify_notification_new(title, body, NULL);
    notify_notification_set_timeout(n, 1500);
    notify_notification_show(n, NULL);
    g_object_unref(n);
}

// CURL write callback
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    MemoryChunk *mem = (MemoryChunk*)userdata;
    char *ptr_new = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr_new) return 0; // out of memory
    mem->data = ptr_new;
    memcpy(&(mem->data[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static char *http_build_url(const char *path) {
    int needed = snprintf(NULL, 0, "%s:%d%s", DEFAULT_HOST, http_port, path);
    if (needed <= 0) return NULL;
    char *url = malloc((size_t)needed + 1);
    if (!url) return NULL;
    snprintf(url, (size_t)needed + 1, "%s:%d%s", DEFAULT_HOST, http_port, path);
    return url;
}

static char *socket_get(const char *cmd) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/cpu_throttle.sock", sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return NULL;
    }

    if (send(sock_fd, cmd, strlen(cmd), 0) < 0) {
        close(sock_fd);
        return NULL;
    }

    char *response = malloc(2048);
    if (!response) {
        close(sock_fd);
        return NULL;
    }
    ssize_t n = recv(sock_fd, response, 2047, 0);
    if (n > 0) {
        response[n] = '\0';
    } else {
        free(response);
        response = NULL;
    }

    close(sock_fd);
    return response;
}

static char *http_get(const char *path) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    MemoryChunk chunk = { .data = malloc(1), .size = 0 };
    char *url = http_build_url(path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    CURLcode res = curl_easy_perform(curl);
    free(url);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(chunk.data);
        // Fallback to socket
        if (strcmp(path, "/api/status") == 0) {
            return socket_get("status json");
        } else if (strcmp(path, "/api/profiles") == 0) {
            return socket_get("list-profiles json");
        }
        return NULL;
    }
    return chunk.data;
}

static int http_post_json(const char *path, const char *json, char **out_body) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    MemoryChunk chunk = { .data = malloc(1), .size = 0 };
    char *url = http_build_url(path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(chunk.data);
        return -1;
    }
    if (out_body) *out_body = chunk.data; else free(chunk.data);
    return 0;
}

// Parse JSON array of strings using json-c
static char **parse_json_string_array(const char *json_text, int *out_count) {
    if (!json_text) { *out_count = 0; return NULL; }
    struct json_tokener *tok = json_tokener_new();
    struct json_object *jobj = json_tokener_parse_ex(tok, json_text, strlen(json_text));
    enum json_tokener_error jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);
    if (jerr != json_tokener_success || !jobj) { *out_count = 0; return NULL; }
    if (!json_object_is_type(jobj, json_type_array)) { json_object_put(jobj); *out_count = 0; return NULL; }
    size_t len = json_object_array_length(jobj);
    char **list = calloc(len ? len : 1, sizeof(char*));
    for (size_t i = 0; i < len; ++i) {
        struct json_object *elem = json_object_array_get_idx(jobj, i);
        if (!elem) continue;
        const char *s = json_object_get_string(elem);
        if (!s) continue;
        list[i] = strdup(s);
    }
    *out_count = (int)len;
    json_object_put(jobj);
    return list;
}

static void free_string_list(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; ++i) free(list[i]);
    free(list);
}

static void on_open_webui(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    char url[256];
    snprintf(url, sizeof(url), "%s:%d/", DEFAULT_HOST, http_port);
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(url, NULL, &err)) {
        // Fallback to xdg-open if GLib launcher fails
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "xdg-open %s:%d/", DEFAULT_HOST, http_port);
        g_spawn_command_line_async(cmd, NULL);
    }
}

static void on_quit(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    gtk_main_quit();
}

static void on_show_version(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;

    if (!about_widget) {
        about_widget = gtk_about_dialog_new();
        gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about_widget), "cpu-throttle GUI");
        gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about_widget), APP_VERSION);
        const gchar *authors[] = {"DiabloPower", NULL};
        gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about_widget), authors);
        gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about_widget), "https://github.com/DiabloPower/burn2cool");
        gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about_widget), get_loc("about.title", "About this application"));

        /* Try to load a large about image from the local assets directory */
        const char *about_img = "/usr/local/share/burn2cool_tray/assets/about.png";
        if (g_file_test(about_img, G_FILE_TEST_EXISTS)) {
            GError *gerr = NULL;
            GdkPixbuf *pb = gdk_pixbuf_new_from_file(about_img, &gerr);
            if (pb) {
                gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(about_widget), pb);
                g_object_unref(pb);
            } else {
                if (gerr) g_error_free(gerr);
            }
        }

        /* destroy on response so we don't leak transient parent references */
        g_signal_connect(about_widget, "response", G_CALLBACK(gtk_widget_destroy), NULL);
        /* clear our stored pointer when the widget is actually destroyed so
         * subsequent opens can re-create the dialog without hitting
         * "GTK_IS_WIDGET (widget)" assertions on show. */
        g_signal_connect(about_widget, "destroy", G_CALLBACK(gtk_widget_destroyed), &about_widget);
    }

    gtk_widget_show_all(about_widget);
}

/* If the main project provided an embedded favicon header, write it to the
 * per-user config dir as "favicon.ico" and return the path. Returns NULL on
 * failure. This allows the tray to use the same icon as the web UI. */
#if defined(__GNUC__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

static char *write_embedded_favicon(void) MAYBE_UNUSED;
static char *write_embedded_favicon(void) {
#ifdef HAVE_EMBEDDED_FAVICON
    if (assets_favicon_ico_len == 0) return NULL;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *dirpath = g_build_filename(home, user_config_rel_dir, NULL);
    if (!dirpath) return NULL;
    if (g_mkdir_with_parents(dirpath, 0700) != 0 && errno != EEXIST) {
        g_free(dirpath);
        return NULL;
    }
    char *outpath_g = g_build_filename(dirpath, "favicon.ico", NULL);
    char *outpath = strdup(outpath_g);
    g_free(outpath_g);
    FILE *f = fopen(outpath, "wb");
    if (!f) return NULL;
    size_t written = fwrite(assets_favicon_ico, 1, assets_favicon_ico_len, f);
    fclose(f);
    if (written != assets_favicon_ico_len) return NULL;
    return strdup(outpath);
#else
    return NULL;
#endif
}

/* Try setting the AppIndicator icon from preferred candidates.
 * Order: gui_tray/assets/icon.png, gui_tray/assets/about.png, embedded favicon.
 * We schedule a few retries because some DEs initialize the indicator area
 * late after resume; retries improve the chance the icon is picked up. */
static int icon_set_attempts = 0;

static void try_set_indicator_icon_once(void) {
    if (!indicator) return;

    const char *icon_small = "/usr/local/share/burn2cool_tray/assets/icon.png";
    const char *about_img = "/usr/local/share/burn2cool_tray/assets/about.png";
    /* Try to install a user-local themed icon so desktop environments that
     * prefer icon names (from the icon theme) can show our icon immediately.
     * The installed path follows the hicolor theme layout for 32x32 apps. */
    if (g_file_test(icon_small, G_FILE_TEST_EXISTS)) {
        /* attempt to install to ~/.local/share/icons/hicolor/32x32/apps/cpu-throttle.png */
        const char *home = getenv("HOME");
        if (home) {
            char *destdir = g_build_filename(home, ".local", "share", "icons", "hicolor", "32x32", "apps", NULL);
            if (destdir) {
                if (g_mkdir_with_parents(destdir, 0755) != 0 && errno != EEXIST) {
                    /* ignore errors */
                }
                char *destpath_g = g_build_filename(destdir, "cpu-throttle.png", NULL);
                char *destpath = destpath_g ? strdup(destpath_g) : NULL;
                if (destpath) {
                    /* copy file if missing */
                    if (!g_file_test(destpath, G_FILE_TEST_EXISTS)) {
                        FILE *src = fopen(icon_small, "rb");
                        if (src) {
                            FILE *dst = fopen(destpath, "wb");
                            if (dst) {
                                char buf[8192];
                                size_t n;
                                while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
                                fclose(dst);
                            }
                            fclose(src);
                        }
                    }
                    if (g_file_test(destpath, G_FILE_TEST_EXISTS)) {
                        app_indicator_set_icon_full(indicator, destpath, "cpu-throttle");
                        free(destpath);
                        g_free(destpath_g);
                        g_free(destdir);
                        return;
                    }
                    free(destpath);
                    g_free(destpath_g);
                }
                g_free(destdir);
            }
        }
        app_indicator_set_icon_full(indicator, icon_small, "cpu-throttle");
        return;
    }
    if (g_file_test(icon_small, G_FILE_TEST_EXISTS)) {
        app_indicator_set_icon_full(indicator, icon_small, "cpu-throttle");
        return;
    }
    if (g_file_test(about_img, G_FILE_TEST_EXISTS)) {
        app_indicator_set_icon_full(indicator, about_img, "cpu-throttle");
        return;
    }
    char *favpath = write_embedded_favicon();
    if (favpath) {
        app_indicator_set_icon_full(indicator, favpath, "cpu-throttle");
        free(favpath);
        return;
    }
}

static gboolean icon_retry_cb(gpointer user_data) {
    (void)user_data;
    if (icon_set_attempts >= 3) return FALSE;
    try_set_indicator_icon_once();
    icon_set_attempts++;
    /* continue retries until attempts exhausted */
    return icon_set_attempts < 3;
}

static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname ? progname : "tray");
    printf("Options:\n");
    printf("  --port <port>      Set the daemon HTTP port (default: 8086)\n");
    printf("  --lang <code>      Force UI language (overrides env and saved preference)\n");
    printf("  --config <path>    Use a custom per-user config file path (reads/writes {\"lang\": \"<code>\"})\n");
    printf("  -h, --help         Show this help and exit\n");
    printf("  -V, --version      Print program version and exit\n");
}

static void on_profile_activate(GtkMenuItem *item, gpointer user_data) {
    const char *name = (const char*)user_data;
    if (!name) return;
    char json[512];
    snprintf(json, sizeof(json), "{\"cmd\":\"load-profile %s\"}", name);
    char *resp = NULL;
    int r = http_post_json("/api/command", json, &resp);
    if (r == 0) {
        notify_init("cpu-throttle-tray");
        const char *summary_title = get_loc("messages.loaded_profile_title", "Loaded profile");
        const char *loaded_fmt = get_loc("messages.loaded_profile_body", "Profile '%s' is now active.");
            // Try to parse the response JSON to show a friendly message instead of raw JSON
            if (resp) {
                struct json_object *jresp = json_tokener_parse(resp);
                if (jresp && json_object_is_type(jresp, json_type_object)) {
                    struct json_object *jok = NULL, *jloaded = NULL, *jmsg = NULL;
                    gboolean ok = FALSE;
                    if (json_object_object_get_ex(jresp, "ok", &jok) && json_object_is_type(jok, json_type_boolean)) {
                        ok = json_object_get_boolean(jok);
                    }
                    if (ok && json_object_object_get_ex(jresp, "loaded", &jloaded) && json_object_is_type(jloaded, json_type_string)) {
                        const char *loaded_name = json_object_get_string(jloaded);
                    char body[256];
                    snprintf(body, sizeof(body), loaded_fmt, loaded_name);
                    NotifyNotification *n = notify_notification_new(summary_title, body, NULL);
                        notify_notification_set_timeout(n, 2000);
                        notify_notification_show(n, NULL);
                        g_object_unref(n);
                    } else if (json_object_object_get_ex(jresp, "error", &jmsg) && json_object_is_type(jmsg, json_type_string)) {
                        const char *err = json_object_get_string(jmsg);
                    const char *load_failed_title = get_loc("messages.load_failed_title", "Load failed");
                    const char *load_failed_fmt = get_loc("messages.load_failed_body", "Failed to load profile '%s': %s");
                    char body[256];
                    snprintf(body, sizeof(body), load_failed_fmt, name, err);
                    NotifyNotification *n = notify_notification_new(load_failed_title, body, NULL);
                        notify_notification_set_timeout(n, 2000);
                        notify_notification_show(n, NULL);
                        g_object_unref(n);
                    } else if (json_object_object_get_ex(jresp, "message", &jmsg) && json_object_is_type(jmsg, json_type_string)) {
                        const char *msg = json_object_get_string(jmsg);
                    char body[256];
                    snprintf(body, sizeof(body), "%s", msg);
                    NotifyNotification *n = notify_notification_new(summary_title, body, NULL);
                        notify_notification_set_timeout(n, 2000);
                        notify_notification_show(n, NULL);
                        g_object_unref(n);
                    } else {
                        // Unknown but valid JSON: show a concise, friendly summary
                    char body[256];
                    snprintf(body, sizeof(body), loaded_fmt, name);
                    NotifyNotification *n = notify_notification_new(summary_title, body, NULL);
                        notify_notification_set_timeout(n, 2000);
                        notify_notification_show(n, NULL);
                        g_object_unref(n);
                    }
                    if (jresp) json_object_put(jresp);
                } else {
                    // not JSON: show the basic friendly summary
                char body[256];
                snprintf(body, sizeof(body), loaded_fmt, name);
                NotifyNotification *n = notify_notification_new(summary_title, body, NULL);
                    notify_notification_set_timeout(n, 2000);
                    notify_notification_show(n, NULL);
                    g_object_unref(n);
                }
            } else {
            const char *no_resp = get_loc("messages.no_response", "No response from daemon.");
            NotifyNotification *n = notify_notification_new(summary_title, no_resp, NULL);
                notify_notification_set_timeout(n, 2000);
                notify_notification_show(n, NULL);
                g_object_unref(n);
            }
    } else {
        // HTTP request failed
        char body[256];
        snprintf(body, sizeof(body), "Failed to contact daemon to load profile '%s'.", name);
        NotifyNotification *n = notify_notification_new("Load failed", body, NULL);
        notify_notification_set_timeout(n, 2000);
        notify_notification_show(n, NULL);
        g_object_unref(n);
    }
    if (resp) free(resp);
}

static void populate_profiles_menu() {
    if (!profiles_menu) return;
    // clear existing
    GList *children = gtk_container_get_children(GTK_CONTAINER(profiles_menu));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    char *body = http_get("/api/profiles");
    if (!body) {
        const char *lbl = get_loc("menu.failed_fetch_profiles", "Failed to fetch profiles");
        GtkWidget *item = gtk_menu_item_new_with_label(lbl);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(profiles_menu), item);
        gtk_widget_show_all(profiles_menu);
        return;
    }
    int count = 0;
    char **list = parse_json_string_array(body, &count);
    free(body);
    if (count == 0) {
        const char *lbl = get_loc("menu.no_profiles_available", "No profiles available");
        GtkWidget *item = gtk_menu_item_new_with_label(lbl);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(profiles_menu), item);
    } else {
        for (int i = 0; i < count; ++i) {
            GtkWidget *mi = gtk_menu_item_new_with_label(list[i]);
            g_signal_connect(mi, "activate", G_CALLBACK(on_profile_activate), g_strdup(list[i]));
            gtk_menu_shell_append(GTK_MENU_SHELL(profiles_menu), mi);
        }
    }
    free_string_list(list, count);
    gtk_widget_show_all(profiles_menu);
}

static gboolean status_timer(gpointer user_data) {
    (void)user_data;
    char *body = http_get("/api/status");
    if (!body) {
        const char *lbl = get_loc("status.offline", "Status: Offline");
        if (status_item) gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), lbl);
        return TRUE; // keep timer
    }
    // parse JSON via json-c
    struct json_object *jobj = json_tokener_parse(body);
    if (jobj && json_object_is_type(jobj, json_type_object)) {
        struct json_object *jtemp = NULL, *jfreq = NULL;
        if (json_object_object_get_ex(jobj, "temperature", &jtemp)) {
            int temp = (int)json_object_get_double(jtemp);
            if (json_object_object_get_ex(jobj, "frequency", &jfreq)) {
                double freq_khz = json_object_get_double(jfreq);
                double freq_mhz = freq_khz / 1000.0;
                char label[128];
                const char *fmt = get_loc("status.format", "Status: %d°C, %.1f MHz");
                snprintf(label, sizeof(label), fmt, temp, freq_mhz);
                if (status_item) gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), label);
            } else {
                const char *lbl = get_loc("status.frequency_na", "Status: Frequency N/A");
                if (status_item) gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), lbl);
            }
        } else {
            const char *lbl = get_loc("status.temperature_na", "Status: Temperature N/A");
            if (status_item) gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), lbl);
        }
        json_object_put(jobj);
    }
    free(body);
    return TRUE;
}

/* Continuous status polling every 5 seconds (default behavior). */

/* refresh menu removed: populate_profiles_menu is called periodically and on open */

/* status_icon_popup removed — using AppIndicator for tray menu */

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    notify_init("cpu-throttle-tray");

    // simple arg parsing for --port and --lang (CLI overrides config/env)
    char cli_lang[8] = "";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("%s\n", APP_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            http_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            strncpy(cli_lang, argv[++i], sizeof(cli_lang)-1);
            cli_lang[sizeof(cli_lang)-1] = '\0';
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            strncpy(config_path_override, argv[++i], sizeof(config_path_override)-1);
            config_path_override[sizeof(config_path_override)-1] = '\0';
        }
    }

    // determine language: CLI -> user config -> env -> default(en)
    if (cli_lang[0]) {
        load_locale(cli_lang);
        strncpy(current_lang, cli_lang, sizeof(current_lang)-1);
        current_lang[sizeof(current_lang)-1] = '\0';
    } else {
        char *user_lang = NULL;
        if (config_path_override[0]) {
            // read directly from override path
            char *content = read_file_to_string(config_path_override);
            if (content) {
                struct json_object *j = json_tokener_parse(content);
                free(content);
                if (j) {
                    struct json_object *jlang = NULL;
                    if (json_object_object_get_ex(j, "lang", &jlang) && json_object_is_type(jlang, json_type_string)) {
                        const char *s = json_object_get_string(jlang);
                        user_lang = strdup(s);
                    }
                    json_object_put(j);
                }
            }
        } else {
            user_lang = read_user_config_lang();
        }
        if (user_lang) {
            load_locale(user_lang);
            strncpy(current_lang, user_lang, sizeof(current_lang)-1);
            current_lang[sizeof(current_lang)-1] = '\0';
            free(user_lang);
        } else {
            // fallback to env-based detection
            load_locale_from_env();
        }
    }

    // Build menu
    GtkWidget *menu = gtk_menu_new();

    const char *init_lbl = get_loc("menu.status_initializing", "Status: Initializing...");
    status_item = gtk_menu_item_new_with_label(init_lbl);
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);

    /* version label (non-selectable) */
    const char *version_fmt = get_loc("menu.version_label", "Version: %s");
    char version_label[128];
    snprintf(version_label, sizeof(version_label), version_fmt, APP_VERSION);
    GtkWidget *version_item = gtk_menu_item_new_with_label(version_label);
    gtk_widget_set_sensitive(version_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), version_item);

    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

    const char *profiles_lbl = get_loc("menu.profiles_label", "Profiles");
    GtkWidget *profiles_label = gtk_menu_item_new_with_label(profiles_lbl);
    profiles_label_widget = profiles_label;
    profiles_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(profiles_label), profiles_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), profiles_label);

    /* Overview (moved up) */
    const char *overview_lbl = get_loc("menu.overview", "Overview");
    GtkWidget *overview_mi = gtk_menu_item_new_with_label(overview_lbl);
    g_signal_connect(overview_mi, "activate", G_CALLBACK(on_show_overview), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), overview_mi);

    const char *open_web_lbl = get_loc("menu.open_web_ui", "Open Web UI");
    GtkWidget *open_web = gtk_menu_item_new_with_label(open_web_lbl);
    open_web_widget = open_web;
    g_signal_connect(open_web, "activate", G_CALLBACK(on_open_webui), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_web);

    const char *quit_lbl = get_loc("menu.quit", "Quit");
    // Language submenu (populated from i18n/*.json)
    const char *lang_lbl = get_loc("menu.language", "Language");
    language_menu = gtk_menu_new();
    GtkWidget *language_label = gtk_menu_item_new_with_label(lang_lbl);
    language_label_widget = language_label;
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(language_label), language_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), language_label);

    // populate available locales into the language submenu
    populate_language_menu();

    /* About menu item */
    const char *about_lbl = get_loc("menu.about", "About");
    GtkWidget *about_mi = gtk_menu_item_new_with_label(about_lbl);
    g_signal_connect(about_mi, "activate", G_CALLBACK(on_show_version), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), about_mi);

    GtkWidget *quit = gtk_menu_item_new_with_label(quit_lbl);
    quit_widget = quit;
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);

    gtk_widget_show_all(menu);

    indicator = app_indicator_new("cpu-throttle-ind", "utilities-system-monitor", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    /* Try setting the icon immediately and schedule retries to handle late DE initialization */
    try_set_indicator_icon_once();
    /* schedule two more retries at 5s and 30s */
    g_timeout_add_seconds(5, icon_retry_cb, NULL);
    g_timeout_add_seconds(30, icon_retry_cb, NULL);

    populate_profiles_menu();
    /* perform one immediate status fetch so we have values at startup, but do
     * not register a continuous timer here. Polling will run only while the
     * tray menu is visible (see menu show/hide handlers below). */
    status_timer(NULL);

    /* add periodic status update every 5 seconds */
    g_timeout_add_seconds(5, status_timer, NULL);

    gtk_main();

    curl_global_cleanup();
    notify_uninit();
    return 0;
}
