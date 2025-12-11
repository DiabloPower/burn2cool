/* Simple Overview window: draws temperature and frequency history using Cairo.
 * Polls the daemon /api/status periodically and stores samples in a ring buffer.
 * This file purposely copies a small HTTP helper instead of sharing static
 * functions from burn2cool_tray.c to keep integration minimal.
 */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define HISTORY_LEN 300
#define POLL_INTERVAL_MS 2000

typedef struct {
    char *data;
    size_t size;
} MemoryChunk;

static int http_port = 8086;
static int use_http_first = 0;

static GtkWidget *overview_window = NULL;
static GtkWidget *drawing_area = NULL;
static double temps[HISTORY_LEN];
static int freqs[HISTORY_LEN];
static int head = 0; // next write position
static int count = 0; // samples stored (<= HISTORY_LEN)
static GMutex data_lock;
static guint poll_id = 0;

/* Cleanup the overview window: stop polling and clear references so the
 * main application can continue running after the window is closed. */
static void overview_on_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    if (poll_id) {
        g_source_remove(poll_id);
        poll_id = 0;
    }
    drawing_area = NULL;
    overview_window = NULL;
    /* release the mutex state */
    g_mutex_clear(&data_lock);
}

/* --- tiny HTTP helpers (copy) --- */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    MemoryChunk *mem = (MemoryChunk*)userdata;
    char *ptr_new = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr_new) return 0;
    mem->data = ptr_new;
    memcpy(&(mem->data[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static char *http_build_url(const char *path) {
    int needed = snprintf(NULL, 0, "http://localhost:%d%s", http_port, path);
    if (needed <= 0) return NULL;
    char *url = malloc((size_t)needed + 1);
    if (!url) return NULL;
    snprintf(url, (size_t)needed + 1, "http://localhost:%d%s", http_port, path);
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
    // Try socket first (more resource-efficient)
    char *result = NULL;
    if (strcmp(path, "/api/status") == 0) {
        result = socket_get("status json");
    }
    if (result) return result;

    if (use_http_first) {
        // Fallback to HTTP if socket failed and HTTP is preferred
        CURL *curl = curl_easy_init();
        if (curl) {
            MemoryChunk chunk = { .data = malloc(1), .size = 0 };
            char *url = http_build_url(path);
            if (!url) { free(chunk.data); curl_easy_cleanup(curl); return NULL; }
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
            CURLcode res = curl_easy_perform(curl);
            free(url);
            curl_easy_cleanup(curl);
            if (res == CURLE_OK) {
                return chunk.data;
            }
            free(chunk.data);
        }
    }

    return NULL;
}

/* --- data handling --- */
static void push_sample(double temp, int freq) {
    g_mutex_lock(&data_lock);
    temps[head] = temp;
    freqs[head] = freq;
    head = (head + 1) % HISTORY_LEN;
    if (count < HISTORY_LEN) count++;
    g_mutex_unlock(&data_lock);
}

static void get_samples(double *out_temps, int *out_freqs, int *out_count) {
    g_mutex_lock(&data_lock);
    int c = count;
    for (int i = 0; i < c; ++i) {
        int idx = (head - c + i + HISTORY_LEN) % HISTORY_LEN;
        out_temps[i] = temps[idx];
        out_freqs[i] = freqs[idx];
    }
    *out_count = c;
    g_mutex_unlock(&data_lock);
}

/* drawing */
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data; (void)widget;
    int c = 0;
    double tbuf[HISTORY_LEN];
    int fbuf[HISTORY_LEN];
    get_samples(tbuf, fbuf, &c);

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    /* Query the widget style to draw a themed background using the current
     * style context. `gtk_render_background` is the recommended (non-deprecated)
     * way to let the theme draw the widget background. */
    GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
    gtk_render_background(ctx, cr, 0, 0, width, height);

    /* Determine whether the theme is 'dark' by sampling the computed text
     * (foreground) color; if the text color is light, the background is
     * assumed dark and vice versa. This avoids using the deprecated API. */
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
    double luminance = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue;
    gboolean prefer_dark = luminance > 0.5; /* light text -> dark theme */

    if (c == 0) {
        /* reuse ctx and fg obtained above */
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 10, 22);
        cairo_show_text(cr, "No samples yet");
        return FALSE;
    }

    // compute min/max for temperature and frequency
    double tmin, tmax;
    int fmin, fmax;
    if (c > 0) {
        tmin = tbuf[0]; tmax = tbuf[0];
        fmin = fbuf[0]; fmax = fbuf[0];
        for (int i = 1; i < c; ++i) {
        if (tbuf[i] < tmin) tmin = tbuf[i];
        if (tbuf[i] > tmax) tmax = tbuf[i];
        if (fbuf[i] < fmin) fmin = fbuf[i];
        if (fbuf[i] > fmax) fmax = fbuf[i];
        }
    } else {
        tmin = tmax = 0.0;
        fmin = fmax = 0;
    }
    // add small padding
    if (tmax - tmin < 1.0) { tmax += 1.0; tmin -= 1.0; }
    if (fmax - fmin < 100) { fmax += 100; fmin = (fmin>100)?fmin-100:0; }

    // layout: two stacked plots
    int pad = 8;
    int plot_h = (height - 3*pad) / 2;

    /* Determine required left margin dynamically by measuring the widest
     * Y label (temperature and frequency) so labels never get clipped. */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    int y_ticks = 4;
    double max_label_w = 0.0;
    for (int ti = 0; ti <= y_ticks; ++ti) {
        double frac = (double)ti / (double)y_ticks;
        double tval = tmax - frac * (tmax - tmin);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%.1f°C", tval);
        cairo_text_extents_t te;
        cairo_text_extents(cr, lbl, &te);
        if (te.width > max_label_w) max_label_w = te.width;
    }
    for (int ti = 0; ti <= y_ticks; ++ti) {
        double frac = (double)ti / (double)y_ticks;
        double fval = fmax - frac * (fmax - fmin);
        char lblf[64];
        double vmhz = fval / 1000.0;
        snprintf(lblf, sizeof(lblf), "%.1f MHz", vmhz);
        cairo_text_extents_t tef;
        cairo_text_extents(cr, lblf, &tef);
        if (tef.width > max_label_w) max_label_w = tef.width;
    }
    int left_margin = pad + (int)ceil(max_label_w) + 12; /* padding inside margin */
    int plot_w = width - (pad + left_margin) - pad;

    // draw temperature plot (top)
    int top_x = pad + left_margin, top_y = pad;
    /* panel bg depending on theme */
    if (prefer_dark) cairo_set_source_rgb(cr, 0.16, 0.16, 0.18); else cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, top_x, top_y, plot_w, plot_h);
    cairo_fill(cr);
    // axes border
    if (prefer_dark) cairo_set_source_rgb(cr, 0.45, 0.45, 0.45); else cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_rectangle(cr, top_x + 0.5, top_y + 0.5, plot_w-1, plot_h-1);
    cairo_stroke(cr);

    /* draw horizontal grid lines and Y labels */
    cairo_set_line_width(cr, 1.0);
    double grid_alpha = prefer_dark ? 0.18 : 0.2;
    double dash_arr[2] = {4.0, 4.0};
    cairo_set_dash(cr, dash_arr, 2, 0);
    for (int ti = 0; ti <= y_ticks; ++ti) {
        double frac = (double)ti / (double)y_ticks; /* 0..1 from top to bottom */
        double val = tmax - frac * (tmax - tmin);
        double y = top_y + frac * (plot_h-1);
        if (prefer_dark) cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, grid_alpha); else cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, grid_alpha);
        cairo_move_to(cr, top_x + 1, y + 0.5);
        cairo_line_to(cr, top_x + plot_w - 2, y + 0.5);
        cairo_stroke(cr);

        /* label: right-align within left margin and clamp vertical pos so it
         * doesn't overlap the plot borders */
        char lbl_val[64];
        snprintf(lbl_val, sizeof(lbl_val), "%.1f°C", val);
        gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te;
        cairo_text_extents(cr, lbl_val, &te);
        double label_x = pad + left_margin - 6 - te.width; /* right align */
        double label_y = y + te.height / 2.0;
        /* clamp label_y inside plot area with small top/bottom padding */
        double min_label_y = top_y + 12;
        double max_label_y = top_y + plot_h - 6;
        if (label_y < min_label_y) label_y = min_label_y;
        if (label_y > max_label_y) label_y = max_label_y;
        cairo_move_to(cr, label_x, label_y);
        cairo_show_text(cr, lbl_val);
    }
    cairo_set_dash(cr, NULL, 0, 0);

    // plot temp line
    cairo_set_line_width(cr, 2.0);
    if (prefer_dark) cairo_set_source_rgb(cr, 1.0, 0.45, 0.45); else cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
    for (int i = 0; i < c; ++i) {
        double x = top_x + ((double)i) / (c-1) * (plot_w-1);
        double t = tbuf[i];
        double y = top_y + (1.0 - (t - tmin) / (tmax - tmin)) * (plot_h-1);
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    // label current temp (larger for readability)
    char lbl[128];
    snprintf(lbl, sizeof(lbl), "Temp: %.1f°C (min %.1f max %.1f)", tbuf[c-1], tmin, tmax);
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_move_to(cr, top_x + 6, top_y + 20);
    cairo_show_text(cr, lbl);

    // draw frequency plot (bottom)
    int bot_x = pad + left_margin, bot_y = pad*2 + plot_h;
    if (prefer_dark) cairo_set_source_rgb(cr, 0.16, 0.16, 0.18); else cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_rectangle(cr, bot_x, bot_y, plot_w, plot_h);
    cairo_fill(cr);
    if (prefer_dark) cairo_set_source_rgb(cr, 0.45, 0.45, 0.45); else cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_rectangle(cr, bot_x + 0.5, bot_y + 0.5, plot_w-1, plot_h-1);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 2.0);
    if (prefer_dark) cairo_set_source_rgb(cr, 0.45, 0.6, 1.0); else cairo_set_source_rgb(cr, 0.1, 0.2, 0.8);
    for (int i = 0; i < c; ++i) {
        double x = bot_x + ((double)i) / (c-1) * (plot_w-1);
        double f = fbuf[i];
        double y = bot_y + (1.0 - (f - fmin) / (double)(fmax - fmin)) * (plot_h-1);
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    /* show frequency in MHz to save horizontal space */
    double cur_mhz = ((double)fbuf[c-1]) / 1000.0;
    double min_mhz = ((double)fmin) / 1000.0;
    double max_mhz = ((double)fmax) / 1000.0;
    snprintf(lbl, sizeof(lbl), "Freq: %.1f MHz (min %.1f max %.1f)", cur_mhz, min_mhz, max_mhz);
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, bot_x + 6, bot_y + 20);
    cairo_show_text(cr, lbl);

    /* draw Y axis ticks for frequency plot */
    int f_ticks = 4;
    cairo_set_line_width(cr, 1.0);
    cairo_set_dash(cr, dash_arr, 2, 0);
    for (int ti = 0; ti <= f_ticks; ++ti) {
        double frac = (double)ti / (double)f_ticks; /* 0..1 top->bottom */
        double val = fmax - frac * (fmax - fmin);
        double y = bot_y + frac * (plot_h-1);
        if (prefer_dark) cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, grid_alpha); else cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, grid_alpha);
        cairo_move_to(cr, bot_x + 1, y + 0.5);
        cairo_line_to(cr, bot_x + plot_w - 2, y + 0.5);
        cairo_stroke(cr);

        char lblf[64];
        double vmhz = val / 1000.0;
        snprintf(lblf, sizeof(lblf), "%.1f MHz", vmhz);
        gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        /* right-align frequency labels within left margin and clamp vertical pos */
        cairo_text_extents_t tef;
        cairo_text_extents(cr, lblf, &tef);
        double label_xf = pad + left_margin - 6 - tef.width;
        double label_yf = y + tef.height / 2.0;
        double min_label_yf = bot_y + 12;
        double max_label_yf = bot_y + plot_h - 6;
        if (label_yf < min_label_yf) label_yf = min_label_yf;
        if (label_yf > max_label_yf) label_yf = max_label_yf;
        cairo_move_to(cr, label_xf, label_yf);
        cairo_show_text(cr, lblf);
    }
    cairo_set_dash(cr, NULL, 0, 0);

    return FALSE;
}

/* Polling: request /api/status every POLL_INTERVAL_MS and push sample if available */
static gboolean poll_cb(gpointer user_data) {
    (void)user_data;
    char *body = http_get("/api/status");
    if (!body) return TRUE; // keep polling
    struct json_object *j = json_tokener_parse(body);
    free(body);
    if (!j) return TRUE;
    if (json_object_is_type(j, json_type_object)) {
        struct json_object *jtemp = NULL, *jfreq = NULL;
        double tempv = NAN; int freqv = 0;
        if (json_object_object_get_ex(j, "temperature", &jtemp)) {
            if (json_object_is_type(jtemp, json_type_double) || json_object_is_type(jtemp, json_type_int)) {
                tempv = json_object_get_double(jtemp);
            }
        }
        if (json_object_object_get_ex(j, "frequency", &jfreq)) {
            if (json_object_is_type(jfreq, json_type_int)) {
                freqv = json_object_get_int(jfreq);
            }
        }
        if (!isnan(tempv) && freqv > 0) push_sample(tempv, freqv);
        gtk_widget_queue_draw(drawing_area);
    }
    json_object_put(j);
    return TRUE;
}

/* Public API */
void overview_set_port(int port) {
    http_port = port;
    // Don't force HTTP preference - let it use socket first for efficiency
    // use_http_first remains 0 (socket preferred) unless explicitly set
}

void overview_show(GtkWindow *parent) {
    if (overview_window) {
        gtk_window_present(GTK_WINDOW(overview_window));
        return;
    }
    g_mutex_init(&data_lock);
    overview_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(overview_window), 640, 360);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(overview_window), parent);
    gtk_window_set_title(GTK_WINDOW(overview_window), "Burn2Cool — Overview");

    /* Try to set a window icon from the bundled assets if present. */
    const char *icon_path = "assets/icon.png";
    if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
        GError *gerr = NULL;
        GdkPixbuf *icon = gdk_pixbuf_new_from_file(icon_path, &gerr);
        if (icon) {
            gtk_window_set_icon(GTK_WINDOW(overview_window), icon);
            g_object_unref(icon);
        } else {
            if (gerr) g_error_free(gerr);
        }
    }

    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_widget_set_hexpand(drawing_area, TRUE);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), drawing_area, TRUE, TRUE, 6);

    gtk_container_add(GTK_CONTAINER(overview_window), box);
    gtk_widget_show_all(overview_window);

    // start polling
    poll_id = g_timeout_add(POLL_INTERVAL_MS, poll_cb, NULL);

    /* When the overview window is destroyed, stop polling and clear state
     * so the tray/main loop keeps running. */
    g_signal_connect(overview_window, "destroy", G_CALLBACK(overview_on_destroy), NULL);
}
