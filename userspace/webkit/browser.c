/* Surfer on WebKit: the browser's window, tabs, toolbar and downloads, with
 * WPE WebKit (Safari's engine) rendering the pages.
 *
 * It is a Linux program (built against Alpine's WPE WebKit, run on ICDA's
 * Linux personality) that draws on ICDA's desktop: the window comes from
 * ICDA's window manager through the kernel's gateway (gui.c with
 * ICDA_SYS_BASE), the chrome is drawn with ICDA's own graphics and fonts
 * (ic_gfx.c, ic_font.c).  Each tab's web process paints frames into shared
 * memory (WPEBackend-fdo's SHM mode, Mesa's software renderer); the active
 * tab's frame is copied under the chrome.
 *
 * Surfer (browser.app) starts this when a Linux root with WebKit is present.
 *
 * usage: icda-webkit [url] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wayland-server.h>
#include "gui.h"
#include "ic_gfx.h"
#include "ic_font.h"

#define MAX_TABS   12
#define HOME_URL   "https://duckduckgo.com/"
#define SEARCH_URL "https://duckduckgo.com/?q="
#define DL_DIR     "/home/Downloads"

/* colours (ARGB), matching ICDA's dark window chrome */
#define C_BG        0xFF2B2B2Eu
#define C_TAB       0xFF3A3A3Eu
#define C_TAB_HOVER 0xFF333336u
#define C_FIELD     0xFF1E1E21u
#define C_FIELD_ON  0xFF161618u
#define C_TEXT      0xFFF2F2F7u
#define C_TEXT2     0xFF98989Fu
#define C_ACCENT    0xFF0A84FFu
#define C_LINE      0xFF404044u

typedef struct {
    WebKitWebView *view;
    struct wpe_view_backend_exportable_fdo *exp;
    struct wpe_view_backend *wb;
    struct wpe_view_backend_exportable_fdo_client client;   /* the backend keeps a pointer to it */
    uint32_t *frame;               /* the last picture, content-area sized */
    int fw, fh;
    char title[160];
    char uri[2048];
    double progress;
    int loading;
} tab_t;

static tab_t *tabs[MAX_TABS];      /* each its own allocation: the backend keeps pointers into it */
static int ntabs, active;
static int W, H, S;                /* window size (physical pixels), scale */
static int TABS_H, BAR_H, TOP;     /* chrome heights; content starts at TOP */
static char addr[2048];
static int addr_focus, addr_all;   /* editing the address; everything selected */
static char status[256];
static uint32_t status_until;
static int mouse_x, mouse_y;
static uint32_t buttons_down;
static int redraw_pending;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int content_h(void) { return H - TOP > 1 ? H - TOP : 1; }

static void set_status(const char *s, int ms) {
    snprintf(status, sizeof status, "%s", s);
    status_until = ms ? now_ms() + (uint32_t)ms : 0;
    redraw_pending = 1;
}

/* ---- drawing -------------------------------------------------------------------- */

static void draw_chevron(ic_canvas_t *c, int cx, int cy, int dir, ic_color_t col) {
    dir = -dir;                                   /* dir: -1 points left, 1 right */
    float d = 5.0f * S;
    ic_gfx_line(c, cx + dir * d * 0.5f, cy - d, cx - dir * d * 0.5f, cy, 1.8f * S, col);
    ic_gfx_line(c, cx - dir * d * 0.5f, cy, cx + dir * d * 0.5f, cy + d, 1.8f * S, col);
}

static void draw_reload(ic_canvas_t *c, int cx, int cy, int stop, ic_color_t col) {
    float r = 6.0f * S;
    if (stop) {
        ic_gfx_line(c, cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, 1.8f * S, col);
        ic_gfx_line(c, cx + r * 0.8f, cy - r * 0.8f, cx - r * 0.8f, cy + r * 0.8f, 1.8f * S, col);
        return;
    }
    ic_gfx_ring(c, cx, cy, r, 1.8f * S, col);
    ic_gfx_fill(c, cx, cy - (int)r - 2 * S, (int)r + 2 * S, (int)r + 1, C_BG);   /* open the ring */
    ic_gfx_line(c, cx, cy - r, cx + 4 * S, cy - r - 3 * S, 1.8f * S, col);
    ic_gfx_line(c, cx, cy - r, cx + 4 * S, cy - r + 3 * S, 1.8f * S, col);
}

/* rects of the chrome's parts (physical pixels) */
static ic_rect_t tab_rect(int i) {
    int avail = W - 44 * S, w = ntabs ? avail / ntabs : avail;
    ic_rect_t r;
    if (w > 220 * S) w = 220 * S;
    r.x = 6 * S + i * w;
    r.y = 4 * S;
    r.w = w - 4 * S;
    r.h = TABS_H - 4 * S;
    return r;
}

static ic_rect_t newtab_rect(void) {
    ic_rect_t last = tab_rect(ntabs - 1), r;
    r.x = last.x + last.w + 8 * S;
    r.y = 6 * S;
    r.w = 26 * S;
    r.h = TABS_H - 10 * S;
    return r;
}

static ic_rect_t button_rect(int i) {        /* 0 back, 1 forward, 2 reload */
    ic_rect_t r = { 8 * S + i * 36 * S, TABS_H + 6 * S, 32 * S, 32 * S };
    return r;
}

static ic_rect_t addr_rect(void) {
    ic_rect_t r = { 8 * S + 3 * 36 * S + 6 * S, TABS_H + 6 * S, 0, 32 * S };
    r.w = W - r.x - 10 * S;
    return r;
}

static int in_rect(ic_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void compose(void) {
    uint32_t *px = gui_pixel_buffer();
    ic_canvas_t c;
    const ic_face_t *body = ic_font(IC_FONT_BODY), *small = ic_font(IC_FONT_FOOTNOTE);
    tab_t *t = ntabs ? tabs[active] : 0;
    if (!px) return;
    c = ic_canvas_make(px, W, H);
    redraw_pending = 0;

    /* tab strip */
    ic_gfx_fill(&c, 0, 0, W, TOP, C_BG);
    for (int i = 0; i < ntabs; i++) {
        ic_rect_t r = tab_rect(i), tr;
        int hover = in_rect(r, mouse_x, mouse_y);
        if (i == active) ic_gfx_rrect4(&c, r.x, r.y, r.w, r.h + 4 * S, 8.0f * S, 8.0f * S, 0, 0, C_TAB);
        else if (hover) ic_gfx_rrect(&c, r.x, r.y + 2 * S, r.w, r.h - 4 * S, 7.0f * S, C_TAB_HOVER);
        tr = r;
        tr.x += 12 * S;
        tr.w -= 12 * S + (i == active ? 26 * S : 8 * S);
        {
            const char *title = tabs[i]->title[0] ? tabs[i]->title : tabs[i]->uri[0] ? tabs[i]->uri : "New tab";
            char cut[160];
            int n = ic_text_fit(small, title, tr.w);
            snprintf(cut, sizeof cut, "%.*s", n, title);
            ic_text_draw_in(&c, small, tr, cut, i == active ? C_TEXT : C_TEXT2, IC_ALIGN_LEFT);
        }
        if (tabs[i]->loading) ic_gfx_circle(&c, r.x + 6 * S, r.y + r.h / 2, 2.0f * S, C_ACCENT);
        if (i == active) {                                   /* close */
            int cx = r.x + r.w - 14 * S, cy = r.y + r.h / 2;
            ic_gfx_line(&c, cx - 4 * S, cy - 4 * S, cx + 4 * S, cy + 4 * S, 1.4f * S, C_TEXT2);
            ic_gfx_line(&c, cx + 4 * S, cy - 4 * S, cx - 4 * S, cy + 4 * S, 1.4f * S, C_TEXT2);
        }
    }
    {
        ic_rect_t r = newtab_rect();
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (in_rect(r, mouse_x, mouse_y)) ic_gfx_rrect(&c, r.x, r.y, r.w, r.h, 6.0f * S, C_TAB_HOVER);
        ic_gfx_line(&c, cx - 6 * S, cy, cx + 6 * S, cy, 1.6f * S, C_TEXT2);
        ic_gfx_line(&c, cx, cy - 6 * S, cx, cy + 6 * S, 1.6f * S, C_TEXT2);
    }

    /* toolbar */
    ic_gfx_fill(&c, 0, TABS_H, W, BAR_H, C_TAB);
    for (int i = 0; i < 3; i++) {
        ic_rect_t r = button_rect(i);
        int able = !t ? 0 : i == 0 ? webkit_web_view_can_go_back(t->view) : i == 1 ? webkit_web_view_can_go_forward(t->view) : 1;
        ic_color_t col = able ? C_TEXT : C_LINE;
        if (able && in_rect(r, mouse_x, mouse_y)) ic_gfx_circle(&c, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, C_TAB_HOVER);
        if (i < 2) draw_chevron(&c, r.x + r.w / 2, r.y + r.h / 2, i == 0 ? -1 : 1, col);
        else draw_reload(&c, r.x + r.w / 2, r.y + r.h / 2, t && t->loading, col);
    }
    {
        ic_rect_t r = addr_rect(), tr;
        const char *text = addr_focus ? addr : (t && t->uri[0] ? t->uri : "");
        ic_gfx_rrect(&c, r.x, r.y, r.w, r.h, 9.0f * S, addr_focus ? C_FIELD_ON : C_FIELD);
        if (addr_focus) ic_gfx_rrect_stroke(&c, r.x, r.y, r.w, r.h, 9.0f * S, 1.5f * S, C_ACCENT);
        tr = r;
        tr.x += 14 * S;
        tr.w -= 28 * S;
        if (addr_focus && addr_all && addr[0]) {
            int tw = ic_text_measure(body, addr);
            if (tw > tr.w) tw = tr.w;
            ic_gfx_rrect(&c, tr.x - 2 * S, r.y + 7 * S, tw + 4 * S, r.h - 14 * S, 3.0f * S, 0xFF1F4E8Cu);
        }
        if (!text[0] && !addr_focus) {
            ic_text_draw_in(&c, body, tr, "Search or enter address", C_TEXT2, IC_ALIGN_LEFT);
        } else {
            /* the end of a long address stays in view while typing */
            int len = (int)strlen(text), start = 0;
            if (addr_focus) while (start < len && ic_text_measure(body, text + start) > tr.w - 4 * S) start++;
            {
                char cut[2048];
                int n = ic_text_fit(body, text + start, tr.w);
                snprintf(cut, sizeof cut, "%.*s", n, text + start);
                ic_text_draw_in(&c, body, tr, cut, C_TEXT, IC_ALIGN_LEFT);
                if (addr_focus && !addr_all) {                       /* caret */
                    int cx = tr.x + ic_text_measure(body, cut) + 1;
                    ic_gfx_vline(&c, cx, r.y + 8 * S, r.h - 16 * S, C_ACCENT);
                }
            }
        }
    }
    if (t && t->loading && t->progress > 0 && t->progress < 1) {
        ic_gfx_fill(&c, 0, TOP - 2 * S, (int)(W * t->progress), 2 * S, C_ACCENT);
    } else {
        ic_gfx_hline(&c, 0, TOP - 1, W, C_LINE);
    }

    /* the page */
    if (t && t->frame) {
        int ch = content_h(), cw = t->fw < W ? t->fw : W, rows = t->fh < ch ? t->fh : ch;
        for (int y = 0; y < rows; y++)
            memcpy(px + (size_t)(TOP + y) * (size_t)W, t->frame + (size_t)y * (size_t)t->fw, (size_t)cw * 4);
        if (rows < ch) ic_gfx_fill(&c, 0, TOP + rows, W, ch - rows, 0xFFFFFFFFu);
        if (cw < W) ic_gfx_fill(&c, cw, TOP, W - cw, rows, 0xFFFFFFFFu);
    } else {
        ic_rect_t r = { 0, TOP, W, content_h() };
        ic_gfx_fill(&c, 0, TOP, W, content_h(), 0xFFFFFFFFu);
        ic_text_draw_in(&c, body, r, t && t->loading ? "Loading..." : "", 0xFF8E8E93u, IC_ALIGN_CENTER);
    }

    /* status line: downloads, links */
    if (status[0] && (!status_until || now_ms() < status_until)) {
        int tw = ic_text_measure(small, status) + 20 * S, sh = 24 * S;
        ic_gfx_rrect4(&c, 0, H - sh, tw, sh, 0, 8.0f * S, 0, 0, 0xEE2B2B2Eu);
        {
            ic_rect_t r = { 10 * S, H - sh, tw - 20 * S, sh };
            ic_text_draw_in(&c, small, r, status, C_TEXT, IC_ALIGN_LEFT);
        }
    } else if (status[0]) {
        status[0] = 0;
    }
    gui_flush();
}

/* ---- tabs ------------------------------------------------------------------------- */

static tab_t *tab_of_view(WebKitWebView *v) {
    for (int i = 0; i < ntabs; i++)
        if (tabs[i]->view == v) return tabs[i];
    return 0;
}

static tab_t *tab_of_exp(void *data) {
    return (tab_t *)data;
}

/* ---- diagnostics: WPE_TIMING prints when a load is asked for, starts,
 * commits and finishes, and the first picture after the commit (CLOCK_MONOTONIC
 * milliseconds: the kernel's clock).  WPE_THEN="seconds url" loads url that
 * long after the start, as the address bar does. -------------------------- */
static int timing_on, timing_painted;

static void timing(const char *what, const char *detail) {
    if (timing_on) fprintf(stderr, "icda-webkit timing: %s at ms %u %s\n", what, now_ms(), detail ? detail : "");
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) {
    static const char *const names[] = { "started", "redirected", "committed", "finished" };
    (void)d;
    if (ev == WEBKIT_LOAD_COMMITTED) timing_painted = 0;
    timing(ev <= WEBKIT_LOAD_FINISHED ? names[ev] : "load event", webkit_web_view_get_uri(v));
}

static gboolean then_load(gpointer url) {
    if (ntabs) {
        timing("requested", (const char *)url);
        webkit_web_view_load_uri(tabs[active]->view, (const char *)url);
    }
    return G_SOURCE_REMOVE;
}

/* WPE_TIMING: frames shown and the time spent showing them, every 50 frames */
static unsigned frames_n, frames_ms, frames_t0;

static void frame_stats(unsigned t0) {
    if (!timing_on) return;
    if (!frames_t0) frames_t0 = t0;
    frames_ms += now_ms() - t0;
    if (++frames_n == 50) {
        fprintf(stderr, "icda-webkit frames: 50 in %u ms, %u ms of it copying and drawing\n", now_ms() - frames_t0, frames_ms);
        frames_n = frames_ms = 0;
        frames_t0 = now_ms();
    }
}

static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer) {
    tab_t *t = tab_of_exp(data);
    unsigned t0 = now_ms();
    struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (shm) {
        int w = wl_shm_buffer_get_width(shm), h = wl_shm_buffer_get_height(shm), stride = wl_shm_buffer_get_stride(shm);
        if (t->fw != w || t->fh != h || !t->frame) {
            free(t->frame);
            t->frame = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
            t->fw = w;
            t->fh = h;
        }
        if (t->frame) {
            const uint8_t *src;
            wl_shm_buffer_begin_access(shm);
            src = wl_shm_buffer_get_data(shm);
            for (int y = 0; y < h; y++) {
                const uint32_t *s = (const uint32_t *)(src + (size_t)y * (size_t)stride);
                uint32_t *d = t->frame + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) d[x] = s[x] | 0xFF000000u;
            }
            wl_shm_buffer_end_access(shm);
        }
        if (t == tabs[active]) compose();
        if (!timing_painted) {
            timing_painted = 1;
            timing("first picture", "");
        }
        frame_stats(t0);
    }
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(t->exp);
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(t->exp, buffer);
}

static void sync_addr(void) {
    if (!addr_focus && ntabs) snprintf(addr, sizeof addr, "%s", tabs[active]->uri);
}

static void on_title(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    tab_t *t = tab_of_view(v);
    const char *s = webkit_web_view_get_title(v);
    (void)ps; (void)d;
    if (!t) return;
    if (s && getenv("WPE_EVAL") && !strncmp(s, "probe:", 6)) {
        fprintf(stderr, "icda-webkit probe: %s\n", s + 6);
        return;
    }
    snprintf(t->title, sizeof t->title, "%s", s ? s : "");
    if (getenv("WPE_CONSOLE")) fprintf(stderr, "icda-webkit title: %s\n", t->title);
    redraw_pending = 1;
}

static void on_uri(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    tab_t *t = tab_of_view(v);
    const char *s = webkit_web_view_get_uri(v);
    (void)ps; (void)d;
    if (!t) return;
    snprintf(t->uri, sizeof t->uri, "%s", s ? s : "");
    sync_addr();
    if (getenv("WPE_CONSOLE")) fprintf(stderr, "icda-webkit uri: %s\n", t->uri);
    redraw_pending = 1;
}

static void on_progress(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    tab_t *t = tab_of_view(v);
    (void)ps; (void)d;
    if (!t) return;
    t->progress = webkit_web_view_get_estimated_load_progress(v);
    t->loading = webkit_web_view_is_loading(v);
    redraw_pending = 1;
}

static gboolean on_fail(WebKitWebView *v, WebKitLoadEvent ev, char *uri, GError *err, gpointer d) {
    char msg[256];
    (void)v; (void)ev; (void)uri; (void)d;
    snprintf(msg, sizeof msg, "Could not load the page: %s", err ? err->message : "error");
    set_status(msg, 6000);
    return FALSE;
}

/* the page's web process ended: say so instead of leaving a blank page */
static void on_terminated(WebKitWebView *v, WebKitWebProcessTerminationReason reason, gpointer d) {
    const char *why = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory"
                    : reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : "was stopped";
    char msg[160];
    (void)v; (void)d;
    fprintf(stderr, "icda-webkit: web process %s (reason %d)\n", why, (int)reason);
    snprintf(msg, sizeof msg, "The page %s. Press F5 to load it again.", why);
    set_status(msg, 0);
}

static void on_hover(WebKitWebView *v, WebKitHitTestResult *hit, guint mods, gpointer d) {
    (void)v; (void)mods; (void)d;
    if (webkit_hit_test_result_context_is_link(hit)) set_status(webkit_hit_test_result_get_link_uri(hit), 0);
    else if (status[0] && !status_until) set_status("", 1);
}

static void set_sizes(tab_t *t) {
    wpe_view_backend_dispatch_set_size(t->wb, (uint32_t)(W / S), (uint32_t)(content_h() / S));
    wpe_view_backend_dispatch_set_device_scale_factor(t->wb, (float)S);
}

static void select_tab(int i) {
    if (i < 0 || i >= ntabs) return;
    if (ntabs && active < ntabs && active != i)
        wpe_view_backend_remove_activity_state(tabs[active]->wb, wpe_view_activity_state_visible | wpe_view_activity_state_focused);
    active = i;
    wpe_view_backend_add_activity_state(tabs[i]->wb, wpe_view_activity_state_visible | wpe_view_activity_state_focused |
                                                    wpe_view_activity_state_in_window);
    addr_focus = 0;
    sync_addr();
    compose();
}

static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d);
static WebKitSettings *settings;

static int add_tab(const char *url, WebKitWebView *related) {
    tab_t *t;
    WebKitWebViewBackend *backend;
    if (ntabs >= MAX_TABS) return -1;
    t = (tab_t *)calloc(1, sizeof(tab_t));
    if (!t) return -1;
    tabs[ntabs] = t;
    t->client.export_shm_buffer = on_export_shm_buffer;
    t->exp = wpe_view_backend_exportable_fdo_create(&t->client, t, (uint32_t)(W / S), (uint32_t)(content_h() / S));
    t->wb = wpe_view_backend_exportable_fdo_get_view_backend(t->exp);
    backend = webkit_web_view_backend_new(t->wb, NULL, NULL);
    if (related)
        t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "related-view", related, NULL));
    else
        t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "settings", settings, NULL));
    set_sizes(t);
    g_signal_connect(t->view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(t->view, "notify::uri", G_CALLBACK(on_uri), NULL);
    g_signal_connect(t->view, "notify::estimated-load-progress", G_CALLBACK(on_progress), NULL);
    g_signal_connect(t->view, "notify::is-loading", G_CALLBACK(on_progress), NULL);
    g_signal_connect(t->view, "load-failed", G_CALLBACK(on_fail), NULL);
    g_signal_connect(t->view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(t->view, "mouse-target-changed", G_CALLBACK(on_hover), NULL);
    g_signal_connect(t->view, "create", G_CALLBACK(on_create), NULL);
    g_signal_connect(t->view, "web-process-terminated", G_CALLBACK(on_terminated), NULL);
    ntabs++;
    select_tab(ntabs - 1);
    if (url) {
        snprintf(t->uri, sizeof t->uri, "%s", url);
        timing("requested", url);
        webkit_web_view_load_uri(t->view, url);
    }
    sync_addr();
    return ntabs - 1;
}

static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d) {
    (void)a; (void)d;
    if (add_tab(NULL, v) < 0) return NULL;
    return tabs[ntabs - 1]->view;
}

static void close_tab(int i) {
    tab_t *t;
    if (i < 0 || i >= ntabs) return;
    t = tabs[i];
    memmove(&tabs[i], &tabs[i + 1], sizeof(tab_t *) * (size_t)(ntabs - i - 1));
    ntabs--;
    webkit_web_view_try_close(t->view);
    g_object_unref(t->view);
    free(t->frame);
    t->frame = 0;
    /* the backend may still hold the tab: it stays allocated (a few hundred bytes) */
    if (!ntabs) exit(0);
    if (active >= ntabs) active = ntabs - 1;
    else if (active > i) active--;
    select_tab(active);
}

/* ---- the address bar ----------------------------------------------------------------- */

static void go(const char *text) {
    char url[3072];
    const char *s = text;
    while (*s == ' ') s++;
    if (!*s) return;
    if (strstr(s, "://") || !strncmp(s, "about:", 6) || !strncmp(s, "file:", 5) || !strncmp(s, "data:", 5)) {
        snprintf(url, sizeof url, "%s", s);
    } else if (strchr(s, '.') && !strchr(s, ' ')) {
        snprintf(url, sizeof url, "https://%s", s);
    } else {
        char *q = url + snprintf(url, sizeof url, "%s", SEARCH_URL);
        for (; *s && q < url + sizeof url - 4; s++) {
            unsigned char ch = (unsigned char)*s;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) *q++ = (char)ch;
            else if (ch == ' ') *q++ = '+';
            else q += sprintf(q, "%%%02X", ch);
        }
        *q = 0;
    }
    if (!ntabs) add_tab(url, NULL);
    else webkit_web_view_load_uri(tabs[active]->view, url);
    addr_focus = 0;
    snprintf(tabs[active]->uri, sizeof tabs[active]->uri, "%s", url);
    sync_addr();
    compose();
}

static void focus_addr(void) {
    addr_focus = 1;
    addr_all = 1;
    if (ntabs) snprintf(addr, sizeof addr, "%s", tabs[active]->uri);
    compose();
}

/* ---- input -------------------------------------------------------------------------- */

enum { M_SHIFT = 1, M_ALT = 2, M_CTRL = 4 };

static void web_key(uint32_t keysym, uint32_t mods) {
    struct wpe_input_keyboard_event ev;
    uint32_t wm = 0;
    if (!ntabs) return;
    if (mods & M_SHIFT) wm |= wpe_input_keyboard_modifier_shift;
    if (mods & M_ALT) wm |= wpe_input_keyboard_modifier_alt;
    if (mods & M_CTRL) wm |= wpe_input_keyboard_modifier_control;
    memset(&ev, 0, sizeof ev);
    ev.time = now_ms();
    ev.key_code = keysym;
    ev.modifiers = wm;
    ev.pressed = true;
    wpe_view_backend_dispatch_keyboard_event(tabs[active]->wb, &ev);
    ev.pressed = false;
    wpe_view_backend_dispatch_keyboard_event(tabs[active]->wb, &ev);
}

/* one key: browser shortcuts, the address bar, or the page */
static void key(uint32_t keysym, uint32_t mods) {
    tab_t *t = ntabs ? tabs[active] : 0;
    if (mods & M_CTRL) {
        switch (keysym) {
        case 'l': focus_addr(); return;
        case 't': add_tab(NULL, NULL); focus_addr(); return;
        case 'w': close_tab(active); return;
        case 'r': if (t) webkit_web_view_reload(t->view); return;
        case 0xff09: case 0xff56: select_tab((active + 1) % ntabs); return;          /* Ctrl+Tab, Ctrl+PgDn */
        case 0xff55: select_tab((active + ntabs - 1) % ntabs); return;               /* Ctrl+PgUp */
        }
    }
    if ((mods & M_ALT) && t) {
        if (keysym == 0xff51) { webkit_web_view_go_back(t->view); return; }
        if (keysym == 0xff53) { webkit_web_view_go_forward(t->view); return; }
    }
    if (keysym == 0xffc2 && t) { webkit_web_view_reload(t->view); return; }      /* F5 */
    if (addr_focus) {
        size_t n = strlen(addr);
        if (keysym == 0xff0d) { go(addr); return; }
        if (keysym == 0xff1b) { addr_focus = 0; sync_addr(); compose(); return; }
        if (mods & M_CTRL) {
            if (keysym == 'a') { addr_all = 1; compose(); }
            return;
        }
        if (keysym == 0xff08) {
            if (addr_all) addr[0] = 0;
            else if (n) {
                while (n > 0 && (addr[n - 1] & 0xC0) == 0x80) n--;   /* a whole UTF-8 character */
                addr[n ? n - 1 : 0] = 0;
            }
            addr_all = 0;
            compose();
            return;
        }
        if (keysym >= 0x20 && keysym < 0xff00) {
            uint32_t cp = keysym >= 0x01000000 ? keysym - 0x01000000 : keysym;
            char u[5];
            int k = 0;
            if (addr_all) { addr[0] = 0; n = 0; addr_all = 0; }
            if (cp < 0x80) u[k++] = (char)cp;
            else if (cp < 0x800) { u[k++] = (char)(0xC0 | cp >> 6); u[k++] = (char)(0x80 | (cp & 63)); }
            else { u[k++] = (char)(0xE0 | cp >> 12); u[k++] = (char)(0x80 | ((cp >> 6) & 63)); u[k++] = (char)(0x80 | (cp & 63)); }
            if (n + (size_t)k < sizeof addr - 1) {
                memcpy(addr + n, u, (size_t)k);
                addr[n + (size_t)k] = 0;
            }
            compose();
        }
        return;
    }
    web_key(keysym, mods);
}

/* ICDA sends keys as a terminal-style byte stream (ic_app.c decodes the same) */
static struct { int state, p1, p2; } kd;

static uint32_t keysym_of(uint32_t c) {
    switch (c) {
    case 8: case 127: return 0xff08;
    case 9: return 0xff09;
    case 13: case 10: return 0xff0d;
    case 27: return 0xff1b;
    }
    return c < 0x100 ? c : 0x01000000u | c;
}

static void plain_key(uint32_t c, uint32_t mods) {
    if (c >= 1 && c <= 26 && c != 8 && c != 9 && c != 13) key('a' + c - 1, mods | M_CTRL);
    else key(keysym_of(c), mods | ((c >= 'A' && c <= 'Z') ? M_SHIFT : 0));
}

static void feed_key(uint32_t c) {
    if (kd.state == 0) {
        if (c == 27) { kd.state = 1; return; }
        plain_key(c, 0);
        return;
    }
    if (kd.state == 1) {
        if (c == '[') { kd.state = 2; kd.p1 = kd.p2 = 0; return; }
        kd.state = 0;
        if (c >= 32 && c < 127) { plain_key(c, M_ALT); return; }
        key(0xff1b, 0);
        feed_key(c);
        return;
    }
    if (c >= '0' && c <= '9') {
        if (kd.state == 3) kd.p2 = kd.p2 * 10 + (int)(c - '0');
        else kd.p1 = kd.p1 * 10 + (int)(c - '0');
        return;
    }
    if (c == ';') { kd.state = 3; return; }
    {
        uint32_t mods = kd.p2 > 1 ? (uint32_t)(kd.p2 - 1) : 0;
        kd.state = 0;
        switch (c) {
        case 'A': key(0xff52, mods); return;
        case 'B': key(0xff54, mods); return;
        case 'C': key(0xff53, mods); return;
        case 'D': key(0xff51, mods); return;
        case 'H': key(0xff50, mods); return;
        case 'F': key(0xff57, mods); return;
        case 'Z': key(0xff09, mods | M_SHIFT); return;
        case '~':
            if (kd.p1 == 3) key(0xffff, mods);
            else if (kd.p1 == 2) key(0xff63, mods);
            else if (kd.p1 == 1 || kd.p1 == 7) key(0xff50, mods);
            else if (kd.p1 == 4 || kd.p1 == 8) key(0xff57, mods);
            else if (kd.p1 == 5) key(0xff55, mods);
            else if (kd.p1 == 6) key(0xff56, mods);
            else if (kd.p1 == 15) key(0xffc2, mods);             /* F5 */
            else if (kd.p1 == 27 && kd.p2) key(0, mods);
            return;
        }
    }
}

static void web_pointer(int x, int y, uint32_t buttons, int wheel) {
    struct wpe_input_pointer_event ev;
    static int last_x = -1, last_y = -1;
    static uint32_t last_buttons;
    struct wpe_view_backend *wb = tabs[active]->wb;
    int lx = x / S, ly = (y - TOP) / S;
    memset(&ev, 0, sizeof ev);
    ev.time = now_ms();
    ev.x = lx;
    ev.y = ly;
    if (lx != last_x || ly != last_y) {
        ev.type = wpe_input_pointer_event_type_motion;
        ev.modifiers = (last_buttons & GUI_BTN_LEFT) ? wpe_input_pointer_modifier_button1 : 0;
        wpe_view_backend_dispatch_pointer_event(wb, &ev);
        last_x = lx;
        last_y = ly;
    }
    for (int i = 0; i < 3; i++) {
        static const uint32_t bit[3] = { GUI_BTN_LEFT, GUI_BTN_RIGHT, GUI_BTN_MIDDLE };
        static const uint32_t wb_button[3] = { 1, 3, 2 };
        if ((buttons & bit[i]) == (last_buttons & bit[i])) continue;
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = wb_button[i];
        ev.state = (buttons & bit[i]) ? 1 : 0;
        wpe_view_backend_dispatch_pointer_event(wb, &ev);
    }
    last_buttons = buttons;
    if (wheel) {
        struct wpe_input_axis_2d_event ax;
        memset(&ax, 0, sizeof ax);
        ax.base.type = (enum wpe_input_axis_event_type)(wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
        ax.base.time = now_ms();
        ax.base.x = lx;
        ax.base.y = ly;
        ax.y_axis = -wheel * 53.0;
        wpe_view_backend_dispatch_axis_event(wb, &ax.base);
    }
}

static void mouse(int x, int y, uint32_t buttons, int wheel) {
    int pressed = (buttons & GUI_BTN_LEFT) && !(buttons_down & GUI_BTN_LEFT);
    int moved = x != mouse_x || y != mouse_y;
    mouse_x = x;
    mouse_y = y;
    if (y >= TOP || (buttons_down & GUI_BTN_LEFT && !pressed && y >= TOP)) {
        if (pressed && addr_focus) {
            addr_focus = 0;
            sync_addr();
            compose();
        }
        if (ntabs) web_pointer(x, y, buttons, wheel);
        buttons_down = buttons;
        return;
    }
    if (ntabs && (buttons_down & GUI_BTN_LEFT) && !(buttons & GUI_BTN_LEFT)) web_pointer(x, TOP, buttons, 0);   /* a drag ended above */
    buttons_down = buttons;
    if (pressed) {
        for (int i = 0; i < ntabs; i++) {
            ic_rect_t r = tab_rect(i);
            if (!in_rect(r, x, y)) continue;
            if (i == active && x >= r.x + r.w - 24 * S) close_tab(i);
            else select_tab(i);
            return;
        }
        if (in_rect(newtab_rect(), x, y)) { add_tab(NULL, NULL); focus_addr(); return; }
        if (ntabs && in_rect(button_rect(0), x, y)) { webkit_web_view_go_back(tabs[active]->view); return; }
        if (ntabs && in_rect(button_rect(1), x, y)) { webkit_web_view_go_forward(tabs[active]->view); return; }
        if (ntabs && in_rect(button_rect(2), x, y)) {
            if (tabs[active]->loading) webkit_web_view_stop_loading(tabs[active]->view);
            else webkit_web_view_reload(tabs[active]->view);
            return;
        }
        if (in_rect(addr_rect(), x, y)) { focus_addr(); return; }
    } else if (moved) {
        redraw_pending = 1;                                  /* hover highlights */
    }
}

static void layout(void) {
    W = gui_window_width();
    H = gui_window_height();
    S = gui_window_scale() > 0 ? gui_window_scale() : 1;
    TABS_H = 36 * S;
    BAR_H = 44 * S;
    TOP = TABS_H + BAR_H;
}

static gboolean poll_window(gpointer data) {
    gui_msg_t m;
    (void)data;
    while (gui_poll_event(&m)) {
        switch (m.type) {
        case GUI_MSG_KEY_EVENT:
            if (m.key.pressed) feed_key(m.key.keycode);
            break;
        case GUI_MSG_MOUSE_EVENT:
            mouse(m.mouse.x, m.mouse.y, m.mouse.buttons, m.mouse.wheel);
            break;
        case GUI_MSG_RESIZE:
            layout();
            for (int i = 0; i < ntabs; i++) set_sizes(tabs[i]);
            compose();
            break;
        case GUI_MSG_FOCUS:
            if (ntabs) {
                if (m.focus.focused) wpe_view_backend_add_activity_state(tabs[active]->wb, wpe_view_activity_state_focused);
                else wpe_view_backend_remove_activity_state(tabs[active]->wb, wpe_view_activity_state_focused);
            }
            break;
        case GUI_MSG_CLOSE_WINDOW:
            exit(0);
        }
    }
    if (status_until && now_ms() >= status_until) {
        status_until = 0;
        status[0] = 0;
        redraw_pending = 1;
    }
    if (redraw_pending) compose();
    return G_SOURCE_CONTINUE;
}

/* ---- downloads ------------------------------------------------------------------------ */

static void dl_progress(WebKitDownload *dl, GParamSpec *ps, gpointer name) {
    char msg[256];
    (void)ps;
    snprintf(msg, sizeof msg, "Downloading %s - %d%%", (const char *)name, (int)(webkit_download_get_estimated_progress(dl) * 100));
    set_status(msg, 0);
}

static void dl_finished(WebKitDownload *dl, gpointer name) {
    char msg[256];
    (void)dl;
    snprintf(msg, sizeof msg, "Downloaded %s to Downloads", (const char *)name);
    set_status(msg, 6000);
}

static void dl_failed(WebKitDownload *dl, GError *err, gpointer name) {
    char msg[256];
    (void)dl;
    snprintf(msg, sizeof msg, "Download of %s failed: %s", (const char *)name, err ? err->message : "error");
    set_status(msg, 8000);
}

static gboolean dl_destination(WebKitDownload *dl, const char *suggested, gpointer data) {
    char path[1024];
    const char *base = suggested && suggested[0] ? suggested : "download";
    (void)data;
    mkdir("/home", 0755);
    mkdir(DL_DIR, 0755);
    snprintf(path, sizeof path, "%s/%s", DL_DIR, base);
    for (int i = 1; i < 100; i++) {                          /* keep what is already there */
        struct stat st;
        if (stat(path, &st) != 0) break;
        snprintf(path, sizeof path, "%s/%d-%s", DL_DIR, i, base);
    }
    webkit_download_set_destination(dl, path);
    {
        char *name = g_strdup(strrchr(path, '/') + 1);
        g_signal_connect(dl, "notify::estimated-progress", G_CALLBACK(dl_progress), name);
        g_signal_connect(dl, "finished", G_CALLBACK(dl_finished), name);
        g_signal_connect(dl, "failed", G_CALLBACK(dl_failed), name);
    }
    return TRUE;
}

static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer d) {
    (void)s; (void)d;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(dl_destination), NULL);
    set_status("Starting download...", 0);
}

/* ---- start --------------------------------------------------------------------------- */

/* ---- diagnostics: WPE_EVAL runs a script in the page every WPE_EVAL_EVERY
 * seconds (default 60) and prints what it returns ------------------------------ */

static void eval_done(GObject *o, GAsyncResult *res, gpointer d) {
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), res, &err);
    (void)d;
    if (!v) {
        fprintf(stderr, "icda-webkit eval: error %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        return;
    }
    {
        char *s = jsc_value_to_string(v);
        fprintf(stderr, "icda-webkit eval: %s\n", s ? s : "(null)");
        g_free(s);
    }
    g_object_unref(v);
}

static gboolean eval_tick(gpointer d) {
    const char *js = getenv("WPE_EVAL");
    (void)d;
    if (js && ntabs) {
        char *wrapped = g_strdup_printf("String(%s)", js);
        webkit_web_view_evaluate_javascript(tabs[active]->view, wrapped, -1, NULL, NULL, NULL, eval_done, NULL);
        g_free(wrapped);
    }
    return G_SOURCE_CONTINUE;
}

static void env_default(const char *k, const char *v) {
    if (!getenv(k)) setenv(k, v, 1);
}

int main(int argc, char **argv) {
    const char *url = argc > 1 && argv[1][0] ? argv[1] : HOME_URL;
    GMainLoop *loop;

    timing_on = getenv("WPE_TIMING") != NULL;

    /* what WebKit needs on ICDA: software rendering, no sandbox helpers,
     * settings in memory, files under the user's home */
    env_default("HOME", "/home");
    env_default("XDG_RUNTIME_DIR", "/home");
    env_default("XDG_CACHE_HOME", "/home/.cache");
    env_default("XDG_DATA_HOME", "/home/.local/share");
    env_default("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
    env_default("LIBGL_ALWAYS_SOFTWARE", "1");
    env_default("GALLIUM_DRIVER", "llvmpipe");
    env_default("EGL_PLATFORM", "wayland");
    env_default("GSETTINGS_BACKEND", "memory");
    /* no PulseAudio server here: sound goes through icdasink, and probing
     * for PulseAudio devices only costs time */
    env_default("GST_PLUGIN_FEATURE_RANK", "pulsesink:NONE,pulsesrc:NONE,pulsedeviceprovider:NONE");
    /* the plugin registry made with the Linux root (tests/linux/mkroot.sh): with
     * none, GStreamer reads every plugin (seconds of disk reads) on first use */
    env_default("GST_REGISTRY", "/usr/lib/icda/gst-registry.bin");
    env_default("GST_REGISTRY_UPDATE", "no");

    if (gui_open_window("Surfer", 1100, 720) != 0) {
        fprintf(stderr, "icda-webkit: no desktop window\n");
        return 1;
    }
    layout();
    set_status("Starting WebKit...", 0);
    compose();

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "icda-webkit: WPEBackend-fdo shared memory mode failed\n");
        return 1;
    }
    settings = webkit_settings_new();
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Safari/605.1.15");
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, getenv("WPE_CONSOLE") != NULL);
    webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    g_signal_connect(webkit_network_session_get_default(), "download-started", G_CALLBACK(on_download), NULL);

    add_tab(url, NULL);
    if (getenv("WPE_EVAL")) g_timeout_add_seconds(getenv("WPE_EVAL_EVERY") ? (guint)atoi(getenv("WPE_EVAL_EVERY")) : 60, eval_tick, NULL);
    set_status("", 1);

    g_timeout_add(10, poll_window, NULL);
    if (getenv("WPE_THEN")) {
        char *spec = g_strdup(getenv("WPE_THEN")), *sp = strchr(spec, 0x20);
        if (sp) {
            *sp = 0;
            g_timeout_add((guint)(atof(spec) * 1000), then_load, sp + 1);
        }
    }
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
