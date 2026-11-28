/* Surfer on WebKit: the browser's window, tabs, toolbar and downloads, with
 * WPE WebKit (Safari's engine) rendering the pages.
 *
 * It is a Linux program (built against Alpine's WPE WebKit, run on ICDA's
 * Linux personality) that draws on ICDA's desktop: the window comes from
 * ICDA's window manager through the kernel's gateway (gui.c with
 * ICDA_SYS_BASE), the chrome is drawn with ICDA's own graphics and fonts
 * (ic_gfx.c, ic_font.c).  Each tab's web process paints frames into shared
 * memory (WPEBackend-fdo's SHM mode, Mesa's software renderer) or, with ICDA's
 * GPU driver, on the GPU (EGL mode, Mesa's Intel driver); the active
 * tab's frame is copied under the chrome.
 *
 * Frames are paced to the display (frame pacing, below) and only the rows of
 * a frame that changed are copied on; memory is watched (memory, below): web
 * processes give memory back as they grow and tabs not shown give theirs
 * back when the machine runs short.
 *
 * What a browser shows besides pages is drawn here too (popups, below): the
 * lists of <select> elements, the right-click menu, Surfer's own menu, the
 * downloads list, the save and open sheets (downloads and file uploads),
 * pages' alert, confirm and prompt dialogs, and the find bar.  A page's full
 * screen (a video's) hides the chrome.
 *
 * Surfer (browser.app) starts this when a Linux root with WebKit is present.
 *
 * usage: icda-webkit [url] */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/fdo-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <wayland-server.h>
#include "gui.h"
#include "ic_gfx.h"
#include "ic_font.h"

#define MAX_TABS   12
#define BADGE_W    (84 * S)           /* "GPU" or "Software", right of the address */
#define TOOL_W     (36 * S)           /* the downloads and menu buttons, right of the address */
#define HOME_URL   "https://duckduckgo.com/"
#define SEARCH_URL "https://duckduckgo.com/?q="
#define DL_DIR     "/home/Downloads"
#define CLIPBOARD  "/home/.clipboard" /* ICDA's clipboard: the text copied last, from any program */
/* WebKit's HTTP cache, in memory: on the ICDA partition (FAT) each of the
 * cache's file operations held ICDA's kernel lock for 50-130 ms, and a page
 * load made dozens of them - the whole machine (pointer, windows, sound)
 * stopped for each.  Wikipedia loaded in 17 s with it on the disk and 11 s in
 * memory, BBC News in 45 s and 10 s.  Cookies and site data stay on the disk. */
#define CACHE_DIR  "/tmp/surfer-cache"

/* colours (ARGB), matching ICDA's dark window chrome */
#define C_BG        0xFF2B2B2Eu
#define C_TAB       0xFF3A3A3Eu
#define C_TAB_HOVER 0xFF333336u
#define C_FIELD     0xFF1E1E21u
#define C_FIELD_ON  0xFF161618u
#define C_TEXT      0xFFF2F2F7u
#define C_TEXT2     0xFF98989Fu
#define C_TEXT3     0xFF6A6A70u       /* a discarded tab's title */
#define C_ACCENT    0xFF0A84FFu
#define C_LINE      0xFF404044u
#define C_POPUP     0xFF2C2C30u       /* menus, the downloads list, sheets */
#define C_SELECT    0xFF0A64D8u       /* the item under the pointer in a menu */

typedef struct {
    WebKitWebView *view;
    struct wpe_view_backend_exportable_fdo *exp;
    struct wpe_view_backend *wb;
    struct wpe_view_backend_exportable_fdo_client client;   /* the backend keeps a pointer to it */
    struct wpe_view_backend_exportable_fdo_egl_client egl_client;   /* the same, on the GPU */
    uint32_t *frame;               /* the last picture, content-area sized */
    int fw, fh;
    char title[160];
    char uri[2048];
    double progress;
    int loading;
    /* frame pacing (below) */
    guint ack_source;              /* the frame complete waiting to be sent, or 0 */
    uint32_t last_ack;             /* when the last one was sent (ms) */
    uint32_t last_change;          /* when a frame last changed something on screen (ms) */
    unsigned run;                  /* frames in a row that changed something */
    uint32_t changes[64];          /* when the last frames that changed something came (ms), a ring */
    unsigned nchanges;
    uint32_t draw_ms;              /* what drawing a frame takes lately (ms, averaged): frame complete to the frame */
    /* memory (below) */
    uint32_t last_shown;           /* when it was last the active tab (ms) */
    int discarded;                 /* its web process was ended to free memory: loads again when selected */
    int related;                   /* shares its web process with another tab (a popup and its opener) */
} tab_t;

static tab_t *tabs[MAX_TABS];      /* each its own allocation: the backend keeps pointers into it */
static int ntabs, active;
static int W, H, S;                /* window size (physical pixels), scale */
static int TABS_H, BAR_H, TOP;     /* chrome heights; content starts at TOP */
static char addr[2048];
static int addr_focus, addr_all;   /* editing the address; everything selected */
static int gpu_mode;                /* pages drawn on the GPU (below) */
static char gpu_reason[200];        /* why not, when not */
static char status[256];
static uint32_t status_until;
static ic_rect_t status_drawn;     /* where the status line is on screen (w 0: nowhere) */
static int mouse_x, mouse_y;
static uint32_t buttons_down;
static uint32_t page_buttons;      /* the buttons the page saw go down (a drag in it) */
static int chrome_dirty;           /* tabs, toolbar or address to draw again */
static int status_dirty;           /* the status line changed */
static int fullscreen;             /* the chrome hidden: a page in full screen, or asked for */
static WebKitNetworkSession *session;
static WebKitWebsitePolicies *policies;

/* popups (below): what is over the page */
static void draw_overlays(ic_canvas_t *c, int top, int bottom);
static int page_dimmed(void);

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int content_h(void) { return H - TOP > 1 ? H - TOP : 1; }

static void set_status(const char *s, int ms) {
    snprintf(status, sizeof status, "%s", s);
    status_until = ms ? now_ms() + (uint32_t)ms : 0;
    status_dirty = 1;
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
    r.w = W - r.x - 10 * S - BADGE_W - 2 * TOOL_W;
    return r;
}

static ic_rect_t tool_rect(int i) {          /* 0 downloads, 1 Surfer's menu */
    ic_rect_t a = addr_rect(), r = { a.x + a.w + 4 * S + i * TOOL_W, TABS_H + 6 * S, 32 * S, 32 * S };
    return r;
}

/* where the speaker of a tab playing sound (or muted) is */
static ic_rect_t tab_audio_rect(int i) {
    ic_rect_t r = tab_rect(i), a;
    a.w = 20 * S;
    a.h = 20 * S;
    a.x = r.x + r.w - (i == active ? 48 * S : 26 * S);
    a.y = r.y + (r.h - a.h) / 2;
    return a;
}

static int tab_has_audio(int i) {
    return webkit_web_view_is_playing_audio(tabs[i]->view) || webkit_web_view_get_is_muted(tabs[i]->view);
}

static void draw_speaker(ic_canvas_t *c, int cx, int cy, int muted, ic_color_t col) {
    float u = (float)S;
    ic_gfx_fill(c, cx - (int)(6 * u), cy - (int)(2 * u), (int)(3 * u), (int)(5 * u), col);
    ic_gfx_line(c, cx - 3 * u, cy - 2 * u, cx + 1 * u, cy - 6 * u, 1.6f * u, col);
    ic_gfx_line(c, cx - 3 * u, cy + 2 * u, cx + 1 * u, cy + 6 * u, 1.6f * u, col);
    ic_gfx_line(c, cx + 1 * u, cy - 6 * u, cx + 1 * u, cy + 6 * u, 1.6f * u, col);
    if (muted) {
        ic_gfx_line(c, cx + 4 * u, cy - 3 * u, cx + 9 * u, cy + 3 * u, 1.4f * u, col);
        ic_gfx_line(c, cx + 9 * u, cy - 3 * u, cx + 4 * u, cy + 3 * u, 1.4f * u, col);
    } else {
        ic_gfx_line(c, cx + 4 * u, cy - 3 * u, cx + 5 * u, cy, 1.4f * u, col);
        ic_gfx_line(c, cx + 5 * u, cy, cx + 4 * u, cy + 3 * u, 1.4f * u, col);
        ic_gfx_line(c, cx + 7 * u, cy - 5 * u, cx + 9 * u, cy, 1.4f * u, col);
        ic_gfx_line(c, cx + 9 * u, cy, cx + 7 * u, cy + 5 * u, 1.4f * u, col);
    }
}

static int downloads_running(double *progress);
static int dl_unseen;

static int in_rect(ic_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* the tab strip, the toolbar and the loading line: rows 0 .. TOP - 1 */
static void draw_chrome(ic_canvas_t *c) {
    const ic_face_t *body = ic_font(IC_FONT_BODY), *small = ic_font(IC_FONT_FOOTNOTE);
    tab_t *t = ntabs ? tabs[active] : 0;
    if (!TOP) return;                                    /* full screen */

    /* tab strip */
    ic_gfx_fill(c, 0, 0, W, TOP, C_BG);
    for (int i = 0; i < ntabs; i++) {
        ic_rect_t r = tab_rect(i), tr;
        int hover = in_rect(r, mouse_x, mouse_y), audio = tab_has_audio(i);
        if (i == active) ic_gfx_rrect4(c, r.x, r.y, r.w, r.h + 4 * S, 8.0f * S, 8.0f * S, 0, 0, C_TAB);
        else if (hover) ic_gfx_rrect(c, r.x, r.y + 2 * S, r.w, r.h - 4 * S, 7.0f * S, C_TAB_HOVER);
        tr = r;
        tr.x += 12 * S;
        tr.w -= 12 * S + (i == active ? 26 * S : 8 * S) + (audio ? 24 * S : 0);
        if (audio) {                                     /* playing sound: a click mutes it */
            ic_rect_t a = tab_audio_rect(i);
            int muted = webkit_web_view_get_is_muted(tabs[i]->view);
            if (in_rect(a, mouse_x, mouse_y)) ic_gfx_rrect(c, a.x, a.y, a.w, a.h, 5.0f * S, C_TAB_HOVER);
            draw_speaker(c, a.x + a.w / 2 - 1 * S, a.y + a.h / 2, muted, muted ? C_TEXT2 : C_TEXT);
        }
        {
            const char *title = tabs[i]->title[0] ? tabs[i]->title : tabs[i]->uri[0] ? tabs[i]->uri : "New tab";
            char cut[160];
            int n = ic_text_fit(small, title, tr.w);
            snprintf(cut, sizeof cut, "%.*s", n, title);
            ic_text_draw_in(c, small, tr, cut, i == active ? C_TEXT : tabs[i]->discarded ? C_TEXT3 : C_TEXT2, IC_ALIGN_LEFT);
        }
        if (tabs[i]->loading) ic_gfx_circle(c, r.x + 6 * S, r.y + r.h / 2, 2.0f * S, C_ACCENT);
        if (i == active) {                                   /* close */
            int cx = r.x + r.w - 14 * S, cy = r.y + r.h / 2;
            ic_gfx_line(c, cx - 4 * S, cy - 4 * S, cx + 4 * S, cy + 4 * S, 1.4f * S, C_TEXT2);
            ic_gfx_line(c, cx + 4 * S, cy - 4 * S, cx - 4 * S, cy + 4 * S, 1.4f * S, C_TEXT2);
        }
    }
    {
        ic_rect_t r = newtab_rect();
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (in_rect(r, mouse_x, mouse_y)) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 6.0f * S, C_TAB_HOVER);
        ic_gfx_line(c, cx - 6 * S, cy, cx + 6 * S, cy, 1.6f * S, C_TEXT2);
        ic_gfx_line(c, cx, cy - 6 * S, cx, cy + 6 * S, 1.6f * S, C_TEXT2);
    }

    /* toolbar */
    ic_gfx_fill(c, 0, TABS_H, W, BAR_H, C_TAB);
    for (int i = 0; i < 3; i++) {
        ic_rect_t r = button_rect(i);
        int able = !t ? 0 : i == 0 ? webkit_web_view_can_go_back(t->view) : i == 1 ? webkit_web_view_can_go_forward(t->view) : 1;
        ic_color_t col = able ? C_TEXT : C_LINE;
        if (able && in_rect(r, mouse_x, mouse_y)) ic_gfx_circle(c, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, C_TAB_HOVER);
        if (i < 2) draw_chevron(c, r.x + r.w / 2, r.y + r.h / 2, i == 0 ? -1 : 1, col);
        else draw_reload(c, r.x + r.w / 2, r.y + r.h / 2, t && t->loading, col);
    }
    {
        ic_rect_t r = addr_rect(), tr;
        const char *text = addr_focus ? addr : (t && t->uri[0] ? t->uri : "");
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 9.0f * S, addr_focus ? C_FIELD_ON : C_FIELD);
        if (addr_focus) ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, 9.0f * S, 1.5f * S, C_ACCENT);
        tr = r;
        tr.x += 14 * S;
        tr.w -= 28 * S;
        if (addr_focus && addr_all && addr[0]) {
            int tw = ic_text_measure(body, addr);
            if (tw > tr.w) tw = tr.w;
            ic_gfx_rrect(c, tr.x - 2 * S, r.y + 7 * S, tw + 4 * S, r.h - 14 * S, 3.0f * S, 0xFF1F4E8Cu);
        }
        if (!text[0] && !addr_focus) {
            ic_text_draw_in(c, body, tr, "Search or enter address", C_TEXT2, IC_ALIGN_LEFT);
        } else {
            /* the end of a long address stays in view while typing */
            int len = (int)strlen(text), start = 0;
            if (addr_focus) while (start < len && ic_text_measure(body, text + start) > tr.w - 4 * S) start++;
            {
                char cut[2048];
                int n = ic_text_fit(body, text + start, tr.w);
                snprintf(cut, sizeof cut, "%.*s", n, text + start);
                ic_text_draw_in(c, body, tr, cut, C_TEXT, IC_ALIGN_LEFT);
                if (addr_focus && !addr_all) {                       /* caret */
                    int cx = tr.x + ic_text_measure(body, cut) + 1;
                    ic_gfx_vline(c, cx, r.y + 8 * S, r.h - 16 * S, C_ACCENT);
                }
            }
        }
    }
    {   /* downloads: an arrow into a tray, a bar while some run, a dot for new ones */
        ic_rect_t r = tool_rect(0);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        double p = 0;
        int running = downloads_running(&p);
        if (in_rect(r, mouse_x, mouse_y)) ic_gfx_circle(c, cx, cy, r.w / 2, C_TAB_HOVER);
        ic_gfx_line(c, cx, cy - 7 * S, cx, cy + 3 * S, 1.6f * S, C_TEXT);
        ic_gfx_line(c, cx - 4 * S, cy - 1 * S, cx, cy + 3 * S, 1.6f * S, C_TEXT);
        ic_gfx_line(c, cx + 4 * S, cy - 1 * S, cx, cy + 3 * S, 1.6f * S, C_TEXT);
        ic_gfx_line(c, cx - 7 * S, cy + 7 * S, cx + 7 * S, cy + 7 * S, 1.6f * S, C_TEXT);
        if (running) {
            ic_gfx_fill(c, cx - 10 * S, r.y + r.h - 3 * S, 20 * S, 2 * S, C_LINE);
            ic_gfx_fill(c, cx - 10 * S, r.y + r.h - 3 * S, (int)(20 * S * (p > 0 ? p : 0.05)), 2 * S, C_ACCENT);
        } else if (dl_unseen) {
            ic_gfx_circle(c, cx + 8 * S, cy - 7 * S, 3.5f * S, C_ACCENT);
        }
    }
    {   /* Surfer's menu: three dots */
        ic_rect_t r = tool_rect(1);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (in_rect(r, mouse_x, mouse_y)) ic_gfx_circle(c, cx, cy, r.w / 2, C_TAB_HOVER);
        for (int k = -1; k <= 1; k++) ic_gfx_circle(c, cx, cy + k * 5 * S, 1.8f * S, C_TEXT);
    }
    {   /* how pages are drawn: on the GPU, or in software (the reason in
         * /tmp/surfer.log and Settings > About) */
        ic_rect_t a = tool_rect(1);
        ic_rect_t r = { a.x + a.w + 6 * S, a.y + 4 * S, BADGE_W - 10 * S, a.h - 8 * S };
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 7.0f * S, gpu_mode ? 0xFF1E5E3Au : C_FIELD);
        ic_text_draw_in(c, small, r, gpu_mode ? "GPU" : "Software", gpu_mode ? 0xFF7CE0A0u : C_TEXT2, IC_ALIGN_CENTER);
    }
    if (t && t->loading && t->progress > 0 && t->progress < 1) {
        ic_gfx_fill(c, 0, TOP - 2 * S, (int)(W * t->progress), 2 * S, C_ACCENT);
    } else {
        ic_gfx_hline(c, 0, TOP - 1, W, C_LINE);
    }
}

/* rows y0 .. y1 of the page area (0 is its top) from the active tab's picture */
static void draw_page(ic_canvas_t *c, int y0, int y1) {
    uint32_t *px = c->px;
    tab_t *t = ntabs ? tabs[active] : 0;
    int ch = content_h();
    if (y0 < 0) y0 = 0;
    if (y1 > ch - 1) y1 = ch - 1;
    if (y1 < y0) return;
    if (t && t->frame) {
        int cw = t->fw < W ? t->fw : W;
        for (int y = y0; y <= y1; y++) {
            uint32_t *row = px + (size_t)(TOP + y) * (size_t)W;
            int x = 0;
            if (y < t->fh) {
                memcpy(row, t->frame + (size_t)y * (size_t)t->fw, (size_t)cw * 4);
                x = cw;
            }
            for (; x < W; x++) row[x] = 0xFFFFFFFFu;
        }
    } else {
        ic_rect_t r = { 0, TOP, W, ch };
        ic_gfx_fill(c, 0, TOP + y0, W, y1 - y0 + 1, 0xFFFFFFFFu);
        if (y0 == 0 && y1 == ch - 1)
            ic_text_draw_in(c, ic_font(IC_FONT_BODY), r, t && t->loading ? "Loading..." : "", 0xFF8E8E93u, IC_ALIGN_CENTER);
    }
    if (page_dimmed()) ic_gfx_fill(c, 0, TOP + y0, W, y1 - y0 + 1, 0x70000000u);   /* under a sheet */
}

/* the status line (downloads, links) over the bottom left of the page */
static void draw_status(ic_canvas_t *c) {
    const ic_face_t *small = ic_font(IC_FONT_FOOTNOTE);
    status_drawn.w = 0;
    if (status[0] && status_until && now_ms() >= status_until) status[0] = 0;
    if (!status[0]) return;
    {
        int tw = ic_text_measure(small, status) + 20 * S, sh = 24 * S;
        ic_rect_t r = { 10 * S, H - sh, tw - 20 * S, sh };
        if (tw > W) tw = W;
        ic_gfx_rrect4(c, 0, H - sh, tw, sh, 0, 8.0f * S, 0, 0, 0xEE2B2B2Eu);
        ic_text_draw_in(c, small, r, status, C_TEXT, IC_ALIGN_LEFT);
        status_drawn.x = 0;
        status_drawn.y = H - sh;
        status_drawn.w = tw;
        status_drawn.h = sh;
    }
}

static int canvas(ic_canvas_t *c) {
    uint32_t *px = gui_pixel_buffer();
    if (!px) return 0;
    *c = ic_canvas_make(px, W, H);
    return 1;
}

/* the whole window */
static void compose(void) {
    ic_canvas_t c;
    if (!canvas(&c)) return;
    chrome_dirty = status_dirty = 0;
    draw_chrome(&c);
    draw_page(&c, 0, content_h() - 1);
    draw_status(&c);
    draw_overlays(&c, TOP, H);
    gui_flush();
}

/* the chrome alone: a hover, a title, the loading line */
static void update_chrome(void) {
    ic_canvas_t c;
    chrome_dirty = 0;
    if (!canvas(&c)) return;
    draw_chrome(&c);
    gui_flush_rect(0, 0, W, TOP);
}

/* rows y0 .. y1 of the page changed (with the status line over them) */
static void update_rows(int y0, int y1) {
    ic_canvas_t c;
    int top = TOP + y0, bottom = TOP + y1 + 1;
    if (!canvas(&c)) return;
    draw_page(&c, y0, y1);
    if (status_drawn.w && bottom > status_drawn.y && top < status_drawn.y + status_drawn.h) {
        draw_status(&c);
        if (status_drawn.y + status_drawn.h > bottom) bottom = status_drawn.y + status_drawn.h;
    }
    draw_overlays(&c, top, bottom);
    gui_flush_rect(0, top, W, bottom - top);
}

/* rows top .. bottom - 1 of the page area again, with what is over them
 * (a popup that changed, or went) */
static void update_area(int top, int bottom) {
    ic_canvas_t c;
    if (top < TOP) top = TOP;
    if (bottom > H) bottom = H;
    if (top >= bottom || !canvas(&c)) return;
    draw_page(&c, top - TOP, bottom - 1 - TOP);
    if (status_drawn.w && bottom > status_drawn.y && top < status_drawn.y + status_drawn.h) {
        draw_page(&c, status_drawn.y - TOP, H - TOP - 1);
        draw_status(&c);
        if (status_drawn.y < top) top = status_drawn.y;
        bottom = H;
    }
    draw_overlays(&c, top, bottom);
    gui_flush_rect(0, top, W, bottom - top);
}

/* the status line changed: the page where it was, and the new one */
static void update_status(void) {
    ic_canvas_t c;
    int top = status_drawn.w ? status_drawn.y : H, bottom = H;
    status_dirty = 0;
    if (!canvas(&c)) return;
    if (status_drawn.w) draw_page(&c, status_drawn.y - TOP, H - TOP - 1);
    draw_status(&c);
    if (status_drawn.w && status_drawn.y < top) {
        draw_page(&c, status_drawn.y - TOP, top - TOP - 1);      /* the new line is taller: under it too */
        draw_status(&c);
        top = status_drawn.y;
    }
    if (top < bottom) {
        draw_overlays(&c, top, bottom);
        gui_flush_rect(0, top, W, bottom - top);
    }
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

/* ---- frame pacing ------------------------------------------------------------------------
 *
 * WebKit draws a page's next frame only after it is told the last one was
 * shown (frame complete), and requestAnimationFrame and CSS animations run at
 * that pace.  Sent at once, as it used to be, a page with any animation at
 * all was drawn as fast as the processors allowed: Wikipedia, sitting still,
 * 200 times a second - two processor cores, ~13,000 system calls and ~20,000
 * page faults a second, each one through ICDA's kernel lock - though not a
 * pixel changed.
 *
 * So frame complete goes at the display's rate, 60 a second, while a key or
 * the pointer is in use and while the picture keeps changing (two frames in a
 * row that changed something: a video, a smooth animation, a scroll) and while
 * the page loads.
 * Otherwise it follows how often the picture really changes: twice as many
 * frames as changes came in the last second (from 15 to 60 a second).
 * Each frame is the whole page put together again in software, whatever
 * changed, and that costs up to 70 ms of processor time on a page with
 * blurred or layered parts (microsoft.com, cnn.com): a page whose banner ticks
 * ten times a second was drawn 62 times a second, 52 of them the same picture.
 * After half a second with no change it is 15 a second, after five seconds 4
 * (a page animating something invisible), and once a second for a tab that
 * is not shown.  A key, the pointer or a change seen at once brings 60 back.
 * Five seconds, not less: an animation that comes back every few seconds (a
 * slideshow, Wikipedia's banner every 3 s) would wait for a slow frame to
 * start, and its first quarter of a second would be skipped. */
#define FRAME_MS        16
#define LOAD_FRAME_MS   33
#define STILL_FRAME_MS  66
#define STILL_AFTER_MS  500
#define QUIET_FRAME_MS  250
#define QUIET_AFTER_MS  5000
#define HIDDEN_FRAME_MS 1000
#define INPUT_HOLD_MS   500
#define BUSY_FRAME_MS   50         /* the slowest pace a busy page is given: 20 frames a second */
static uint32_t last_input;        /* ms of the last key or pointer event */
static int busy_pace;              /* one or two processors (heavy pages, below) */

/* frames that changed something in the last second */
static unsigned changes_per_second(tab_t *t, uint32_t now) {
    unsigned n = 0, have = t->nchanges < 64 ? t->nchanges : 64;
    for (unsigned i = 0; i < have; i++) {
        uint32_t when = t->changes[(t->nchanges - 1 - i) % 64];
        if (now - when > 1000) break;
        n++;
    }
    return n;
}

static gboolean frame_complete(gpointer data) {
    tab_t *t = (tab_t *)data;
    t->ack_source = 0;
    t->last_ack = now_ms();
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(t->exp);
    return G_SOURCE_REMOVE;
}

static uint32_t frame_interval(tab_t *t) {
    uint32_t now = now_ms(), quiet, ms;
    unsigned fps;
    if (!ntabs || t != tabs[active]) return HIDDEN_FRAME_MS;
    /* full pace while a key or the pointer is in use and while the picture
     * keeps changing; 30 frames a second at least while the page loads
     * (pages that chain their loading through requestAnimationFrame would
     * load at the slow pace), not 60: each frame is the whole page put
     * together again on the processors, and a loading page's frames took
     * them from its parsing and scripts */
    if (now - last_input < INPUT_HOLD_MS || (t->run >= 2 && !t->loading)) {
        ms = FRAME_MS;
    } else if (t->loading) {
        ms = LOAD_FRAME_MS;
    } else if ((quiet = now - t->last_change) >= QUIET_AFTER_MS) {
        ms = QUIET_FRAME_MS;
    } else if (quiet >= STILL_AFTER_MS) {
        ms = STILL_FRAME_MS;
    } else {
        fps = changes_per_second(t, now) * 2;
        if (fps > 1000 / FRAME_MS) fps = 1000 / FRAME_MS;
        if (fps < 15) fps = 15;
        ms = 1000 / fps;
    }
    /* On one or two processors, and on any while a page loads, a page whose
     * frames take long to draw would be drawing all the time, and its
     * scripts (and everything else) wait behind the drawing: a frame's turn
     * is then at least one and a half times what drawing it takes, so a
     * third of the time is left over. */
    if ((busy_pace || t->loading) && t->draw_ms > FRAME_MS * 2 / 3) {
        uint32_t busy = t->draw_ms + t->draw_ms / 2;
        if (busy > BUSY_FRAME_MS) busy = BUSY_FRAME_MS;
        if (ms < busy) ms = busy;
    }
    return ms;
}

/* frame complete for t's last frame, as soon as its pace allows */
static void pace_frame(tab_t *t) {
    int32_t left;
    if (t->ack_source) g_source_remove(t->ack_source);
    t->ack_source = 0;
    left = (int32_t)(t->last_ack + frame_interval(t) - now_ms());
    if (left <= 0) frame_complete(t);
    else t->ack_source = g_timeout_add_full(G_PRIORITY_HIGH, (guint)left, frame_complete, t, NULL);
}

/* a key, the pointer, or the tab being shown: a frame complete held back
 * for a still or hidden page goes at the full rate */
static void hurry_frame(tab_t *t) {
    if (t && t->ack_source) pace_frame(t);
}

static void note_input(void) {
    last_input = now_ms();
    if (ntabs) hurry_frame(tabs[active]);
}

/* ---- frames in --------------------------------------------------------------------------- */

typedef uint32_t px4_t __attribute__((vector_size(16)));

/* copies n pixels, opaque, and says whether any differed from what was there:
 * one pass for the copy, the alpha and the change */
static int copy_row(uint32_t *restrict d, const uint32_t *restrict s, int n) {
    const px4_t opaque = { 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u };
    px4_t diff = { 0, 0, 0, 0 };
    uint32_t any;
    int x = 0;
    for (; x + 4 <= n; x += 4) {
        px4_t a, b;
        memcpy(&a, s + x, sizeof a);
        memcpy(&b, d + x, sizeof b);
        a |= opaque;
        diff |= a ^ b;
        memcpy(d + x, &a, sizeof a);
    }
    any = diff[0] | diff[1] | diff[2] | diff[3];
    for (; x < n; x++) {
        uint32_t v = s[x] | 0xFF000000u;
        any |= v ^ d[x];
        d[x] = v;
    }
    return any != 0;
}

/* the same from bytes R G B A (the GPU's read-back without BGRA) */
static int copy_row_rgba(uint32_t *restrict d, const uint32_t *restrict s, int n) {
    uint32_t any = 0;
    for (int x = 0; x < n; x++) {
        uint32_t v = s[x];
        v = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
        any |= v ^ d[x];
        d[x] = v;
    }
    return any != 0;
}

/* A frame from the web process into t->frame.  *y0 .. *y1 are the rows that
 * changed; returns 0 if none did (the page drew the same picture again). */
static int take_frame(tab_t *t, const uint8_t *src, int w, int h, size_t stride, int rgba, int *y0, int *y1) {
    int all = 0;
    *y0 = h;
    *y1 = -1;
    if (t->fw != w || t->fh != h || !t->frame) {
        free(t->frame);
        t->frame = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
        t->fw = t->frame ? w : 0;
        t->fh = t->frame ? h : 0;
        if (!t->frame) return 0;
        all = 1;
    }
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * stride);
        uint32_t *d = t->frame + (size_t)y * (size_t)w;
        if ((rgba ? copy_row_rgba(d, s, w) : copy_row(d, s, w)) || all) {
            if (*y0 > y) *y0 = y;
            *y1 = y;
        }
    }
    return *y1 >= 0;
}

/* after a frame: shown if it changed something, then paced.  WPE_DAMAGE
 * prints each frame's changed rows and the pace it got. */
static void frame_done(tab_t *t, int changed, int y0, int y1, unsigned t0) {
    static int damage_log = -1;
    if (damage_log < 0) damage_log = getenv("WPE_DAMAGE") != NULL;
    if (damage_log)
        fprintf(stderr, "icda-webkit damage: ms %u tab %s rows %d-%d next %u\n", now_ms(),
                ntabs && t == tabs[active] ? "shown" : "hidden", changed ? y0 : -1, changed ? y1 : -1, frame_interval(t));
    if (changed && ntabs && t == tabs[active]) update_rows(y0, y1);
    if (!timing_painted) {
        timing_painted = 1;
        timing("first picture", "");
    }
    frame_stats(t0);
    {   /* how long the page took to draw this frame, when it was drawing one
         * after another (a longer wait is a page with nothing to draw) */
        uint32_t took = t0 - t->last_ack;
        if (t->last_ack && took < 200) t->draw_ms = (t->draw_ms * 7 + took + 4) / 8;
        else t->draw_ms = 0;
    }
    if (changed) {
        uint32_t now = now_ms();
        t->last_change = now;
        t->changes[t->nchanges++ % 64] = now;
        t->run++;
    } else {
        t->run = 0;
    }
    pace_frame(t);
}

static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer) {
    tab_t *t = tab_of_exp(data);
    unsigned t0 = now_ms();
    struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    int changed = 0, y0 = 0, y1 = -1;
    /* a tab not shown keeps the picture it had (WebKit draws it again when it is shown) */
    if (shm && ntabs && (t == tabs[active] || !t->frame)) {
        int w = wl_shm_buffer_get_width(shm), h = wl_shm_buffer_get_height(shm), stride = wl_shm_buffer_get_stride(shm);
        wl_shm_buffer_begin_access(shm);
        changed = take_frame(t, (const uint8_t *)wl_shm_buffer_get_data(shm), w, h, (size_t)stride, 0, &y0, &y1);
        wl_shm_buffer_end_access(shm);
    }
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(t->exp, buffer);
    frame_done(t, changed, y0, y1, t0);
}

/* ---- pages drawn on the GPU ------------------------------------------------------------
 *
 * With ICDA's Intel GPU driver (/dev/dri/renderD128), the web processes
 * render with Mesa's Intel driver and hand over each frame as a GPU image
 * (WPEBackend-fdo's EGL mode); the GPU also turns it back into pixels here
 * (the image as a texture, read through a framebuffer) for the window.
 *
 * Anything going wrong falls back to drawing in software, as before:
 *   - before WebKit starts, a child process draws a test picture on the GPU;
 *     a crash, a hang or a wrong pixel means software for this session
 *   - a web process that crashes while the GPU draws: Surfer restarts
 *     itself in software for the session (/tmp is emptied at sign-in)
 *   - a GPU session that did not end cleanly (Surfer itself died): the
 *     next start uses software */

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#define GPU_OFF_FLAG "/tmp/icda-webkit-software"
#define GPU_RUN_FLAG "/tmp/icda-webkit-gpu"

static int gpu_bgra;
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static GLuint rb_fbo, rb_tex;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static uint32_t *rb_buf;            /* a frame read back from the GPU */
static size_t rb_cap;

static void touch(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

/* /tmp/surfer.log: how Surfer chose to draw and why, kept for the session
 * (Terminal: cat /tmp/surfer.log); also in SURFER_DISK_LOG,
 * which outlives a restart, with what Mesa said while the GPU was tried */
#define SURFER_DISK_LOG "/home/.cache/surfer.log"
#define MIMALLOC "/usr/lib/libmimalloc.so.2"    /* Alpine's mimalloc2, in the Linux root */
#define FONTS_CONF "/usr/lib/icda/fonts.conf"   /* userspace/webkit/fonts.conf */
static void slog(const char *fmt, ...) {
    static const char *const logs[] = { "/tmp/surfer.log", SURFER_DISK_LOG };
    char line[400];
    va_list ap;
    FILE *f;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "icda-webkit: %s\n", line);
    for (int i = 0; i < 2; i++)
        if ((f = fopen(logs[i], "a"))) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
}

/* why pages are drawn in software: the first reason counts */
static void reason(const char *why) {
    if (!gpu_reason[0]) snprintf(gpu_reason, sizeof gpu_reason, "%s", why);
}

/* /tmp/surfer-mode, for Settings > About: "GPU", or "Software: why" */
static void write_mode(void) {
    FILE *f = fopen("/tmp/surfer-mode", "w");
    if (!f) return;
    if (gpu_mode) fprintf(f, "GPU\n");
    else fprintf(f, "Software: %s\n", gpu_reason[0] ? gpu_reason : "no GPU driver");
    fclose(f);
}

static int gpu_start(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    static const EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx;
    const char *renderer, *exts;
    if (!get_display) return 0;
    egl_dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) return 0;
    ctx = eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 0;
    renderer = (const char *)glGetString(GL_RENDERER);
    slog("GL renderer: %s", renderer ? renderer : "(none)");
    if (!renderer || strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") || strstr(renderer, "Software")) return 0;
    exts = (const char *)glGetString(GL_EXTENSIONS);
    gpu_bgra = exts && strstr(exts, "GL_EXT_read_format_bgra") != NULL;
    image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!image_target) return 0;
    glGenTextures(1, &rb_tex);
    glGenFramebuffers(1, &rb_fbo);
    glBindTexture(GL_TEXTURE_2D, rb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return 1;
}

/* in a child: the GPU set up and a picture drawn and read back */
static int gpu_probe(void) {
    pid_t pid = fork();
    if (pid == 0) {
        unsigned char px[4] = { 0, 0, 0, 0 };
        GLuint tex, fbo;
        /* Mesa's and EGL's own messages into the disk log */
        if (freopen(SURFER_DISK_LOG, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        setenv("EGL_LOG_LEVEL", "debug", 1);
        setenv("MESA_DEBUG", "1", 1);
        if (!gpu_start()) _exit(1);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) _exit(2);
        glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        slog("GPU test pixel: %d %d %d %d (want 255 127 0 255)", px[0], px[1], px[2], px[3]);
        _exit(px[0] == 255 && px[1] >= 126 && px[1] <= 129 && px[2] == 0 && px[3] == 255 ? 0 : 3);
    }
    if (pid < 0) return 0;
    for (int i = 0; i < 200; i++) {                  /* 20 s: the first start compiles shaders */
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                slog("GPU test passed");
            } else {
                static const char *const why[] = { "", "Mesa could not start on the GPU (see the GL renderer line)",
                                                   "the GPU could not draw into a picture", "the GPU drew the test picture wrong" };
                int code = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
                slog("GPU test failed (%s %d)", WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? code : WTERMSIG(st));
                reason(code >= 1 && code <= 3 ? why[code] : "the GPU test crashed");
            }
            return WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    slog("GPU test did not finish in 20 s");
    reason("the GPU test hung");
    return 0;
}

static void gpu_clean_exit(void) {
    unlink(GPU_RUN_FLAG);
}

/* Surfer again, drawing in software, at url */
static void restart_in_software(const char *url, const char *why) {
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    char *args[3];
    FILE *off = fopen(GPU_OFF_FLAG, "w");          /* the rest of the session: software, and why */
    if (off) { fprintf(off, "%s\n", why); fclose(off); }
    slog("restarting with software drawing: %s", why);
    unlink(GPU_RUN_FLAG);
    if (n <= 0) exit(1);
    self[n] = 0;
    args[0] = self;
    args[1] = (char *)(url ? url : "");
    args[2] = NULL;
    execv(self, args);
    exit(1);
}

static void on_export_egl_image(void *data, struct wpe_fdo_egl_exported_image *image) {
    tab_t *t = tab_of_exp(data);
    unsigned t0 = now_ms();
    int w = (int)wpe_fdo_egl_exported_image_get_width(image), h = (int)wpe_fdo_egl_exported_image_get_height(image);
    EGLImageKHR img = wpe_fdo_egl_exported_image_get_egl_image(image);
    int changed = 0, y0 = 0, y1 = -1;
    if (w > 0 && h > 0 && img && ntabs && (t == tabs[active] || !t->frame)) {
        size_t n = (size_t)w * (size_t)h;
        if (n > rb_cap) {
            free(rb_buf);
            rb_buf = (uint32_t *)malloc(n * 4);
            rb_cap = rb_buf ? n : 0;
        }
        glBindTexture(GL_TEXTURE_2D, rb_tex);
        image_target(GL_TEXTURE_2D, (GLeglImageOES)img);
        glBindFramebuffer(GL_FRAMEBUFFER, rb_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rb_tex, 0);
        if (rb_buf && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadPixels(0, 0, w, h, gpu_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, rb_buf);
            changed = take_frame(t, (const uint8_t *)rb_buf, w, h, (size_t)w * 4, !gpu_bgra, &y0, &y1);
        }
    }
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(t->exp, image);
    frame_done(t, changed, y0, y1, t0);
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
    if (t->discarded && !(s && *s)) return;           /* ending its process clears it: the tab keeps its name */
    snprintf(t->title, sizeof t->title, "%s", s ? s : "");
    if (getenv("WPE_CONSOLE")) fprintf(stderr, "icda-webkit title: %s\n", t->title);
    chrome_dirty = 1;
}

static void on_uri(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    tab_t *t = tab_of_view(v);
    const char *s = webkit_web_view_get_uri(v);
    (void)ps; (void)d;
    if (!t) return;
    if (t->discarded && !(s && *s)) return;
    snprintf(t->uri, sizeof t->uri, "%s", s ? s : "");
    sync_addr();
    if (getenv("WPE_CONSOLE")) fprintf(stderr, "icda-webkit uri: %s\n", t->uri);
    chrome_dirty = 1;
}

static void on_progress(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    tab_t *t = tab_of_view(v);
    (void)ps; (void)d;
    if (!t) return;
    t->progress = webkit_web_view_get_estimated_load_progress(v);
    t->loading = webkit_web_view_is_loading(v);
    chrome_dirty = 1;
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
    tab_t *t = tab_of_view(v);
    const char *why = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory"
                    : reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : "was stopped";
    char msg[160];
    (void)d;
    if (t && t->discarded && reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) return;   /* memory, below: it reloads when shown */
    fprintf(stderr, "icda-webkit: web process %s (reason %d)\n", why, (int)reason);
    if (gpu_mode && reason == WEBKIT_WEB_PROCESS_CRASHED) {
        const char *uri = webkit_web_view_get_uri(v);
        restart_in_software(uri, "a page crashed while drawing on the GPU");
    }
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

/* ---- memory ------------------------------------------------------------------------------
 *
 * ICDA has no swap.  When memory runs out its kernel ends whichever program
 * asks for a page next: the page being read, or the desktop itself.  So:
 *   - each web process gives memory back as it grows (WebKit's memory
 *     pressure settings, scaled to the machine below: caches dropped from a
 *     sixth of the RAM, harder from a quarter) and a page that keeps
 *     growing past two fifths of the RAM is ended with a message instead
 *     (at most 1, 1.5 and 2.4 GB: WebKit's limit stops at 3 GB);
 *   - when the machine is short of memory, the tab shown longest ago gives
 *     its memory back: its web process is ended (the tab is discarded) and
 *     the page loads again when the tab is selected.  One tab every two
 *     seconds while memory stays short; tabs playing sound, and popups and
 *     their openers (they share a process), are kept. */
#define MEM_CHECK_MS 2000
static WebKitWebContext *web_context;
static unsigned ram_mb;            /* the machine's RAM */

/* MemTotal and MemAvailable (MemFree if it has none), in kB.  Opened anew
 * each time: older ICDA kernels render the file only when it is opened. */
static int meminfo(unsigned long long *total_kb, unsigned long long *avail_kb) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long long total = 0, avail = 0, freekb = 0, v;
    int have_avail = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %llu", &v) == 1) total = v;
        else if (sscanf(line, "MemAvailable: %llu", &v) == 1) { avail = v; have_avail = 1; }
        else if (sscanf(line, "MemFree: %llu", &v) == 1) freekb = v;
    }
    fclose(f);
    if (!total) return 0;
    *total_kb = total;
    *avail_kb = have_avail ? avail : freekb;
    return 1;
}

/* below this much available memory (kB), tabs are discarded */
static unsigned long long mem_low_kb(unsigned long long total_kb) {
    unsigned long long low = total_kb / 12;
    return low < 192 * 1024 ? 192 * 1024 : low;
}

static void memory_setup(void) {
    unsigned long long total_kb = 0, avail_kb = 0;
    unsigned limit_mb;
    WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
    ram_mb = meminfo(&total_kb, &avail_kb) ? (unsigned)(total_kb / 1024) : 4096;
    limit_mb = ram_mb / 2;
    if (limit_mb < 512) limit_mb = 512;
    if (limit_mb > 3072) limit_mb = 3072;
    webkit_memory_pressure_settings_set_memory_limit(mp, limit_mb);
    webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.33);
    webkit_memory_pressure_settings_set_strict_threshold(mp, 0.5);
    webkit_memory_pressure_settings_set_kill_threshold(mp, 0.8);
    webkit_memory_pressure_settings_set_poll_interval(mp, 10);
    /* The web browser cache model keeps the web process of every site left
     * behind (4 per GB of RAM, up to 30) for a quicker return, and WebKit
     * 2.40 never lets them go when memory runs short on Linux: six sites in
     * one tab left eight web processes and 2.1 GB.  The document browser
     * model keeps none, only the two pages back/forward goes to at once.
     * The cache model is one setting for the whole program and a context's
     * process cache is sized when the context is made, so it is set before
     * (through the default context, otherwise unused). */
    webkit_web_context_set_cache_model(webkit_web_context_get_default(), WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    web_context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, NULL));
    webkit_memory_pressure_settings_free(mp);
    slog("memory: %u MB of RAM; a page gives memory back from %u MB, harder from %u MB, ended at %u MB; "
         "tabs are discarded below %llu MB available; back/forward keeps pages %s",
         ram_mb, limit_mb / 3, limit_mb / 2, limit_mb * 4 / 5, mem_low_kb((unsigned long long)ram_mb * 1024) / 1024,
         ram_mb < 6144 ? "on disk only (under 6 GB)" : "in memory");
}

static void discard_tab(tab_t *t, unsigned long long avail_kb) {
    fprintf(stderr, "icda-webkit: discarded %s (%llu MB available)\n", t->uri[0] ? t->uri : "a tab", avail_kb / 1024);
    t->discarded = 1;
    t->loading = 0;
    if (t->ack_source) g_source_remove(t->ack_source);
    t->ack_source = 0;
    free(t->frame);
    t->frame = 0;
    t->fw = t->fh = 0;
    webkit_web_view_terminate_web_process(t->view);
    chrome_dirty = 1;
}

static gboolean memory_watch(gpointer d) {
    unsigned long long total, avail;
    tab_t *pick = 0;
    (void)d;
    if (!meminfo(&total, &avail) || avail >= mem_low_kb(total)) return G_SOURCE_CONTINUE;
    for (int i = 0; i < ntabs; i++) {
        tab_t *t = tabs[i];
        if (i == active || t->discarded || t->related || webkit_web_view_is_playing_audio(t->view)) continue;
        if (!pick || (int32_t)(t->last_shown - pick->last_shown) < 0) pick = t;
    }
    if (pick) discard_tab(pick, avail);
    return G_SOURCE_CONTINUE;
}

/* ---- heavy pages ---------------------------------------------------------------------------
 *
 * What a page costs here is mostly drawn and decoded on the processors:
 * without the GPU, Mesa composites every frame in software, and video is
 * always decoded in software.  Three things keep heavy sites usable:
 *
 *   - video the machine cannot play smoothly is not offered: a script at the
 *     start of every document answers the questions players ask
 *     (MediaSource.isTypeSupported, canPlayType, mediaCapabilities) with no
 *     for AV1, for more frames a second than the limit and for pictures
 *     larger than it, so YouTube and the like pick H.264 or VP9 at a size
 *     that plays (720p at 30 in software, 480p on one or two processors;
 *     1080p at 60 on the GPU);
 *   - requests to advertising and tracking networks from other sites' pages
 *     are not made (WebKit's content filter, compiled once into the cache):
 *     on news sites they are most of the scripts that keep running.
 *     SURFER_TRACKERS=allow turns it off;
 *   - on one or two processors a page's frames take at most about two thirds
 *     of the time (pace, in frame pacing), leaving the rest to its scripts. */
static WebKitUserContentManager *content;
static int ncpu = 1;

static const char media_script[] =
    "(function(){'use strict';"
    "var MAXH=%d,MAXW=%d,MAXFPS=%d,AV1=%d;"
    "function size(w,h){if(!w&&!h)return true;if(w&&h)return w*h<=MAXW*MAXH*1.05;return (h||0)<=MAXH&&(w||0)<=MAXW;}"
    "function ok(type){"
      "if(typeof type!=='string')return true;"
      "var t=type.toLowerCase(),m,w=0,h=0;"
      "if(!AV1&&/codecs\\s*=\\s*\"?[^\";]*av01/.test(t))return false;"
      "if((m=/framerate=(\\d+)/.exec(t))&&+m[1]>MAXFPS)return false;"
      "if((m=/width=(\\d+)/.exec(t)))w=+m[1];"
      "if((m=/height=(\\d+)/.exec(t)))h=+m[1];"
      "return size(w,h);}"
    "function wrap(o){"
      "if(!o||typeof o.isTypeSupported!=='function')return;"
      "var f=o.isTypeSupported.bind(o);"
      "o.isTypeSupported=function(t){return ok(t)&&f(t);};}"
    "try{wrap(window.MediaSource);wrap(window.ManagedMediaSource);wrap(window.WebKitMediaSource);}catch(e){}"
    "try{var p=HTMLMediaElement.prototype,c=p.canPlayType;"
      "p.canPlayType=function(t){return ok(t)?c.call(this,t):'';};}catch(e){}"
    "try{var mc=navigator.mediaCapabilities;if(mc&&mc.decodingInfo){var d=mc.decodingInfo.bind(mc);"
      "mc.decodingInfo=function(cfg){"
        "var v=cfg&&cfg.video;"
        "if(v&&(!ok(v.contentType)||(v.framerate&&parseFloat(v.framerate)>MAXFPS)||!size(v.width,v.height)))"
          "return Promise.resolve({supported:false,smooth:false,powerEfficient:false,configuration:cfg});"
        "return d(cfg);};}}catch(e){}"
    "})();";

/* advertising and tracking networks: hosts (and their subdomains) that other
 * sites' pages load scripts, frames and beacons from.  Nothing a page needs to
 * work: no logins, no video hosts, no tag managers. */
static const char *const tracker_hosts[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "google-analytics.com", "adservice.google.com",
    "amazon-adsystem.com", "adnxs.com", "adsrvr.org", "criteo.com", "criteo.net", "rubiconproject.com", "pubmatic.com",
    "openx.net", "casalemedia.com", "indexww.com", "taboola.com", "outbrain.com", "moatads.com", "scorecardresearch.com",
    "quantserve.com", "quantcast.com", "chartbeat.com", "chartbeat.net", "bluekai.com", "krxd.net", "demdex.net",
    "everesttech.net", "omtrdc.net", "2o7.net", "adform.net", "smartadserver.com", "teads.tv", "sharethrough.com",
    "triplelift.com", "3lift.com", "yieldmo.com", "media.net", "bidswitch.net", "contextweb.com", "lijit.com",
    "sovrn.com", "gumgum.com", "advertising.com", "adsafeprotected.com", "doubleverify.com", "mathtag.com",
    "rlcdn.com", "agkn.com", "exelator.com", "crwdcntrl.net", "tapad.com", "zemanta.com", "revcontent.com", "mgid.com",
    "adroll.com", "hotjar.com", "mouseflow.com", "fullstory.com", "newrelic.com", "nr-data.net", "permutive.com",
    "permutive.app", "parsely.com", "optimizely.com", "segment.io", "mixpanel.com", "amplitude.com", "bounceexchange.com",
    "sail-horizon.com", "onetag-sys.com", "33across.com", "imrworldwide.com", "liadm.com", "id5-sync.com", "pippio.com",
    "adtechus.com", "serving-sys.com", "flashtalking.com", "innovid.com", "springserve.com", "spotxchange.com",
    "freewheel.tv", "fwmrm.net", "admanmedia.com", "aniview.com", "connatix.com", "primis.tech", "vidazoo.com",
};
/* YouTube's own pages are left alone: when its requests to the ad networks
 * fail it stops playing videos and asks for the blocker to be turned off.
 * A new list or rule needs a new name, or the one compiled before is used. */
#define TRACKER_FILTER "icda-trackers-3"

static void filter_ready(WebKitUserContentFilter *f, GError *err, const char *how) {
    if (f) {
        webkit_user_content_manager_add_filter(content, f);
        webkit_user_content_filter_unref(f);
        slog("trackers: requests to %d advertising and tracking networks are not made (%s)",
             (int)G_N_ELEMENTS(tracker_hosts), how);
    } else {
        slog("trackers: the filter could not be %s: %s", how, err ? err->message : "?");
    }
    if (err) g_error_free(err);
}

static void filter_saved(GObject *o, GAsyncResult *res, gpointer d) {
    GError *err = NULL;
    (void)d;
    filter_ready(webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(o), res, &err), err, "compiled");
}

static void filter_loaded(GObject *o, GAsyncResult *res, gpointer d) {
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(o);
    WebKitUserContentFilter *f = webkit_user_content_filter_store_load_finish(store, res, NULL);
    (void)d;
    if (f) {
        filter_ready(f, NULL, "from the cache");
        return;
    }
    {   /* the first start (or a new list): one rule a host, WebKit compiles them */
        GString *json = g_string_new("[");
        GBytes *bytes;
        for (size_t i = 0; i < G_N_ELEMENTS(tracker_hosts); i++) {
            g_string_append(json, i ? ",{\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?" : "{\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?");
            for (const char *p = tracker_hosts[i]; *p; p++) {
                if (*p == '.') g_string_append(json, "\\\\.");
                else g_string_append_c(json, *p);
            }
            g_string_append(json, "[:/]\",\"load-type\":[\"third-party\"],\"unless-domain\":[\"*youtube.com\"]},"
                                  "\"action\":{\"type\":\"block\"}}");
        }
        g_string_append_c(json, ']');
        bytes = g_string_free_to_bytes(json);
        webkit_user_content_filter_store_save(store, TRACKER_FILTER, bytes, NULL, filter_saved, NULL);
        g_bytes_unref(bytes);
    }
}

static void heavy_setup(void) {
    int max_h, max_w, max_fps, av1;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    char *js;
    WebKitUserScript *script;
    ncpu = n > 0 ? (int)n : 1;
    busy_pace = ncpu <= 2 && !getenv("SURFER_FULL_RATE");
    /* no AV1 even on the GPU: the decoding is done on the processors all the
     * same, by libaom, on one of them (GST_PLUGIN_FEATURE_RANK in main) */
    av1 = 0;
    if (gpu_mode && ncpu > 2) { max_w = 1920; max_h = 1080; max_fps = 60; }
    else if (ncpu > 2) { max_w = 1280; max_h = 720; max_fps = 30; }
    else { max_w = 854; max_h = 480; max_fps = 30; }
    content = webkit_user_content_manager_new();
    if (!getenv("SURFER_MEDIA_ANY")) {
        js = g_strdup_printf(media_script, max_h, max_w, max_fps, av1);
        script = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(content, script);
        webkit_user_script_unref(script);
        g_free(js);
        slog("video: up to %dx%d at %d frames a second%s (%d processor%s, drawing on the %s)", max_w, max_h, max_fps,
             av1 ? "" : ", no AV1", ncpu, ncpu == 1 ? "" : "s", gpu_mode ? "GPU" : "CPU");
    }
    {
        const char *tr = getenv("SURFER_TRACKERS");
        if (tr && !strcmp(tr, "allow")) {
            slog("trackers: allowed (SURFER_TRACKERS=allow)");
        } else {
            char *dir = g_build_filename(g_get_user_cache_dir(), "surfer-filters", NULL);
            WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(dir);
            webkit_user_content_filter_store_load(store, TRACKER_FILTER, NULL, filter_loaded, NULL);
            g_free(dir);
        }
    }
}

static void popup_close(void);
static void find_close(void);
static tab_t *fs_tab;              /* the tab whose page is in full screen */
static void set_fullscreen(int on);
static bool on_fullscreen(void *data, bool enable);
static void on_audio(WebKitWebView *v, GParamSpec *ps, gpointer d);
static gboolean on_option_menu(WebKitWebView *v, WebKitOptionMenu *m, WebKitRectangle *r, gpointer d);
static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *cm, WebKitHitTestResult *hit, gpointer d);
static gboolean on_script_dialog(WebKitWebView *v, WebKitScriptDialog *dlg, gpointer d);
static gboolean on_file_chooser(WebKitWebView *v, WebKitFileChooserRequest *req, gpointer d);
static gboolean on_policy(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d);
static void on_find_count(WebKitFindController *fc, guint n, gpointer d);
static void on_find_found(WebKitFindController *fc, guint n, gpointer d);
static void on_find_failed(WebKitFindController *fc, gpointer d);

/* the last tabs closed, for Ctrl+Shift+T */
#define CLOSED_MAX 10
static char closed_uri[CLOSED_MAX][2048];
static int nclosed;

static void select_tab(int i) {
    tab_t *t;
    if (i < 0 || i >= ntabs) return;
    if (ntabs && active < ntabs && active != i) {
        popup_close();
        find_close();
        if (fs_tab) wpe_view_backend_dispatch_request_exit_fullscreen(fs_tab->wb);
        tabs[active]->last_shown = now_ms();
        wpe_view_backend_remove_activity_state(tabs[active]->wb, wpe_view_activity_state_visible | wpe_view_activity_state_focused);
    }
    active = i;
    t = tabs[i];
    t->last_shown = now_ms();
    t->last_change = now_ms();         /* shown again: at full pace until it settles */
    wpe_view_backend_add_activity_state(t->wb, wpe_view_activity_state_visible | wpe_view_activity_state_focused |
                                                wpe_view_activity_state_in_window);
    if (t->discarded) {                     /* back from memory, below: load the page again */
        t->discarded = 0;
        webkit_web_view_reload(t->view);
    }
    hurry_frame(t);
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
    if (gpu_mode) {
        t->egl_client.export_fdo_egl_image = on_export_egl_image;
        t->exp = wpe_view_backend_exportable_fdo_egl_create(&t->egl_client, t, (uint32_t)(W / S), (uint32_t)(content_h() / S));
    } else {
        t->client.export_shm_buffer = on_export_shm_buffer;
        t->exp = wpe_view_backend_exportable_fdo_create(&t->client, t, (uint32_t)(W / S), (uint32_t)(content_h() / S));
    }
    t->wb = wpe_view_backend_exportable_fdo_get_view_backend(t->exp);
    backend = webkit_web_view_backend_new(t->wb, NULL, NULL);
    if (related) {
        tab_t *opener = tab_of_view(related);
        t->related = 1;
        if (opener) opener->related = 1;
        /* the opener's web process and settings; the scripts and filters
         * (heavy pages, below) are not shared unless given */
        t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "related-view", related,
                                               "user-content-manager", content, "website-policies", policies, NULL));
    } else {
        t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "settings", settings,
                                               "web-context", web_context, "network-session", session,
                                               "user-content-manager", content, "website-policies", policies, NULL));
    }
    wpe_view_backend_set_fullscreen_handler(t->wb, on_fullscreen, t);
    /* WebGL drawn by the processors takes them all and still crawls: without
     * the GPU, pages are told there is none and use what they have for that
     * (maps their flat tiles, sites a still picture).  Set here because
     * making a view turns it on again in the settings it is given */
    if (!gpu_mode) webkit_settings_set_enable_webgl(webkit_web_view_get_settings(t->view), FALSE);
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
    g_signal_connect(t->view, "notify::is-playing-audio", G_CALLBACK(on_audio), NULL);
    g_signal_connect(t->view, "notify::is-muted", G_CALLBACK(on_audio), NULL);
    g_signal_connect(t->view, "show-option-menu", G_CALLBACK(on_option_menu), NULL);
    g_signal_connect(t->view, "context-menu", G_CALLBACK(on_context_menu), NULL);
    g_signal_connect(t->view, "script-dialog", G_CALLBACK(on_script_dialog), NULL);
    g_signal_connect(t->view, "run-file-chooser", G_CALLBACK(on_file_chooser), NULL);
    g_signal_connect(t->view, "decide-policy", G_CALLBACK(on_policy), NULL);
    {
        WebKitFindController *fc = webkit_web_view_get_find_controller(t->view);
        g_signal_connect(fc, "counted-matches", G_CALLBACK(on_find_count), NULL);
        g_signal_connect(fc, "found-text", G_CALLBACK(on_find_found), NULL);
        g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_find_failed), NULL);
    }
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
    popup_close();
    if (i == active) find_close();
    if (t == fs_tab) {
        fs_tab = 0;
        set_fullscreen(0);
    }
    if (t->uri[0] && strncmp(t->uri, "about:", 6)) {
        if (nclosed == CLOSED_MAX) memmove(closed_uri[0], closed_uri[1], sizeof closed_uri[0] * (CLOSED_MAX - 1));
        else nclosed++;
        snprintf(closed_uri[nclosed - 1], sizeof closed_uri[0], "%s", t->uri);
    }
    memmove(&tabs[i], &tabs[i + 1], sizeof(tab_t *) * (size_t)(ntabs - i - 1));
    ntabs--;
    if (t->ack_source) g_source_remove(t->ack_source);
    t->ack_source = 0;
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

/* ---- ICDA's clipboard and one-line text fields ------------------------------------- */

static int clip_write(const char *s) {
    FILE *f = fopen(CLIPBOARD, "wb");
    size_t n = strlen(s);
    int ok = f && fwrite(s, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    return ok;
}

/* the clipboard's text (g_free it), or 0 when it is empty or unreadable */
static char *clip_read(void) {
    FILE *f = fopen(CLIPBOARD, "rb");
    char *buf;
    size_t got;
    if (!f) return 0;
    buf = (char *)g_malloc(1 << 20);
    got = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[got] = 0;
    if (!got) {
        g_free(buf);
        return 0;
    }
    return buf;
}

/* A key in a one-line text field (the address, a file name, a prompt, the
 * find bar): editing, Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V.  *all: everything is
 * selected (typing replaces it).  Returns 1 if it took the key. */
static int field_key(char *buf, size_t cap, int *all, uint32_t keysym, uint32_t mods) {
    size_t n = strlen(buf);
    if (mods & M_CTRL) {
        if (keysym == 'a') {
            *all = 1;
        } else if (keysym == 'c' || keysym == 'x') {
            if (!clip_write(buf)) {
                set_status("Could not copy", 3000);
                return 1;
            }
            if (keysym == 'x') { buf[0] = 0; *all = 0; }
        } else if (keysym == 'v') {
            char *text = clip_read();
            size_t used = 0, at = *all ? 0 : n;
            if (!text) {
                set_status("The clipboard is empty", 3000);
                return 1;
            }
            for (size_t i = 0; text[i]; i++)
                if ((unsigned char)text[i] >= 32 && text[i] != 127) text[used++] = text[i];
            if (used >= cap - at) {
                set_status("What is on the clipboard is too long", 3000);
            } else if (used) {
                memcpy(buf + at, text, used);
                buf[at + used] = 0;
                *all = 0;
            }
            g_free(text);
        } else {
            return 0;
        }
        return 1;
    }
    if (keysym == 0xff08) {
        if (*all) buf[0] = 0;
        else if (n) {
            while (n > 0 && (buf[n - 1] & 0xC0) == 0x80) n--;      /* a whole UTF-8 character */
            buf[n ? n - 1 : 0] = 0;
        }
        *all = 0;
        return 1;
    }
    if (keysym >= 0x20 && keysym < 0xff00) {
        uint32_t cp = keysym >= 0x01000000 ? keysym - 0x01000000 : keysym;
        char u[5];
        int k = 0;
        if (*all) { buf[0] = 0; n = 0; *all = 0; }
        if (cp < 0x80) u[k++] = (char)cp;
        else if (cp < 0x800) { u[k++] = (char)(0xC0 | cp >> 6); u[k++] = (char)(0x80 | (cp & 63)); }
        else { u[k++] = (char)(0xE0 | cp >> 12); u[k++] = (char)(0x80 | ((cp >> 6) & 63)); u[k++] = (char)(0x80 | (cp & 63)); }
        if (n + (size_t)k < cap - 1) {
            memcpy(buf + n, u, (size_t)k);
            buf[n + (size_t)k] = 0;
        }
        return 1;
    }
    return 0;
}

/* draws a text field in r: its end in view, the caret, the selection */
static void draw_field(ic_canvas_t *c, ic_rect_t r, const char *text, int focus, int all, const char *placeholder) {
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 8.0f * S, focus ? C_FIELD_ON : C_FIELD);
    if (focus) ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, 8.0f * S, 1.5f * S, C_ACCENT);
    r.x += 10 * S;
    r.w -= 20 * S;
    if (!text[0]) {
        if (placeholder) ic_text_draw_in(c, body, r, placeholder, C_TEXT3, IC_ALIGN_LEFT);
        if (focus) ic_gfx_vline(c, r.x, r.y + 7 * S, r.h - 14 * S, C_ACCENT);
        return;
    }
    {
        int len = (int)strlen(text), start = 0, n, tw;
        char cut[1024];
        while (start < len && ic_text_measure(body, text + start) > r.w - 4 * S) start++;
        n = ic_text_fit(body, text + start, r.w);
        snprintf(cut, sizeof cut, "%.*s", n, text + start);
        tw = ic_text_measure(body, cut);
        if (focus && all) ic_gfx_rrect(c, r.x - 2 * S, r.y + 6 * S, tw + 4 * S, r.h - 12 * S, 3.0f * S, 0xFF1F4E8Cu);
        ic_text_draw_in(c, body, r, cut, C_TEXT, IC_ALIGN_LEFT);
        if (focus && !all) ic_gfx_vline(c, r.x + tw + 1, r.y + 7 * S, r.h - 14 * S, C_ACCENT);
    }
}

/* the selected text of the page (or of the text field in it) to ICDA's clipboard */
static void copied(GObject *o, GAsyncResult *res, gpointer d) {
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), res, NULL);
    (void)d;
    if (!v) return;
    if (jsc_value_is_string(v)) {
        char *s = jsc_value_to_string(v);
        if (s && *s && !clip_write(s)) set_status("Could not copy", 3000);
        g_free(s);
    }
    g_object_unref(v);
}

static void copy_selection(tab_t *t) {
    static const char js[] =
        "(function(){var a=document.activeElement;"
        "try{if(a&&typeof a.selectionStart=='number'&&a.selectionEnd>a.selectionStart)"
        "return a.value.substring(a.selectionStart,a.selectionEnd);}catch(e){}"
        "return String(getSelection());})()";
    webkit_web_view_evaluate_javascript(t->view, js, -1, NULL, NULL, NULL, copied, NULL);
}

/* ICDA's clipboard into the page, where the caret is: 1 if there was text */
static int paste_into(tab_t *t) {
    char *text = clip_read();
    if (!text) return 0;
    webkit_web_view_execute_editing_command_with_argument(t->view, "InsertText", text);
    g_free(text);
    return 1;
}

static int popup_key(uint32_t keysym, uint32_t mods);
static int find_key(uint32_t keysym, uint32_t mods);
static void find_open(void);
static void downloads_toggle(void);
static void reopen_tab(void);

/* one key: a popup, the find bar, browser shortcuts, the address bar, or the page */
static void key(uint32_t keysym, uint32_t mods) {
    tab_t *t = ntabs ? tabs[active] : 0;
    if (popup_key(keysym, mods)) return;
    if (mods & M_CTRL) {
        switch (keysym) {
        case 'l': focus_addr(); return;
        case 't':
            if (mods & M_SHIFT) reopen_tab();
            else { add_tab(NULL, NULL); focus_addr(); }
            return;
        case 'w': close_tab(active); return;
        case 'r': if (t) webkit_web_view_reload(t->view); return;
        case 'f': find_open(); return;
        case 'j': downloads_toggle(); return;
        case 's': if (t && t->uri[0]) webkit_web_view_download_uri(t->view, t->uri); return;   /* save the page */
        case 0xff09: case 0xff56: select_tab((active + 1) % ntabs); return;          /* Ctrl+Tab, Ctrl+PgDn */
        case 0xff55: select_tab((active + ntabs - 1) % ntabs); return;               /* Ctrl+PgUp */
        }
    }
    if ((mods & M_ALT) && t) {
        if (keysym == 0xff51) { webkit_web_view_go_back(t->view); return; }
        if (keysym == 0xff53) { webkit_web_view_go_forward(t->view); return; }
    }
    if (keysym == 0xffc2 && t) { webkit_web_view_reload(t->view); return; }      /* F5 */
    if (keysym == 0xff1b && fs_tab) {                                             /* Escape leaves full screen */
        wpe_view_backend_dispatch_request_exit_fullscreen(fs_tab->wb);
        return;
    }
    if (keysym == 0xff1b && fullscreen) { set_fullscreen(0); return; }
    if (find_key(keysym, mods)) return;
    if (addr_focus) {
        if (keysym == 0xff0d) { go(addr); return; }
        if (keysym == 0xff1b) { addr_focus = 0; sync_addr(); compose(); return; }
        if (field_key(addr, sizeof addr, &addr_all, keysym, mods)) compose();
        return;
    }
    if (t && (mods & M_CTRL) && !(mods & (M_ALT | M_SHIFT))) {
        /* the page's copy and paste, with ICDA's clipboard */
        if (keysym == 'c' || keysym == 'x') copy_selection(t);
        if (keysym == 'v' && paste_into(t)) return;
    }
    web_key(keysym, mods);
}

/* ICDA sends keys as a terminal-style byte stream (ic_app.c decodes the same) */
static struct { int state, p1, p2, p3; } kd;

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
        if (c == '[') { kd.state = 2; kd.p1 = kd.p2 = kd.p3 = 0; return; }
        kd.state = 0;
        if (c >= 32 && c < 127) { plain_key(c, M_ALT); return; }
        key(0xff1b, 0);
        feed_key(c);
        return;
    }
    if (c >= '0' && c <= '9') {
        if (kd.state == 4) kd.p3 = kd.p3 * 10 + (int)(c - '0');
        else if (kd.state == 3) kd.p2 = kd.p2 * 10 + (int)(c - '0');
        else kd.p1 = kd.p1 * 10 + (int)(c - '0');
        return;
    }
    if (c == ';') { kd.state = kd.state == 3 ? 4 : 3; return; }
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
            else if (kd.p1 == 27 && kd.p3 > 0 && kd.p3 < 0x110000) {   /* ESC [ 27 ; mods ; code ~ (Ctrl+Shift+letter) */
                uint32_t k = (uint32_t)kd.p3;
                key(k >= 'A' && k <= 'Z' ? k + 32 : k, mods);
            }
            return;
        }
    }
}

/* the pointer over the page (window coordinates; y below the chrome).
 * page_buttons are the buttons the page saw go down. */
static void web_pointer(int x, int y, uint32_t buttons, int wheel) {
    struct wpe_input_pointer_event ev;
    static int last_x = -1, last_y = -1;
    struct wpe_view_backend *wb = tabs[active]->wb;
    int lx = x / S, ly = (y - TOP) / S;
    memset(&ev, 0, sizeof ev);
    ev.time = now_ms();
    ev.x = lx;
    ev.y = ly;
    if (lx != last_x || ly != last_y) {
        ev.type = wpe_input_pointer_event_type_motion;
        ev.modifiers = (page_buttons & GUI_BTN_LEFT) ? wpe_input_pointer_modifier_button1 : 0;
        wpe_view_backend_dispatch_pointer_event(wb, &ev);
        last_x = lx;
        last_y = ly;
    }
    for (int i = 0; i < 3; i++) {
        static const uint32_t bit[3] = { GUI_BTN_LEFT, GUI_BTN_RIGHT, GUI_BTN_MIDDLE };
        /* libwpe numbers them as evdev does (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
         * from 1): right 2, middle 3.  Sent the other way round, WebKit took
         * a right click for a middle one and never asked for a menu. */
        static const uint32_t wb_button[3] = { 1, 2, 3 };
        if ((buttons & bit[i]) == (page_buttons & bit[i])) continue;
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = wb_button[i];
        ev.state = (buttons & bit[i]) ? 1 : 0;
        wpe_view_backend_dispatch_pointer_event(wb, &ev);
    }
    page_buttons = buttons;
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

static int popup_mouse(int x, int y, uint32_t pressed, int wheel, int moved);
static int find_mouse(int x, int y, uint32_t pressed);
static void main_menu(void);

static void mouse(int x, int y, uint32_t buttons, int wheel) {
    uint32_t pressed = buttons & ~buttons_down, released = buttons_down & ~buttons;
    int moved = x != mouse_x || y != mouse_y;
    mouse_x = x;
    mouse_y = y;
    buttons_down = buttons;
    if (popup_mouse(x, y, pressed, wheel, moved) || find_mouse(x, y, pressed)) {
        /* the page still hears of the buttons it saw go down coming up */
        if (ntabs && (page_buttons & released)) web_pointer(x, y < TOP ? TOP : y, page_buttons & ~released, 0);
        return;
    }
    if (ntabs && (y >= TOP || page_buttons)) {               /* the page, or a drag that started in it */
        if ((pressed & GUI_BTN_LEFT) && addr_focus) {
            addr_focus = 0;
            sync_addr();
            compose();
        }
        web_pointer(x, y < TOP ? TOP : y, buttons, y >= TOP ? wheel : 0);
        if (moved && y < TOP) chrome_dirty = 1;
        return;
    }
    if (pressed & GUI_BTN_MIDDLE) {                          /* a middle click closes a tab */
        for (int i = 0; i < ntabs; i++)
            if (in_rect(tab_rect(i), x, y)) { close_tab(i); return; }
    }
    if (pressed & GUI_BTN_LEFT) {
        for (int i = 0; i < ntabs; i++) {
            ic_rect_t r = tab_rect(i);
            if (!in_rect(r, x, y)) continue;
            if (tab_has_audio(i) && in_rect(tab_audio_rect(i), x, y)) {
                webkit_web_view_set_is_muted(tabs[i]->view, !webkit_web_view_get_is_muted(tabs[i]->view));
                chrome_dirty = 1;
            } else if (i == active && x >= r.x + r.w - 24 * S) {
                close_tab(i);
            } else {
                select_tab(i);
            }
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
        if (in_rect(tool_rect(0), x, y)) { downloads_toggle(); return; }
        if (in_rect(tool_rect(1), x, y)) { main_menu(); return; }
    } else if (moved) {
        chrome_dirty = 1;                                    /* hover highlights */
    }
}

static void layout(void) {
    W = gui_window_width();
    H = gui_window_height();
    S = gui_window_scale() > 0 ? gui_window_scale() : 1;
    TABS_H = 36 * S;
    BAR_H = 44 * S;
    TOP = fullscreen ? 0 : TABS_H + BAR_H;
}


/* ---- popups ----------------------------------------------------------------------------
 *
 * One at a time over the page: a menu (a <select>'s options, the right-click
 * menu, Surfer's menu), the downloads list, or a sheet (saving a download,
 * choosing a file for a page, a page's alert / confirm / prompt).  A sheet
 * dims the page and takes all input until it is answered; a menu or the
 * downloads list closes on a click outside it.  The find bar is separate and
 * stays while the page is used.  All are drawn after the page, clipped to the
 * rows being drawn, so a page frame under them redraws only its rows. */

enum { POP_NONE, POP_MENU, POP_DOWNLOADS, POP_SHEET };
static int popup;

static int page_dimmed(void) { return popup == POP_SHEET; }

static void human_size(guint64 b, char *out, size_t cap) {
    if (b >= 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(out, cap, "%.0f KB", b / 1024.0);
    else snprintf(out, cap, "%u bytes", (unsigned)b);
}

static void draw_x(ic_canvas_t *c, int cx, int cy, int r, ic_color_t col) {
    ic_gfx_line(c, cx - r, cy - r, cx + r, cy + r, 1.5f * S, col);
    ic_gfx_line(c, cx + r, cy - r, cx - r, cy + r, 1.5f * S, col);
}

static void draw_button(ic_canvas_t *c, ic_rect_t r, const char *label, int primary, int hover) {
    ic_color_t bg = primary ? (hover ? 0xFF3394FFu : C_ACCENT) : (hover ? 0xFF4A4A50u : 0xFF3C3C41u);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 8.0f * S, bg);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), r, label, C_TEXT, IC_ALIGN_CENTER);
}

static void draw_chevron_up(ic_canvas_t *c, int cx, int cy) {
    ic_gfx_line(c, cx - 5 * S, cy + 3 * S, cx, cy - 2 * S, 1.6f * S, C_TEXT);
    ic_gfx_line(c, cx, cy - 2 * S, cx + 5 * S, cy + 3 * S, 1.6f * S, C_TEXT);
}

static void draw_chevron_down(ic_canvas_t *c, int cx, int cy) {
    ic_gfx_line(c, cx - 5 * S, cy - 2 * S, cx, cy + 3 * S, 1.6f * S, C_TEXT);
    ic_gfx_line(c, cx, cy + 3 * S, cx + 5 * S, cy - 2 * S, 1.6f * S, C_TEXT);
}

/* a circular arrow (try again) on background bg */
static void draw_reload_icon(ic_canvas_t *c, int cx, int cy, ic_color_t col, ic_color_t bg) {
    float r = 6.0f * S;
    ic_gfx_ring(c, cx, cy, r, 1.6f * S, col);
    ic_gfx_fill(c, cx, cy - (int)r - 2 * S, (int)r + 2 * S, (int)r + 1, bg);
    ic_gfx_line(c, cx, cy - r, cx + 4 * S, cy - r - 3 * S, 1.6f * S, col);
    ic_gfx_line(c, cx, cy - r, cx + 4 * S, cy - r + 3 * S, 1.6f * S, col);
}

static void popup_panel(ic_canvas_t *c, ic_rect_t r) {
    ic_gfx_shadow(c, r.x, r.y, r.w, r.h, 10.0f * S, 10 * S, 3 * S, 0x70);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 10.0f * S, C_POPUP);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, 10.0f * S, 1.0f * S, C_LINE);
}

/* ---- menus ---------------------------------------------------------------------------- */

#define MENU_MAX 600
enum { MENU_OPTIONS = 1, MENU_CONTEXT, MENU_MAIN };
enum { A_SEP = 1, A_GROUP, A_OPTION, A_BACK, A_FORWARD, A_RELOAD, A_OPEN_LINK_TAB, A_COPY_LINK, A_SAVE_LINK,
       A_OPEN_IMAGE_TAB, A_COPY_IMAGE, A_SAVE_IMAGE, A_MEDIA_PLAY, A_MEDIA_MUTE, A_MEDIA_LOOP, A_MEDIA_CONTROLS,
       A_SAVE_MEDIA, A_CUT, A_COPY, A_PASTE, A_SELECT_ALL, A_SAVE_PAGE, A_FIND, A_NEW_TAB, A_REOPEN_TAB,
       A_DOWNLOADS, A_ZOOM_IN, A_ZOOM_OUT, A_ZOOM_RESET, A_FULLSCREEN, A_MUTE_TAB };

typedef struct {
    char label[96];
    int  action, enabled, checked, opt;
} menu_item_t;

static struct {
    int kind, n, hover, scroll, x, y, w;
    menu_item_t item[MENU_MAX];
    WebKitOptionMenu *options;      /* a <select>'s list (MENU_OPTIONS), referenced */
    tab_t *tab;
    char link[2048], image[2048], media[2048];
    int px, py;                     /* where in the page it was opened (CSS pixels) */
} menu;

static void menu_add(const char *label, int action, int enabled) {
    menu_item_t *it;
    if (menu.n >= MENU_MAX) return;
    if (action == A_SEP && (!menu.n || menu.item[menu.n - 1].action == A_SEP)) return;
    it = &menu.item[menu.n++];
    snprintf(it->label, sizeof it->label, "%s", label);
    it->action = action;
    it->enabled = enabled;
    it->checked = 0;
    it->opt = -1;
}

static int menu_row_h(int i) { return menu.item[i].action == A_SEP ? 9 * S : 30 * S; }

static int menu_content_h(void) {
    int h = 8 * S;
    for (int i = 0; i < menu.n; i++) h += menu_row_h(i);
    return h;
}

static ic_rect_t menu_rect(void) {
    ic_rect_t r = { menu.x, menu.y, menu.w, menu_content_h() };
    if (r.y + r.h > H - 6 * S) r.h = H - 6 * S - r.y;
    return r;
}

/* places the menu by the anchor (window coordinates): below it, or above, or
 * as tall as the page allows */
static void menu_show(int ax, int ay, int aw, int ah, int min_w) {
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    int w = min_w, h, room_below, room_above;
    while (menu.n && menu.item[menu.n - 1].action == A_SEP) menu.n--;
    if (!menu.n) return;
    for (int i = 0; i < menu.n; i++) {
        int tw = ic_text_measure(body, menu.item[i].label) + 56 * S;
        if (tw > w) w = tw;
    }
    if (w > W - 16 * S) w = W - 16 * S;
    if (menu.kind != MENU_OPTIONS && w > 360 * S) w = 360 * S;
    h = menu_content_h();
    room_below = H - 6 * S - (ay + ah);
    room_above = ay - (TOP + 6 * S);
    if (h <= room_below || room_below >= room_above) menu.y = ay + ah;
    else menu.y = h <= room_above ? ay - h : TOP + 6 * S;
    if (menu.y < TOP + 6 * S) menu.y = TOP + 6 * S;
    menu.x = ax;
    if (menu.x + w > W - 8 * S) menu.x = W - 8 * S - w;
    if (menu.x < 8 * S) menu.x = 8 * S;
    menu.w = w;
    menu.scroll = 0;
    if (menu.hover >= 0) {                       /* the selected option in view */
        int y = 4 * S;
        for (int i = 0; i < menu.hover; i++) y += menu_row_h(i);
        if (y + 30 * S > menu_rect().h) menu.scroll = y - menu_rect().h / 2;
    }
    popup = POP_MENU;
    compose();
}

static int menu_hit(int x, int y) {
    ic_rect_t r = menu_rect();
    int at = r.y + 4 * S - menu.scroll;
    if (!in_rect(r, x, y)) return -1;
    for (int i = 0; i < menu.n; i++) {
        int h = menu_row_h(i);
        if (y >= at && y < at + h) return i;
        at += h;
    }
    return -1;
}

static int menu_selectable(int i) {
    return i >= 0 && i < menu.n && menu.item[i].enabled && menu.item[i].action != A_SEP && menu.item[i].action != A_GROUP;
}

static void draw_menu(ic_canvas_t *c) {
    const ic_face_t *body = ic_font(IC_FONT_BODY), *small = ic_font(IC_FONT_FOOTNOTE);
    ic_rect_t r = menu_rect(), saved;
    int y = r.y + 4 * S - menu.scroll, total = menu_content_h();
    popup_panel(c, r);
    ic_canvas_push_clip(c, r.x, r.y + 2 * S, r.w, r.h - 4 * S, &saved);
    for (int i = 0; i < menu.n; i++) {
        menu_item_t *it = &menu.item[i];
        int h = menu_row_h(i);
        if (y + h > r.y && y < r.y + r.h) {
            ic_rect_t tr = { r.x + 28 * S, y, r.w - 36 * S, h };
            if (it->action == A_SEP) {
                ic_gfx_hline(c, r.x + 10 * S, y + h / 2, r.w - 20 * S, C_LINE);
            } else if (it->action == A_GROUP) {
                tr.x = r.x + 14 * S;
                ic_text_draw_in(c, small, tr, it->label, C_TEXT2, IC_ALIGN_LEFT);
            } else {
                int hot = i == menu.hover && it->enabled;
                if (hot) ic_gfx_rrect(c, r.x + 5 * S, y + 1 * S, r.w - 10 * S, h - 2 * S, 6.0f * S, C_SELECT);
                if (it->checked) {
                    int cx = r.x + 15 * S, cy = y + h / 2;
                    ic_gfx_line(c, cx - 5 * S, cy, cx - 1 * S, cy + 4 * S, 1.8f * S, C_TEXT);
                    ic_gfx_line(c, cx - 1 * S, cy + 4 * S, cx + 6 * S, cy - 5 * S, 1.8f * S, C_TEXT);
                }
                {
                    char cut[96];
                    int n = ic_text_fit(body, it->label, tr.w);
                    snprintf(cut, sizeof cut, "%.*s", n, it->label);
                    ic_text_draw_in(c, body, tr, cut, it->enabled ? C_TEXT : C_TEXT3, IC_ALIGN_LEFT);
                }
            }
        }
        y += h;
    }
    ic_canvas_pop_clip(c, &saved);
    if (menu.scroll > 0) draw_chevron_up(c, r.x + r.w / 2, r.y + 6 * S);
    if (menu.scroll + r.h < total) draw_chevron_down(c, r.x + r.w / 2, r.y + r.h - 6 * S);
}

/* ---- the downloads list ---------------------------------------------------------------- */

enum { DL_ASKING, DL_RUNNING, DL_DONE, DL_FAILED, DL_CANCELLED };
typedef struct {
    WebKitDownload *dl;            /* referenced */
    int state;
    char name[256], folder[1024], uri[2048], error[160], suggested[256];
    guint64 received, total;
    guint64 rate_bytes;            /* received when the rate was last measured */
    uint32_t rate_t;
    double rate;                   /* bytes a second, smoothed */
} dlrec_t;

#define DL_MAX 40
static dlrec_t *dls[DL_MAX];       /* oldest first */
static int ndl, dl_dirty, dl_scroll, dl_hover;
static char dl_folder[1024] = DL_DIR;   /* where the last download went */
static uint32_t dl_drawn;

static int downloads_running(double *progress) {
    guint64 got = 0, total = 0;
    int n = 0;
    for (int i = 0; i < ndl; i++)
        if (dls[i]->state == DL_RUNNING) {
            n++;
            got += dls[i]->received;
            total += dls[i]->total > dls[i]->received ? dls[i]->total : dls[i]->received;
        }
    *progress = total ? (double)got / (double)total : 0;
    return n;
}

#define DL_ROW_H (58 * S)
#define DL_HEAD_H (44 * S)

static ic_rect_t downloads_rect(void) {
    ic_rect_t r;
    r.w = 400 * S < W - 16 * S ? 400 * S : W - 16 * S;
    r.x = W - r.w - 8 * S;
    r.y = TOP + 6 * S;
    r.h = DL_HEAD_H + (ndl ? ndl * DL_ROW_H : 60 * S) + 8 * S;
    if (r.y + r.h > H - 8 * S) r.h = H - 8 * S - r.y;
    return r;
}

/* row i of the list (newest first): its rectangle, and its buttons' */
static ic_rect_t dl_row(int i) {
    ic_rect_t r = downloads_rect(), row = { r.x, r.y + DL_HEAD_H + i * DL_ROW_H - dl_scroll, r.w, DL_ROW_H };
    return row;
}

static ic_rect_t dl_button(int i, int which) {      /* 0 the main one (cancel / retry), 1 remove */
    ic_rect_t row = dl_row(i), b = { row.x + row.w - (which ? 34 : 68) * S, row.y + 15 * S, 28 * S, 28 * S };
    return b;
}

static ic_rect_t dl_clear_rect(void) {
    ic_rect_t r = downloads_rect(), b = { r.x + r.w - 92 * S, r.y + 8 * S, 80 * S, 28 * S };
    return b;
}

static dlrec_t *dl_at(int row) { return row >= 0 && row < ndl ? dls[ndl - 1 - row] : 0; }

static void dl_detail(dlrec_t *d, char *out, size_t cap) {
    char a[32], b[32], rate[32];
    human_size(d->received, a, sizeof a);
    human_size(d->total, b, sizeof b);
    switch (d->state) {
    case DL_ASKING: snprintf(out, cap, "Waiting for where to save it"); break;
    case DL_RUNNING:
        human_size((guint64)d->rate, rate, sizeof rate);
        if (d->total) snprintf(out, cap, "%s of %s - %s/s", a, b, rate);
        else snprintf(out, cap, "%s - %s/s", a, rate);
        break;
    case DL_DONE: snprintf(out, cap, "%s - in %s", a, d->folder); break;
    case DL_FAILED: snprintf(out, cap, "Failed: %s", d->error[0] ? d->error : "error"); break;
    default: snprintf(out, cap, "Cancelled"); break;
    }
}

static void draw_downloads(ic_canvas_t *c) {
    const ic_face_t *body = ic_font(IC_FONT_BODY), *small = ic_font(IC_FONT_FOOTNOTE), *head = ic_font(IC_FONT_HEADLINE);
    ic_rect_t r = downloads_rect(), saved, t = { r.x + 16 * S, r.y + 8 * S, r.w - 120 * S, 28 * S };
    int finished = 0;
    popup_panel(c, r);
    ic_text_draw_in(c, head, t, "Downloads", C_TEXT, IC_ALIGN_LEFT);
    for (int i = 0; i < ndl; i++) finished += dls[i]->state != DL_RUNNING && dls[i]->state != DL_ASKING;
    if (finished) draw_button(c, dl_clear_rect(), "Clear", 0, dl_hover == -2);
    ic_gfx_hline(c, r.x, r.y + DL_HEAD_H - 1, r.w, C_LINE);
    ic_canvas_push_clip(c, r.x, r.y + DL_HEAD_H, r.w, r.h - DL_HEAD_H - 4 * S, &saved);
    if (!ndl) {
        ic_rect_t e = { r.x, r.y + DL_HEAD_H, r.w, 60 * S };
        ic_text_draw_in(c, small, e, "Nothing downloaded yet", C_TEXT2, IC_ALIGN_CENTER);
    }
    for (int i = 0; i < ndl; i++) {
        dlrec_t *d = dl_at(i);
        ic_rect_t row = dl_row(i), nr = { row.x + 16 * S, row.y + 8 * S, row.w - 100 * S, 22 * S },
                  dr = { row.x + 16 * S, row.y + 30 * S, row.w - 100 * S, 18 * S };
        char detail[300], cut[300];
        if (row.y + row.h < r.y + DL_HEAD_H || row.y > r.y + r.h) continue;
        snprintf(cut, sizeof cut, "%.*s", ic_text_fit(body, d->name, nr.w), d->name);
        ic_text_draw_in(c, body, nr, cut, d->state == DL_CANCELLED ? C_TEXT2 : C_TEXT, IC_ALIGN_LEFT);
        dl_detail(d, detail, sizeof detail);
        snprintf(cut, sizeof cut, "%.*s", ic_text_fit(small, detail, dr.w), detail);
        ic_text_draw_in(c, small, dr, cut, d->state == DL_FAILED ? 0xFFFF6961u : C_TEXT2, IC_ALIGN_LEFT);
        if (d->state == DL_RUNNING) {
            int bw = row.w - 100 * S;
            double p = d->total ? (double)d->received / (double)d->total : 0;
            ic_gfx_rrect(c, row.x + 16 * S, row.y + row.h - 8 * S, bw, 3 * S, 1.5f * S, C_LINE);
            if (p > 0) ic_gfx_rrect(c, row.x + 16 * S, row.y + row.h - 8 * S, (int)(bw * (p > 1 ? 1 : p)), 3 * S, 1.5f * S, C_ACCENT);
        }
        {   /* cancel while it runs; again after it failed or was cancelled; remove when it ended */
            ic_rect_t b0 = dl_button(i, 0), b1 = dl_button(i, 1);
            int ended = d->state != DL_RUNNING && d->state != DL_ASKING;
            if (d->state == DL_RUNNING || d->state == DL_ASKING) {
                if (dl_hover == i * 2 + 1) ic_gfx_circle(c, b1.x + b1.w / 2, b1.y + b1.h / 2, b1.w / 2, C_TAB_HOVER);
                draw_x(c, b1.x + b1.w / 2, b1.y + b1.h / 2, 5 * S, C_TEXT);
            }
            if (d->state == DL_FAILED || d->state == DL_CANCELLED) {
                if (dl_hover == i * 2) ic_gfx_circle(c, b0.x + b0.w / 2, b0.y + b0.h / 2, b0.w / 2, C_TAB_HOVER);
                draw_reload_icon(c, b0.x + b0.w / 2, b0.y + b0.h / 2, C_TEXT, C_POPUP);
            }
            if (ended) {
                if (dl_hover == i * 2 + 1) ic_gfx_circle(c, b1.x + b1.w / 2, b1.y + b1.h / 2, b1.w / 2, C_TAB_HOVER);
                draw_x(c, b1.x + b1.w / 2, b1.y + b1.h / 2, 4 * S, C_TEXT2);
            }
        }
        if (i + 1 < ndl) ic_gfx_hline(c, row.x + 16 * S, row.y + row.h - 1, row.w - 32 * S, C_LINE);
    }
    ic_canvas_pop_clip(c, &saved);
}

/* ---- sheets ----------------------------------------------------------------------------- */

enum { SH_SAVE = 1, SH_OPEN, SH_ALERT, SH_CONFIRM, SH_PROMPT, SH_LEAVE };
enum { HB_CANCEL = -10, HB_OK = -11, HB_UP = -12, HB_HOME = -13, HB_DL = -14, HB_DRIVES = -15, HB_FIELD = -16 };

#define LIST_MAX 600
typedef struct { char name[128]; int dir; } entry_t;
static entry_t list[LIST_MAX];
static int nlist;

static struct {
    int kind;
    char title[200], message[1024];
    char field[512];
    int field_all, field_focus;
    char folder[1024];
    int hover, sel, scroll;
    dlrec_t *dl;
    WebKitFileChooserRequest *chooser;    /* referenced */
    WebKitScriptDialog *dialog;           /* referenced */
} sheet;

static int entry_cmp(const void *a, const void *b) {
    const entry_t *x = (const entry_t *)a, *y = (const entry_t *)b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

/* the folder's folders (and files, for choosing one), sorted, hidden ones left out */
static void list_load(void) {
    DIR *d = opendir(sheet.folder);
    struct dirent *e;
    nlist = 0;
    sheet.sel = -1;
    sheet.scroll = 0;
    if (!d) return;
    while ((e = readdir(d)) && nlist < LIST_MAX) {
        char path[1200];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", strcmp(sheet.folder, "/") ? sheet.folder : "", e->d_name);
        if (stat(path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && sheet.kind != SH_OPEN) continue;
        snprintf(list[nlist].name, sizeof list[0].name, "%s", e->d_name);
        list[nlist].dir = S_ISDIR(st.st_mode);
        nlist++;
    }
    closedir(d);
    qsort(list, (size_t)nlist, sizeof list[0], entry_cmp);
}

static void sheet_go(const char *folder) {
    char real[1024];
    snprintf(real, sizeof real, "%s", folder[0] ? folder : "/");
    {   /* no trailing slash but on "/" */
        size_t n = strlen(real);
        while (n > 1 && real[n - 1] == '/') real[--n] = 0;
    }
    snprintf(sheet.folder, sizeof sheet.folder, "%s", real);
    list_load();
}

static int sheet_is_file(void) { return sheet.kind == SH_SAVE || sheet.kind == SH_OPEN; }

static ic_rect_t sheet_rect(void) {
    ic_rect_t r;
    int ph = H - TOP;
    if (sheet_is_file()) {
        r.w = 560 * S < W - 24 * S ? 560 * S : W - 24 * S;
        r.h = 460 * S < ph - 24 * S ? 460 * S : ph - 24 * S;
    } else {
        r.w = 440 * S < W - 24 * S ? 440 * S : W - 24 * S;
        r.h = (sheet.kind == SH_PROMPT ? 232 : 186) * S;
        if (r.h > ph - 24 * S) r.h = ph - 24 * S;
    }
    r.x = (W - r.w) / 2;
    r.y = TOP + (ph - r.h) / 3;
    if (r.y < TOP + 12 * S) r.y = TOP + 12 * S;
    return r;
}

static ic_rect_t sheet_button(int which) {          /* HB_OK, HB_CANCEL */
    ic_rect_t r = sheet_rect(), b = { 0, r.y + r.h - 48 * S, 96 * S, 34 * S };
    b.x = r.x + r.w - 16 * S - b.w - (which == HB_CANCEL && sheet.kind != SH_ALERT ? b.w + 10 * S : 0);
    return b;
}

static ic_rect_t sheet_field(void) {
    ic_rect_t r = sheet_rect(), f = { r.x + 16 * S, 0, r.w - 32 * S, 34 * S };
    f.y = sheet_is_file() ? r.y + 50 * S : r.y + r.h - 96 * S;
    return f;
}

static int sheet_path_y(void) { ic_rect_t r = sheet_rect(); return r.y + (sheet.kind == SH_SAVE ? 96 : 50) * S; }

static ic_rect_t sheet_chip(int which) {             /* HB_UP, HB_HOME, HB_DL, HB_DRIVES */
    ic_rect_t r = sheet_rect(), b = { r.x + 16 * S, sheet_path_y(), 32 * S, 28 * S };
    static const int widths[] = { 32, 64, 92, 64 };
    int k = HB_UP - which;
    for (int i = 0; i < k; i++) b.x += widths[i] * S + 6 * S;
    b.w = widths[k] * S;
    return b;
}

static ic_rect_t sheet_list(void) {
    ic_rect_t r = sheet_rect(), l;
    l.x = r.x + 16 * S;
    l.w = r.w - 32 * S;
    l.y = sheet_path_y() + 64 * S;
    l.h = r.y + r.h - 60 * S - l.y;
    return l;
}

#define ROW_H (28 * S)

static int sheet_hit(int x, int y) {
    static const int chips[] = { HB_UP, HB_HOME, HB_DL, HB_DRIVES };
    if (in_rect(sheet_button(HB_OK), x, y)) return HB_OK;
    if (sheet.kind != SH_ALERT && in_rect(sheet_button(HB_CANCEL), x, y)) return HB_CANCEL;
    if ((sheet.kind == SH_SAVE || sheet.kind == SH_PROMPT) && in_rect(sheet_field(), x, y)) return HB_FIELD;
    if (sheet_is_file()) {
        ic_rect_t l = sheet_list();
        for (int i = 0; i < 4; i++)
            if (in_rect(sheet_chip(chips[i]), x, y)) return chips[i];
        if (in_rect(l, x, y)) {
            int row = (y - l.y + sheet.scroll) / ROW_H;
            if (row >= 0 && row < nlist) return row;
        }
    }
    return -1;
}

static void draw_sheet(ic_canvas_t *c) {
    const ic_face_t *body = ic_font(IC_FONT_BODY), *small = ic_font(IC_FONT_FOOTNOTE), *head = ic_font(IC_FONT_HEADLINE);
    ic_rect_t r = sheet_rect(), t = { r.x + 16 * S, r.y + 12 * S, r.w - 32 * S, 28 * S };
    const char *ok = sheet.kind == SH_SAVE ? "Save" : sheet.kind == SH_OPEN ? "Open" : sheet.kind == SH_LEAVE ? "Leave" : "OK";
    popup_panel(c, r);
    {
        char cut[200];
        snprintf(cut, sizeof cut, "%.*s", ic_text_fit(head, sheet.title, t.w), sheet.title);
        ic_text_draw_in(c, head, t, cut, C_TEXT, IC_ALIGN_LEFT);
    }
    if (sheet_is_file()) {
        static const int chips[] = { HB_UP, HB_HOME, HB_DL, HB_DRIVES };
        static const char *const names[] = { "", "Home", "Downloads", "Drives" };
        ic_rect_t l = sheet_list(), saved, pr;
        if (sheet.kind == SH_SAVE) draw_field(c, sheet_field(), sheet.field, 1, sheet.field_all, "File name");
        for (int i = 0; i < 4; i++) {
            ic_rect_t b = sheet_chip(chips[i]);
            ic_gfx_rrect(c, b.x, b.y, b.w, b.h, 7.0f * S, sheet.hover == chips[i] ? 0xFF4A4A50u : 0xFF3C3C41u);
            if (i == 0) {                                       /* up: a chevron */
                int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
                ic_gfx_line(c, cx - 5 * S, cy + 2 * S, cx, cy - 3 * S, 1.6f * S, C_TEXT);
                ic_gfx_line(c, cx, cy - 3 * S, cx + 5 * S, cy + 2 * S, 1.6f * S, C_TEXT);
            } else {
                ic_text_draw_in(c, small, b, names[i], C_TEXT, IC_ALIGN_CENTER);
            }
        }
        pr.x = r.x + 16 * S;
        pr.y = sheet_path_y() + 32 * S;
        pr.w = r.w - 32 * S;
        pr.h = 26 * S;
        {   /* the folder's path, its end in view */
            const char *p = sheet.folder;
            while (*p && ic_text_measure(small, p) > pr.w) p++;
            ic_text_draw_in(c, small, pr, p, C_TEXT2, IC_ALIGN_LEFT);
        }
        ic_gfx_rrect(c, l.x, l.y, l.w, l.h, 8.0f * S, C_FIELD);
        ic_canvas_push_clip(c, l.x, l.y + 2 * S, l.w, l.h - 4 * S, &saved);
        if (!nlist) {
            ic_rect_t e = { l.x, l.y, l.w, 40 * S };
            ic_text_draw_in(c, small, e, sheet.kind == SH_OPEN ? "This folder is empty" : "No folders in this one", C_TEXT3, IC_ALIGN_CENTER);
        }
        for (int i = 0; i < nlist; i++) {
            int y = l.y + i * ROW_H - sheet.scroll + 2 * S;
            ic_rect_t tr = { l.x + 34 * S, y, l.w - 44 * S, ROW_H };
            char cut[160];
            if (y + ROW_H < l.y || y > l.y + l.h) continue;
            if (i == sheet.sel) ic_gfx_rrect(c, l.x + 4 * S, y, l.w - 8 * S, ROW_H, 6.0f * S, C_SELECT);
            else if (i == sheet.hover) ic_gfx_rrect(c, l.x + 4 * S, y, l.w - 8 * S, ROW_H, 6.0f * S, C_TAB_HOVER);
            if (list[i].dir) {                                  /* a folder */
                int fx = l.x + 12 * S, fy = y + 8 * S;
                ic_gfx_rrect(c, fx, fy, 8 * S, 4 * S, 1.5f * S, 0xFF5AA9FFu);
                ic_gfx_rrect(c, fx, fy + 2 * S, 16 * S, 11 * S, 2.0f * S, 0xFF5AA9FFu);
            } else {                                            /* a file */
                int fx = l.x + 14 * S, fy = y + 6 * S;
                ic_gfx_rrect_stroke(c, fx, fy, 12 * S, 16 * S, 2.0f * S, 1.2f * S, C_TEXT2);
            }
            snprintf(cut, sizeof cut, "%.*s", ic_text_fit(body, list[i].name, tr.w), list[i].name);
            ic_text_draw_in(c, body, tr, cut, list[i].dir || sheet.kind == SH_OPEN ? C_TEXT : C_TEXT2, IC_ALIGN_LEFT);
        }
        ic_canvas_pop_clip(c, &saved);
    } else {
        ic_rect_t m = { r.x + 16 * S, r.y + 46 * S, r.w - 32 * S, (sheet.kind == SH_PROMPT ? 232 - 46 - 104 : 186 - 46 - 60) * S }, saved;
        ic_canvas_push_clip(c, m.x, m.y, m.w, m.h, &saved);
        ic_text_draw_wrapped(c, body, m, sheet.message, C_TEXT, 2 * S);
        ic_canvas_pop_clip(c, &saved);
        if (sheet.kind == SH_PROMPT) draw_field(c, sheet_field(), sheet.field, 1, sheet.field_all, 0);
    }
    if (sheet.kind != SH_ALERT)
        draw_button(c, sheet_button(HB_CANCEL), sheet.kind == SH_LEAVE ? "Stay" : "Cancel", 0, sheet.hover == HB_CANCEL);
    draw_button(c, sheet_button(HB_OK), ok, 1, sheet.hover == HB_OK);
}

static void dl_save_to(dlrec_t *d, const char *folder, const char *name);
static void sheet_finish(int ok);

static void sheet_open(int kind, const char *title) {
    if (popup == POP_MENU || popup == POP_DOWNLOADS) popup_close();
    memset(&sheet, 0, sizeof sheet);
    sheet.kind = kind;
    sheet.hover = -1;
    sheet.sel = -1;
    snprintf(sheet.title, sizeof sheet.title, "%s", title);
    popup = POP_SHEET;
    addr_focus = 0;
}

/* the answer: ok 1 (Save, Open, OK, Leave) or 0 (Cancel, Escape, closed) */
static void sheet_finish(int ok) {
    int kind = sheet.kind;
    if (popup != POP_SHEET) return;
    popup = POP_NONE;
    sheet.kind = 0;
    if (kind == SH_SAVE && sheet.dl) {
        dlrec_t *d = sheet.dl;
        char name[512];
        size_t n;
        snprintf(name, sizeof name, "%s", sheet.field);
        n = strlen(name);
        while (n && name[n - 1] == ' ') name[--n] = 0;
        for (char *p = name; *p; p++)
            if (*p == '/') *p = '_';
        if (ok && name[0]) {
            snprintf(dl_folder, sizeof dl_folder, "%s", sheet.folder);
            dl_save_to(d, sheet.folder, name);
        } else {
            webkit_download_cancel(d->dl);
        }
    } else if (kind == SH_OPEN && sheet.chooser) {
        if (ok && sheet.sel >= 0 && sheet.sel < nlist && !list[sheet.sel].dir) {
            char path[1200];
            const char *files[2] = { path, NULL };
            snprintf(path, sizeof path, "%s/%s", strcmp(sheet.folder, "/") ? sheet.folder : "", list[sheet.sel].name);
            webkit_file_chooser_request_select_files(sheet.chooser, files);
        } else {
            webkit_file_chooser_request_cancel(sheet.chooser);
        }
        g_object_unref(sheet.chooser);
        sheet.chooser = 0;
    } else if (sheet.dialog) {
        WebKitScriptDialog *dlg = sheet.dialog;
        sheet.dialog = 0;
        if (kind == SH_CONFIRM || kind == SH_LEAVE) webkit_script_dialog_confirm_set_confirmed(dlg, ok);
        if (kind == SH_PROMPT && ok) webkit_script_dialog_prompt_set_text(dlg, sheet.field);
        webkit_script_dialog_close(dlg);
        webkit_script_dialog_unref(dlg);
    }
    compose();
}

/* the sheet's list: a folder entered, a file chosen */
static void sheet_pick(int row) {
    if (row < 0 || row >= nlist) return;
    if (list[row].dir) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", strcmp(sheet.folder, "/") ? sheet.folder : "", list[row].name);
        sheet_go(path);
    } else if (sheet.kind == SH_OPEN) {
        if (sheet.sel == row) { sheet_finish(1); return; }   /* a second click on it */
        sheet.sel = row;
    }
    compose();
}

static void sheet_up(void) {
    char up[1024], *slash;
    snprintf(up, sizeof up, "%s", sheet.folder);
    slash = strrchr(up, '/');
    if (!slash) return;
    if (slash == up) up[1] = 0;
    else *slash = 0;
    sheet_go(up);
    compose();
}

/* ---- the find bar ------------------------------------------------------------------------- */

static struct {
    int open, focus, all, failed;
    guint count;
    char text[256];
} find;

static ic_rect_t find_rect(void) {
    ic_rect_t r;
    r.w = 380 * S < W - 16 * S ? 380 * S : W - 16 * S;
    r.h = 44 * S;
    r.x = W - r.w - 8 * S;
    r.y = TOP + 6 * S;
    return r;
}

static ic_rect_t find_part(int which) {             /* 0 field, 1 previous, 2 next, 3 close */
    ic_rect_t r = find_rect(), p;
    p.y = r.y + 6 * S;
    p.h = 32 * S;
    if (which == 0) {
        p.x = r.x + 6 * S;
        p.w = r.w - 6 * S - 3 * 30 * S - 8 * S;
    } else {
        p.w = 28 * S;
        p.x = r.x + r.w - 6 * S - (4 - which) * 30 * S;
    }
    return p;
}

static void draw_find(ic_canvas_t *c) {
    ic_rect_t r = find_rect(), f = find_part(0), cr;
    char count[48] = "";
    popup_panel(c, r);
    draw_field(c, f, find.text, find.focus, find.all, "Find in page");
    if (find.text[0]) {
        if (find.failed || !find.count) snprintf(count, sizeof count, "No matches");
        else snprintf(count, sizeof count, "%u match%s", find.count, find.count == 1 ? "" : "es");
        cr = f;
        cr.w -= 10 * S;
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), cr, count, C_TEXT2, IC_ALIGN_RIGHT);
    }
    for (int i = 1; i <= 3; i++) {
        ic_rect_t b = find_part(i);
        int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (in_rect(b, mouse_x, mouse_y)) ic_gfx_circle(c, cx, cy, b.w / 2, C_TAB_HOVER);
        if (i == 1) draw_chevron_up(c, cx, cy);
        else if (i == 2) draw_chevron_down(c, cx, cy);
        else draw_x(c, cx, cy, 5 * S, C_TEXT);
    }
}

static void find_search(int backwards) {
    WebKitFindController *fc;
    if (!ntabs) return;
    fc = webkit_web_view_get_find_controller(tabs[active]->view);
    if (!find.text[0]) {
        webkit_find_controller_search_finish(fc);
        find.count = 0;
        find.failed = 0;
        return;
    }
    if (backwards < 0) {
        const char *was = webkit_find_controller_get_search_text(fc);
        guint32 opt = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
        webkit_find_controller_search(fc, find.text, opt, 10000);
        if (!was || strcmp(was, find.text)) webkit_find_controller_count_matches(fc, find.text, opt, 10000);
        return;
    }
    if (backwards) webkit_find_controller_search_previous(fc);
    else webkit_find_controller_search_next(fc);
}

static void find_redraw(void) {
    ic_rect_t r = find_rect();
    update_area(r.y - 16 * S, r.y + r.h + 16 * S);
}

static void find_open(void) {
    popup_close();
    find.open = 1;
    find.focus = 1;
    find.all = 1;
    addr_focus = 0;
    sync_addr();
    compose();
}

static void find_close(void) {
    ic_rect_t r = find_rect();
    if (!find.open) return;
    find.open = find.focus = 0;
    if (ntabs) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(tabs[active]->view));
    update_area(r.y - 16 * S, r.y + r.h + 16 * S);
}

static void on_find_count(WebKitFindController *fc, guint n, gpointer d) {
    (void)fc; (void)d;
    find.count = n;
    if (find.open) find_redraw();
}

static void on_find_found(WebKitFindController *fc, guint n, gpointer d) {
    (void)fc; (void)n; (void)d;
    find.failed = 0;
    if (find.open) find_redraw();
}

static void on_find_failed(WebKitFindController *fc, gpointer d) {
    (void)fc; (void)d;
    find.failed = 1;
    find.count = 0;
    if (find.open) find_redraw();
}

static int find_key(uint32_t keysym, uint32_t mods) {
    if (!find.open || !find.focus) return 0;
    if (keysym == 0xff1b) { find_close(); return 1; }
    if (keysym == 0xff0d) { find_search((mods & M_SHIFT) != 0); return 1; }
    if (keysym == 0xff52) { find_search(1); return 1; }
    if (keysym == 0xff54) { find_search(0); return 1; }
    {
        char was[sizeof find.text];
        snprintf(was, sizeof was, "%s", find.text);
        if (!field_key(find.text, sizeof find.text, &find.all, keysym, mods)) return 0;
        if (strcmp(was, find.text)) {
            find.failed = 0;
            find.count = 0;
            find_search(-1);
        }
        find_redraw();
    }
    return 1;
}

static int find_mouse(int x, int y, uint32_t pressed) {
    if (!find.open || !in_rect(find_rect(), x, y)) {
        if (find.open && find.focus && (pressed & GUI_BTN_LEFT)) {   /* the page took the focus */
            find.focus = 0;
            find_redraw();
        }
        return 0;
    }
    if (pressed & GUI_BTN_LEFT) {
        if (in_rect(find_part(1), x, y)) find_search(1);
        else if (in_rect(find_part(2), x, y)) find_search(0);
        else if (in_rect(find_part(3), x, y)) { find_close(); return 1; }
        else { find.focus = 1; addr_focus = 0; }
    }
    find_redraw();
    return 1;
}

/* ---- drawing and input for all of them ------------------------------------------------- */

/* where a popup is, with room for its shadow (h 0: none) */
static ic_rect_t popup_bounds(void) {
    ic_rect_t r = { 0, 0, 0, 0 };
    if (popup == POP_MENU) r = menu_rect();
    else if (popup == POP_DOWNLOADS) r = downloads_rect();
    else if (popup == POP_SHEET) { r.x = 0; r.y = TOP; r.w = W; r.h = H - TOP; return r; }
    if (r.h) {
        r.x -= 16 * S;
        r.y -= 16 * S;
        r.w += 32 * S;
        r.h += 32 * S;
    }
    return r;
}

/* the find bar and the popup, on rows top .. bottom - 1 only */
static void draw_overlays(ic_canvas_t *c, int top, int bottom) {
    ic_rect_t saved;
    if (top < TOP) top = TOP;
    if (bottom <= top || (!find.open && popup == POP_NONE)) return;
    ic_canvas_push_clip(c, 0, top, W, bottom - top, &saved);
    if (find.open) draw_find(c);
    if (popup == POP_MENU) draw_menu(c);
    else if (popup == POP_DOWNLOADS) draw_downloads(c);
    else if (popup == POP_SHEET) draw_sheet(c);
    ic_canvas_pop_clip(c, &saved);
}

static void popup_redraw(void) {
    ic_rect_t b = popup_bounds();
    if (b.h) update_area(b.y, b.y + b.h);
}

static void popup_close(void) {
    ic_rect_t b = popup_bounds();
    if (popup == POP_NONE) return;
    if (popup == POP_SHEET) {
        sheet_finish(0);
        return;
    }
    if (popup == POP_MENU && menu.options) {
        WebKitOptionMenu *o = menu.options;
        menu.options = 0;
        webkit_option_menu_close(o);
        g_object_unref(o);
    }
    popup = POP_NONE;
    update_area(b.y, b.y + b.h);
}

static void menu_run(int action);

static void menu_activate(int i) {
    int action;
    if (!menu_selectable(i)) return;
    action = menu.item[i].action;
    if (action == A_OPTION && menu.options) webkit_option_menu_activate_item(menu.options, (guint)menu.item[i].opt);
    popup_close();
    if (action != A_OPTION) menu_run(action);
}

static void downloads_click(int x, int y);
static void downloads_hover(int x, int y);

static int popup_mouse(int x, int y, uint32_t pressed, int wheel, int moved) {
    if (popup == POP_MENU) {
        ic_rect_t r = menu_rect();
        if (!in_rect(r, x, y)) {
            if (pressed) popup_close();
            return 1;
        }
        if (wheel) {
            int max = menu_content_h() - r.h;
            menu.scroll += wheel * 3 * 30 * S;
            if (menu.scroll > max) menu.scroll = max;
            if (menu.scroll < 0) menu.scroll = 0;
            popup_redraw();
        }
        if (moved || wheel) {
            int h = menu_hit(x, y);
            if (h != menu.hover) {
                menu.hover = h;
                popup_redraw();
            }
        }
        if (pressed & GUI_BTN_LEFT) menu_activate(menu_hit(x, y));
        return 1;
    }
    if (popup == POP_DOWNLOADS) {
        if (!in_rect(downloads_rect(), x, y)) {
            if (!pressed) return 0;                          /* hovering elsewhere is the page's */
            popup_close();
            return in_rect(tool_rect(0), x, y);              /* the button closed it: not open it again */
        }
        if (wheel) {
            ic_rect_t r = downloads_rect();
            int max = DL_HEAD_H + ndl * DL_ROW_H + 8 * S - r.h;
            dl_scroll += wheel * DL_ROW_H;
            if (dl_scroll > max) dl_scroll = max;
            if (dl_scroll < 0) dl_scroll = 0;
            popup_redraw();
        }
        if (moved) downloads_hover(x, y);
        if (pressed & GUI_BTN_LEFT) downloads_click(x, y);
        return 1;
    }
    if (popup == POP_SHEET) {
        int h = sheet_hit(x, y);
        if (wheel && sheet_is_file()) {
            int max = nlist * ROW_H - sheet_list().h + 4 * S;
            sheet.scroll += wheel * 3 * ROW_H;
            if (sheet.scroll > max) sheet.scroll = max;
            if (sheet.scroll < 0) sheet.scroll = 0;
            popup_redraw();
        }
        if ((moved || wheel) && h != sheet.hover) {
            sheet.hover = h;
            popup_redraw();
        }
        if (pressed & GUI_BTN_LEFT) {
            if (h == HB_OK) sheet_finish(1);
            else if (h == HB_CANCEL) sheet_finish(0);
            else if (h == HB_UP) sheet_up();
            else if (h == HB_HOME) { sheet_go("/home"); compose(); }
            else if (h == HB_DL) { mkdir(DL_DIR, 0755); sheet_go(DL_DIR); compose(); }
            else if (h == HB_DRIVES) { sheet_go("/volumes"); compose(); }
            else if (h >= 0) sheet_pick(h);
        }
        return 1;
    }
    return 0;
}

static int popup_key(uint32_t keysym, uint32_t mods) {
    if (popup == POP_MENU) {
        int i = menu.hover, step = 0;
        if (keysym == 0xff1b) { popup_close(); return 1; }
        if (keysym == 0xff0d || keysym == ' ') { menu_activate(menu.hover); return 1; }
        if (keysym == 0xff52) step = -1;
        else if (keysym == 0xff54) step = 1;
        else if (keysym == 0xff55) step = -10;
        else if (keysym == 0xff56) step = 10;
        else if (keysym == 0xff50) { i = -1; step = 1; }
        else if (keysym == 0xff57) { i = menu.n; step = -1; }
        else if (keysym < 0xff00 && keysym > ' ' && !(mods & M_CTRL)) {
            /* a letter: the next item starting with it */
            for (int k = 1; k <= menu.n; k++) {
                int j = (menu.hover + k + menu.n) % menu.n;
                char ch = menu.item[j].label[0];
                if (menu_selectable(j) && (ch | 32) == (char)(keysym | 32)) { i = j; break; }
            }
        }
        if (step) {
            int dir = step > 0 ? 1 : -1, left = step * dir;
            while (left > 0) {
                int j = i + dir;
                while (j >= 0 && j < menu.n && !menu_selectable(j)) j += dir;
                if (j < 0 || j >= menu.n) break;
                i = j;
                left--;
            }
        }
        if (i != menu.hover && menu_selectable(i)) {
            ic_rect_t r = menu_rect();
            int y = 4 * S;
            menu.hover = i;
            for (int k = 0; k < i; k++) y += menu_row_h(k);
            if (y - menu.scroll < 0) menu.scroll = y - 4 * S;
            if (y + 30 * S - menu.scroll > r.h) menu.scroll = y + 34 * S - r.h;
            popup_redraw();
        }
        return 1;
    }
    if (popup == POP_DOWNLOADS) {
        if (keysym == 0xff1b) { popup_close(); return 1; }
        popup_close();
        return 0;
    }
    if (popup == POP_SHEET) {
        if (keysym == 0xff1b) { sheet_finish(0); return 1; }
        if (keysym == 0xff0d) {
            if (sheet.kind == SH_OPEN && sheet.sel >= 0 && list[sheet.sel].dir) sheet_pick(sheet.sel);
            else sheet_finish(1);
            return 1;
        }
        if (sheet_is_file() && (keysym == 0xff52 || keysym == 0xff54) && nlist) {
            int i = sheet.sel + (keysym == 0xff54 ? 1 : -1);
            ic_rect_t l = sheet_list();
            if (i < 0) i = 0;
            if (i >= nlist) i = nlist - 1;
            sheet.sel = i;
            if (i * ROW_H < sheet.scroll) sheet.scroll = i * ROW_H;
            if ((i + 1) * ROW_H > sheet.scroll + l.h - 4 * S) sheet.scroll = (i + 1) * ROW_H - l.h + 4 * S;
            popup_redraw();
            return 1;
        }
        if (sheet.kind == SH_OPEN && keysym == 0xff08) { sheet_up(); return 1; }
        if (sheet.kind == SH_SAVE || sheet.kind == SH_PROMPT) {
            if (field_key(sheet.field, sizeof sheet.field, &sheet.field_all, keysym, mods)) popup_redraw();
        }
        return 1;
    }
    return 0;
}

/* ---- downloads -------------------------------------------------------------------------- */

static dlrec_t *dl_find(WebKitDownload *dl) {
    for (int i = 0; i < ndl; i++)
        if (dls[i]->dl == dl) return dls[i];
    return 0;
}

static void dl_remove(dlrec_t *d) {
    for (int i = 0; i < ndl; i++) {
        if (dls[i] != d) continue;
        memmove(&dls[i], &dls[i + 1], sizeof dls[0] * (size_t)(ndl - i - 1));
        ndl--;
        if (sheet.dl == d) sheet.dl = 0;
        g_signal_handlers_disconnect_matched(d->dl, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, d);
        g_object_unref(d->dl);
        g_free(d);
        return;
    }
}

static void downloads_changed(int size_changed) {
    dl_dirty = 0;
    dl_drawn = now_ms();
    chrome_dirty = 1;
    if (popup != POP_DOWNLOADS) return;
    if (size_changed) compose();
    else popup_redraw();
}

static void downloads_toggle(void) {
    if (popup == POP_DOWNLOADS) { popup_close(); return; }
    popup_close();
    if (popup != POP_NONE) return;                        /* a sheet is waiting for an answer */
    dl_unseen = 0;
    dl_scroll = 0;
    dl_hover = -1;
    popup = POP_DOWNLOADS;
    chrome_dirty = 1;
    compose();
}

static int dl_hit(int x, int y) {          /* row * 2 (+1 for the second button), -2 Clear, -1 none */
    ic_rect_t r = downloads_rect();
    if (in_rect(dl_clear_rect(), x, y)) return -2;
    if (y < r.y + DL_HEAD_H) return -1;
    for (int i = 0; i < ndl; i++) {
        if (in_rect(dl_button(i, 0), x, y)) return i * 2;
        if (in_rect(dl_button(i, 1), x, y)) return i * 2 + 1;
    }
    return -1;
}

static void downloads_hover(int x, int y) {
    int h = dl_hit(x, y);
    if (h != dl_hover) {
        dl_hover = h;
        popup_redraw();
    }
}

static void downloads_click(int x, int y) {
    int h = dl_hit(x, y);
    dlrec_t *d;
    if (h == -2) {                                     /* Clear: the ended ones go */
        for (int i = ndl - 1; i >= 0; i--)
            if (dls[i]->state != DL_RUNNING && dls[i]->state != DL_ASKING) dl_remove(dls[i]);
        dl_scroll = 0;
        compose();
        return;
    }
    if (h < 0 || !(d = dl_at(h / 2))) return;
    if (h % 2 == 0) {
        if (d->state == DL_FAILED || d->state == DL_CANCELLED) {   /* again */
            char uri[2048];
            snprintf(uri, sizeof uri, "%s", d->uri);
            dl_remove(d);
            if (uri[0]) webkit_network_session_download_uri(session, uri);
            compose();
        }
    } else if (d->state == DL_RUNNING || d->state == DL_ASKING) {
        webkit_download_cancel(d->dl);
    } else {
        dl_remove(d);
        compose();
    }
}

/* the first of "name", "name (1)", ... that is not in the folder yet */
static void dl_unique(const char *folder, const char *name, char *path, size_t cap) {
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    struct stat st;
    snprintf(path, cap, "%s/%s", folder, name);
    for (int i = 1; i < 1000 && stat(path, &st) == 0; i++)
        snprintf(path, cap, "%s/%.*s (%d)%s", folder, stem, name, i, name + stem);
}

static void dl_save_to(dlrec_t *d, const char *folder, const char *name) {
    char path[1600];
    mkdir(folder, 0755);
    dl_unique(folder, name, path, sizeof path);
    snprintf(d->folder, sizeof d->folder, "%s", folder);
    snprintf(d->name, sizeof d->name, "%s", strrchr(path, '/') + 1);
    d->state = DL_RUNNING;
    d->rate_t = now_ms();
    webkit_download_set_destination(d->dl, path);
    {
        char msg[400];
        snprintf(msg, sizeof msg, "Downloading %s", d->name);
        set_status(msg, 3000);
    }
    downloads_changed(1);
}

static void dl_received(WebKitDownload *dl, guint64 n, gpointer data) {
    dlrec_t *d = (dlrec_t *)data;
    uint32_t now = now_ms();
    (void)dl;
    d->received += n;
    if (now - d->rate_t >= 1000) {
        double r = (double)(d->received - d->rate_bytes) * 1000.0 / (double)(now - d->rate_t);
        d->rate = d->rate > 0 ? d->rate * 0.6 + r * 0.4 : r;
        d->rate_bytes = d->received;
        d->rate_t = now;
    }
    dl_dirty = 1;
}

static void dl_response(WebKitDownload *dl, GParamSpec *ps, gpointer data) {
    dlrec_t *d = (dlrec_t *)data;
    WebKitURIResponse *r = webkit_download_get_response(dl);
    (void)ps;
    if (r) d->total = webkit_uri_response_get_content_length(r);
    dl_dirty = 1;
}

static void dl_failed(WebKitDownload *dl, GError *err, gpointer data) {
    dlrec_t *d = (dlrec_t *)data;
    (void)dl;
    if (err && g_error_matches(err, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        d->state = DL_CANCELLED;
    } else {
        char msg[400];
        d->state = DL_FAILED;
        snprintf(d->error, sizeof d->error, "%s", err ? err->message : "error");
        snprintf(msg, sizeof msg, "Download of %s failed: %s", d->name, d->error);
        set_status(msg, 8000);
    }
    if (sheet.dl == d && popup == POP_SHEET) {          /* the page went while we asked */
        sheet.dl = 0;
        sheet_finish(0);
    }
    downloads_changed(0);
}

static void dl_finished(WebKitDownload *dl, gpointer data) {
    dlrec_t *d = (dlrec_t *)data;
    (void)dl;
    if (d->state == DL_RUNNING) {
        char msg[400];
        d->state = DL_DONE;
        if (d->received > d->total) d->total = d->received;
        snprintf(msg, sizeof msg, "Downloaded %s to %s", d->name, d->folder);
        set_status(msg, 6000);
        if (popup != POP_DOWNLOADS) dl_unseen = 1;
    }
    downloads_changed(0);
}

/* where to save: a sheet asks (the download waits), or, while another sheet
 * is open, the folder used last */
static gboolean dl_destination(WebKitDownload *dl, const char *suggested, gpointer data) {
    dlrec_t *d = (dlrec_t *)data;
    const char *base = suggested && suggested[0] ? suggested : "download";
    (void)dl;
    snprintf(d->suggested, sizeof d->suggested, "%s", base);
    snprintf(d->name, sizeof d->name, "%s", base);
    if (popup == POP_SHEET) {
        dl_save_to(d, dl_folder, base);
        return TRUE;
    }
    sheet_open(SH_SAVE, "Save download");
    sheet.dl = d;
    snprintf(sheet.field, sizeof sheet.field, "%s", base);
    sheet.field_all = 1;
    mkdir(DL_DIR, 0755);
    {
        struct stat st;
        sheet_go(stat(dl_folder, &st) == 0 && S_ISDIR(st.st_mode) ? dl_folder : DL_DIR);
    }
    compose();
    return TRUE;
}

static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer data) {
    dlrec_t *d = (dlrec_t *)g_malloc0(sizeof(dlrec_t));
    WebKitURIRequest *req = webkit_download_get_request(dl);
    (void)s; (void)data;
    if (ndl == DL_MAX) {                         /* the oldest that ended makes room, or the oldest */
        int k = 0;
        for (int i = 0; i < ndl; i++)
            if (dls[i]->state != DL_RUNNING && dls[i]->state != DL_ASKING) { k = i; break; }
        dl_remove(dls[k]);
    }
    d->dl = (WebKitDownload *)g_object_ref(dl);
    d->state = DL_ASKING;
    snprintf(d->uri, sizeof d->uri, "%s", req ? webkit_uri_request_get_uri(req) : "");
    snprintf(d->name, sizeof d->name, "%s", "download");
    dls[ndl++] = d;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(dl_destination), d);
    g_signal_connect(dl, "received-data", G_CALLBACK(dl_received), d);
    g_signal_connect(dl, "notify::response", G_CALLBACK(dl_response), d);
    g_signal_connect(dl, "failed", G_CALLBACK(dl_failed), d);
    g_signal_connect(dl, "finished", G_CALLBACK(dl_finished), d);
    set_status("Starting download...", 3000);
    downloads_changed(1);
}

/* ---- what pages ask for -------------------------------------------------------------------- */

static gboolean on_script_dialog(WebKitWebView *v, WebKitScriptDialog *dlg, gpointer d) {
    tab_t *t = tab_of_view(v);
    WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dlg);
    char title[200];
    const char *uri = webkit_web_view_get_uri(v), *host = uri ? strstr(uri, "://") : 0;
    (void)d;
    if (popup == POP_SHEET || !t) {                       /* one question at a time: no, to this one */
        if (type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) webkit_script_dialog_confirm_set_confirmed(dlg, TRUE);
        return TRUE;
    }
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == t && i != active) select_tab(i);
    if (host) {
        const char *end = strchr(host + 3, '/');
        snprintf(title, sizeof title, "%.*s says", end ? (int)(end - host - 3) : (int)strlen(host + 3), host + 3);
    } else {
        snprintf(title, sizeof title, "This page says");
    }
    sheet_open(type == WEBKIT_SCRIPT_DIALOG_ALERT ? SH_ALERT : type == WEBKIT_SCRIPT_DIALOG_CONFIRM ? SH_CONFIRM
               : type == WEBKIT_SCRIPT_DIALOG_PROMPT ? SH_PROMPT : SH_LEAVE, title);
    if (type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) {
        snprintf(sheet.title, sizeof sheet.title, "Leave this page?");
        snprintf(sheet.message, sizeof sheet.message, "Changes you made may not be saved.");
    } else {
        snprintf(sheet.message, sizeof sheet.message, "%s", webkit_script_dialog_get_message(dlg));
    }
    if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
        const char *def = webkit_script_dialog_prompt_get_default_text(dlg);
        snprintf(sheet.field, sizeof sheet.field, "%s", def ? def : "");
        sheet.field_all = 1;
    }
    sheet.dialog = webkit_script_dialog_ref(dlg);
    compose();
    return TRUE;
}

static gboolean on_file_chooser(WebKitWebView *v, WebKitFileChooserRequest *req, gpointer d) {
    struct stat st;
    (void)v; (void)d;
    if (popup == POP_SHEET) {
        webkit_file_chooser_request_cancel(req);
        return TRUE;
    }
    sheet_open(SH_OPEN, "Choose a file");
    sheet.chooser = (WebKitFileChooserRequest *)g_object_ref(req);
    sheet_go(stat(dl_folder, &st) == 0 ? dl_folder : "/home");
    compose();
    return TRUE;
}

static void option_closed(WebKitOptionMenu *m, gpointer d) {
    (void)d;
    if (popup != POP_MENU || menu.options != m) return;
    menu.options = 0;
    g_object_unref(m);
    {
        ic_rect_t b = popup_bounds();
        popup = POP_NONE;
        update_area(b.y, b.y + b.h);
    }
}

/* a <select>'s list */
static gboolean on_option_menu(WebKitWebView *v, WebKitOptionMenu *m, WebKitRectangle *rect, gpointer d) {
    tab_t *t = tab_of_view(v);
    guint n = webkit_option_menu_get_n_items(m);
    (void)d;
    if (!t || t != tabs[active] || popup == POP_SHEET) return FALSE;
    popup_close();
    menu.kind = MENU_OPTIONS;
    menu.n = 0;
    menu.hover = -1;
    menu.tab = t;
    for (guint i = 0; i < n && menu.n < MENU_MAX; i++) {
        WebKitOptionMenuItem *it = webkit_option_menu_get_item(m, i);
        const char *label = webkit_option_menu_item_get_label(it);
        int group = webkit_option_menu_item_is_group_label(it);
        char text[96];
        snprintf(text, sizeof text, "%s%s", webkit_option_menu_item_is_group_child(it) ? "  " : "", label ? label : "");
        menu_add(text, group ? A_GROUP : A_OPTION, webkit_option_menu_item_is_enabled(it));
        menu.item[menu.n - 1].opt = (int)i;
        if (webkit_option_menu_item_is_selected(it)) {
            menu.item[menu.n - 1].checked = 1;
            menu.hover = menu.n - 1;
        }
    }
    menu.options = (WebKitOptionMenu *)g_object_ref(m);
    g_signal_connect(m, "close", G_CALLBACK(option_closed), NULL);
    /* the element's place in the view (logical pixels) */
    menu_show(rect->x * S, TOP + rect->y * S, rect->width * S, rect->height * S, rect->width * S);
    if (popup != POP_MENU) {                         /* nothing to show */
        menu.options = 0;
        webkit_option_menu_close(m);
        g_object_unref(m);
    }
    return TRUE;
}

/* the right-click menu, for what is under the pointer */
static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *cm, WebKitHitTestResult *hit, gpointer d) {
    tab_t *t = tab_of_view(v);
    int link = webkit_hit_test_result_context_is_link(hit), image = webkit_hit_test_result_context_is_image(hit);
    int media = webkit_hit_test_result_context_is_media(hit), editable = webkit_hit_test_result_context_is_editable(hit);
    int selection = webkit_hit_test_result_context_is_selection(hit);
    double z = webkit_web_view_get_zoom_level(v);
    (void)cm; (void)d;
    if (getenv("WPE_CONSOLE")) fprintf(stderr, "icda-webkit context menu: link %d image %d media %d editable %d\n", link, image, media, editable);
    if (!t || t != tabs[active] || popup == POP_SHEET) return TRUE;
    popup_close();
    menu.kind = MENU_CONTEXT;
    menu.n = 0;
    menu.hover = -1;
    menu.tab = t;
    snprintf(menu.link, sizeof menu.link, "%s", link && webkit_hit_test_result_get_link_uri(hit) ? webkit_hit_test_result_get_link_uri(hit) : "");
    snprintf(menu.image, sizeof menu.image, "%s", image && webkit_hit_test_result_get_image_uri(hit) ? webkit_hit_test_result_get_image_uri(hit) : "");
    snprintf(menu.media, sizeof menu.media, "%s", media && webkit_hit_test_result_get_media_uri(hit) ? webkit_hit_test_result_get_media_uri(hit) : "");
    menu.px = (int)(mouse_x / S / (z > 0 ? z : 1));
    menu.py = (int)((mouse_y - TOP) / S / (z > 0 ? z : 1));
    if (menu.link[0]) {
        menu_add("Open link in new tab", A_OPEN_LINK_TAB, 1);
        menu_add("Copy link address", A_COPY_LINK, 1);
        menu_add("Save link as...", A_SAVE_LINK, 1);
        menu_add("", A_SEP, 0);
    }
    if (menu.image[0]) {
        menu_add("Open image in new tab", A_OPEN_IMAGE_TAB, 1);
        menu_add("Copy image address", A_COPY_IMAGE, 1);
        menu_add("Save image as...", A_SAVE_IMAGE, 1);
        menu_add("", A_SEP, 0);
    }
    if (media) {
        menu_add("Play / Pause", A_MEDIA_PLAY, 1);
        menu_add("Mute / Unmute", A_MEDIA_MUTE, 1);
        menu_add("Loop", A_MEDIA_LOOP, 1);
        menu_add("Show controls", A_MEDIA_CONTROLS, 1);
        menu_add("Save media as...", A_SAVE_MEDIA, menu.media[0] && strncmp(menu.media, "blob:", 5) != 0);
        menu_add("", A_SEP, 0);
    }
    if (editable) {
        menu_add("Cut", A_CUT, selection);
        menu_add("Copy", A_COPY, selection);
        menu_add("Paste", A_PASTE, 1);
        menu_add("Select all", A_SELECT_ALL, 1);
    } else if (selection) {
        menu_add("Copy", A_COPY, 1);
    }
    if (!menu.link[0] && !menu.image[0] && !media && !editable) {
        if (selection) menu_add("", A_SEP, 0);
        menu_add("Back", A_BACK, webkit_web_view_can_go_back(v));
        menu_add("Forward", A_FORWARD, webkit_web_view_can_go_forward(v));
        menu_add("Reload", A_RELOAD, 1);
        menu_add("", A_SEP, 0);
        menu_add("Save page as...", A_SAVE_PAGE, 1);
        menu_add("Find in page...", A_FIND, 1);
        menu_add("Select all", A_SELECT_ALL, 1);
    }
    menu_show(mouse_x, mouse_y, 0, 0, 180 * S);
    return TRUE;
}

static void main_menu(void) {
    tab_t *t = ntabs ? tabs[active] : 0;
    char zoom[64];
    if (popup == POP_MENU && menu.kind == MENU_MAIN) { popup_close(); return; }
    popup_close();
    if (popup != POP_NONE) return;
    menu.kind = MENU_MAIN;
    menu.n = 0;
    menu.hover = -1;
    menu.tab = t;
    menu_add("New tab", A_NEW_TAB, 1);
    menu_add("Reopen closed tab", A_REOPEN_TAB, nclosed > 0);
    menu_add("Downloads", A_DOWNLOADS, 1);
    menu_add("Find in page...", A_FIND, t != 0);
    menu_add("Save page as...", A_SAVE_PAGE, t && t->uri[0]);
    menu_add("", A_SEP, 0);
    menu_add("Zoom in", A_ZOOM_IN, t != 0);
    menu_add("Zoom out", A_ZOOM_OUT, t != 0);
    snprintf(zoom, sizeof zoom, "Actual size (now %d%%)", t ? (int)(webkit_web_view_get_zoom_level(t->view) * 100 + 0.5) : 100);
    menu_add(zoom, A_ZOOM_RESET, t != 0);
    menu_add("", A_SEP, 0);
    menu_add("Full screen (Esc leaves it)", A_FULLSCREEN, 1);
    menu_add(t && webkit_web_view_get_is_muted(t->view) ? "Unmute tab" : "Mute tab", A_MUTE_TAB, t != 0);
    {
        ic_rect_t b = tool_rect(1);
        menu_show(b.x + b.w - 260 * S, b.y, b.w, b.h, 260 * S);
    }
}

static void zoom_step(tab_t *t, int dir) {
    static const double steps[] = { 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 };
    double z = webkit_web_view_get_zoom_level(t->view);
    int n = (int)G_N_ELEMENTS(steps), i = 0;
    if (!dir) {
        z = 1.0;
    } else if (dir > 0) {
        while (i < n - 1 && steps[i] <= z + 0.001) i++;
        z = steps[i];
    } else {
        i = n - 1;
        while (i > 0 && steps[i] >= z - 0.001) i--;
        z = steps[i];
    }
    webkit_web_view_set_zoom_level(t->view, z);
    {
        char msg[48];
        snprintf(msg, sizeof msg, "Zoom %d%%", (int)(z * 100 + 0.5));
        set_status(msg, 1500);
    }
}

/* a media element at the menu's place: op runs with it as e */
static void media_js(tab_t *t, const char *op) {
    char *js = g_strdup_printf(
        "(function(){var x=%d,y=%d,e=document.elementFromPoint(x,y);"
        "while(e&&!(e instanceof HTMLMediaElement))e=e.parentElement;"
        "if(!e){var m=document.querySelectorAll('video,audio');for(var i=0;i<m.length;i++){var r=m[i].getBoundingClientRect();"
        "if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom){e=m[i];break;}}}"
        "if(e){%s}})()", menu.px, menu.py, op);
    webkit_web_view_evaluate_javascript(t->view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

static void reopen_tab(void) {
    if (!nclosed) return;
    nclosed--;
    add_tab(closed_uri[nclosed], NULL);
}

static void menu_run(int action) {
    tab_t *t = menu.tab;
    int ok = 0;
    for (int i = 0; i < ntabs; i++) ok |= tabs[i] == t;
    if (!ok) t = ntabs ? tabs[active] : 0;
    switch (action) {
    case A_NEW_TAB: add_tab(NULL, NULL); focus_addr(); return;
    case A_REOPEN_TAB: reopen_tab(); return;
    case A_DOWNLOADS: downloads_toggle(); return;
    case A_FULLSCREEN: set_fullscreen(1); return;
    case A_OPEN_LINK_TAB:
    case A_OPEN_IMAGE_TAB: {                         /* behind this one */
        int was = active;
        add_tab(action == A_OPEN_LINK_TAB ? menu.link : menu.image, NULL);
        select_tab(was);
        return;
    }
    case A_COPY_LINK: if (!clip_write(menu.link)) set_status("Could not copy", 3000); return;
    case A_COPY_IMAGE: if (!clip_write(menu.image)) set_status("Could not copy", 3000); return;
    }
    if (!t) return;
    switch (action) {
    case A_BACK: webkit_web_view_go_back(t->view); break;
    case A_FORWARD: webkit_web_view_go_forward(t->view); break;
    case A_RELOAD: webkit_web_view_reload(t->view); break;
    case A_SAVE_LINK: webkit_web_view_download_uri(t->view, menu.link); break;
    case A_SAVE_IMAGE: webkit_web_view_download_uri(t->view, menu.image); break;
    case A_SAVE_MEDIA: webkit_web_view_download_uri(t->view, menu.media); break;
    case A_SAVE_PAGE: if (t->uri[0]) webkit_web_view_download_uri(t->view, t->uri); break;
    case A_MEDIA_PLAY: media_js(t, "if(e.paused)e.play();else e.pause();"); break;
    case A_MEDIA_MUTE: media_js(t, "e.muted=!e.muted;"); break;
    case A_MEDIA_LOOP: media_js(t, "e.loop=!e.loop;"); break;
    case A_MEDIA_CONTROLS: media_js(t, "e.controls=!e.controls;"); break;
    case A_CUT: copy_selection(t); webkit_web_view_execute_editing_command(t->view, WEBKIT_EDITING_COMMAND_CUT); break;
    case A_COPY: copy_selection(t); webkit_web_view_execute_editing_command(t->view, WEBKIT_EDITING_COMMAND_COPY); break;
    case A_PASTE: if (!paste_into(t)) webkit_web_view_execute_editing_command(t->view, WEBKIT_EDITING_COMMAND_PASTE); break;
    case A_SELECT_ALL: webkit_web_view_execute_editing_command(t->view, WEBKIT_EDITING_COMMAND_SELECT_ALL); break;
    case A_FIND: find_open(); break;
    case A_ZOOM_IN: zoom_step(t, 1); break;
    case A_ZOOM_OUT: zoom_step(t, -1); break;
    case A_ZOOM_RESET: zoom_step(t, 0); break;
    case A_MUTE_TAB:
        webkit_web_view_set_is_muted(t->view, !webkit_web_view_get_is_muted(t->view));
        chrome_dirty = 1;
        break;
    }
}

/* a middle click on a link: the page opens in a new tab behind this one */
static gboolean on_policy(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) {
    (void)v; (void)d;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
        if (webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
            webkit_navigation_action_get_mouse_button(a) == 2) {
            WebKitURIRequest *req = webkit_navigation_action_get_request(a);
            const char *uri = req ? webkit_uri_request_get_uri(req) : 0;
            if (uri && *uri) {
                int was = active;
                char copy[2048];
                snprintf(copy, sizeof copy, "%s", uri);
                webkit_policy_decision_ignore(dec);
                add_tab(copy, NULL);
                select_tab(was);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void on_audio(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)v; (void)ps; (void)d;
    chrome_dirty = 1;
}

/* ---- full screen ----------------------------------------------------------------------------
 *
 * A page asking for it (a video's button, requestFullscreen) or the menu's
 * Full screen: the chrome goes and the page has the whole window (ICDA's
 * window manager has no full-screen windows; maximize the window for the
 * whole screen).  Escape leaves it. */

static void set_fullscreen(int on) {
    if (fullscreen == on) return;
    fullscreen = on;
    layout();
    for (int i = 0; i < ntabs; i++) set_sizes(tabs[i]);
    if (on) set_status("Press Esc to leave full screen", 2500);
    compose();
}

static gboolean fullscreen_done(gpointer data) {
    tab_t *t = (tab_t *)data;
    if (t == fs_tab) wpe_view_backend_dispatch_did_enter_fullscreen(t->wb);
    else wpe_view_backend_dispatch_did_exit_fullscreen(t->wb);
    return G_SOURCE_REMOVE;
}

static bool on_fullscreen(void *data, bool enable) {
    tab_t *t = (tab_t *)data;
    if (enable) {
        if (!ntabs || t != tabs[active]) return false;
        popup_close();
        fs_tab = t;
        set_fullscreen(1);
    } else {
        if (fs_tab == t) fs_tab = 0;
        set_fullscreen(0);
    }
    g_idle_add(fullscreen_done, t);
    return true;
}

static gboolean poll_window(gpointer data) {
    gui_msg_t m;
    (void)data;
    while (gui_poll_event(&m)) {
        switch (m.type) {
        case GUI_MSG_KEY_EVENT:
            note_input();
            if (m.key.pressed) feed_key(m.key.keycode);
            break;
        case GUI_MSG_MOUSE_EVENT:
            note_input();
            mouse(m.mouse.x, m.mouse.y, m.mouse.buttons, m.mouse.wheel);
            break;
        case GUI_MSG_RESIZE:
            if (popup == POP_MENU || popup == POP_DOWNLOADS) popup_close();   /* placed for the old size */
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
        status_dirty = 1;
    }
    if (kd.state == 1) {                     /* Escape alone: no sequence came with it */
        kd.state = 0;
        key(0xff1b, 0);
    }
    if (dl_dirty && now_ms() - dl_drawn >= 250) downloads_changed(0);   /* progress, four times a second */
    if (chrome_dirty) update_chrome();
    if (status_dirty) update_status();
    return G_SOURCE_CONTINUE;
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
    /* the GPU when ICDA's driver offers it and it draws correctly here;
     * Mesa's software renderer otherwise */
    {
        int want = 1;
        FILE *off;
        struct stat st;
        mkdir("/home/.cache", 0755);
        if (stat(SURFER_DISK_LOG, &st) == 0 && st.st_size > 256 * 1024 && (off = fopen(SURFER_DISK_LOG, "w")))
            fclose(off);                                             /* kept small */
        slog("---- Surfer starting");
        if (access("/dev/dri/renderD128", F_OK) != 0) {
            want = 0;
            reason("no GPU driver (Terminal: cat /dev/gpu)");
        } else if ((off = fopen(GPU_OFF_FLAG, "r"))) {
            /* the GPU failed earlier this session: the reason is in the flag */
            char why[200] = "";
            if (fgets(why, sizeof why, off)) why[strcspn(why, "\n")] = 0;
            fclose(off);
            want = 0;
            reason(why[0] ? why : "the GPU failed earlier this session");
        } else if (getenv("ICDA_WEBKIT_SOFTWARE")) {
            want = 0;
            reason("asked for (ICDA_WEBKIT_SOFTWARE)");
        } else if (access(GPU_RUN_FLAG, F_OK) == 0) {
            slog("the last GPU session did not end cleanly");
            reason("Surfer stopped unexpectedly while drawing on the GPU");
            unlink(GPU_RUN_FLAG);
            want = 0;
        }
        if (want && gpu_probe() && gpu_start()) {
            gpu_mode = 1;
            touch(GPU_RUN_FLAG);
            atexit(gpu_clean_exit);
        } else if (want) {
            reason("the GPU test failed");
        }
        if (!gpu_mode && want != 0) want = 0;
        if (!gpu_mode && gpu_reason[0] && access(GPU_OFF_FLAG, F_OK) != 0 && access("/dev/dri/renderD128", F_OK) == 0) {
            FILE *f = fopen(GPU_OFF_FLAG, "w");          /* the rest of the session stays in software */
            if (f) { fprintf(f, "%s\n", gpu_reason); fclose(f); }
        }
        /* the engine actually loaded (Terminal: cat /tmp/surfer.log) */
        slog("WebKit %u.%u.%u", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
        slog("drawing on the %s%s%s", gpu_mode ? "GPU" : "CPU (software)", gpu_mode ? "" : ": ", gpu_mode ? "" : gpu_reason);
        write_mode();
    }
    if (!gpu_mode) {
        env_default("LIBGL_ALWAYS_SOFTWARE", "1");
        env_default("GALLIUM_DRIVER", "llvmpipe");
        /* Skia paints the page with its own code for the processor, on half
         * of them, not through Mesa doing a GPU's work: as fast, and 2-7%
         * less memory (WebKit 2.54 on six pages; Mesa still puts the layers
         * together) */
        env_default("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
    }
    env_default("EGL_PLATFORM", "wayland");
    env_default("GSETTINGS_BACKEND", "memory");
    /* WebKit's processes allocate with mimalloc rather than musl's malloc,
     * which gives every large block back to the kernel when it is freed:
     * the compositor's buffers, made again each frame, meant an mmap, an
     * munmap and page faults every frame, and each enters ICDA's kernel
     * under its one lock.  WebKit 2.54 on the bench: 48% fewer system calls
     * and page faults at rest, 47% fewer while scrolling, 5-7% less
     * processor time, 5% more memory.  (Set before
     * WebKit starts its processes, which take this environment; added to
     * what LD_PRELOAD already has; SURFER_MALLOC=musl leaves it out.) */
    if (access(MIMALLOC, R_OK) == 0 && !(getenv("SURFER_MALLOC") && !strcmp(getenv("SURFER_MALLOC"), "musl"))) {
        const char *pre = getenv("LD_PRELOAD");
        if (!pre || !*pre) {
            setenv("LD_PRELOAD", MIMALLOC, 1);
        } else if (!strstr(pre, "libmimalloc")) {
            char *both = g_strdup_printf("%s %s", pre, MIMALLOC);
            setenv("LD_PRELOAD", both, 1);
            g_free(both);
        }
    }
    /* no PulseAudio server here: sound goes through icdasink, and probing
     * for PulseAudio devices only costs time.  libaom's AV1 decoder is left
     * out: it decodes on one processor at half VP9's speed (1080p at 30
     * frames a second takes 68% of a fast core, 60 frames a second more than
     * one), so with it sites such as YouTube send AV1 and the video stutters;
     * without it they send VP9 */
    env_default("GST_PLUGIN_FEATURE_RANK", "pulsesink:NONE,pulsesrc:NONE,pulsedeviceprovider:NONE,av1dec:NONE");
    /* the plugin registry made with the Linux root (tests/linux/mkroot.sh): with
     * none, GStreamer reads every plugin (seconds of disk reads) on first use */
    env_default("GST_REGISTRY", "/usr/lib/icda/gst-registry.bin");
    env_default("GST_REGISTRY_UPDATE", "no");
    /* fontconfig's rules (userspace/webkit/fonts.conf): the root's conf.d
     * folder holds only links, which a FAT root cannot, so without this
     * "serif" was not an alias of anything and pages with audio players
     * aborted the web process in WebKit's font code */
    if (access(FONTS_CONF, R_OK) == 0) env_default("FONTCONFIG_FILE", FONTS_CONF);

    if (gui_open_window("Surfer", 1100, 720) != 0) {
        fprintf(stderr, "icda-webkit: no desktop window\n");
        return 1;
    }
    layout();
    set_status("Starting WebKit...", 0);
    compose();

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (gpu_mode && !wpe_fdo_initialize_for_egl_display(egl_dpy)) restart_in_software(url, "WebKit could not take the GPU display");
    if (!gpu_mode && !wpe_fdo_initialize_shm()) {
        fprintf(stderr, "icda-webkit: WPEBackend-fdo shared memory mode failed\n");
        return 1;
    }
    memory_setup();
    settings = webkit_settings_new();
    /* the user agent is WebKit's own: a Safari version high enough that sites
     * do not turn it away, and WebKit's list of sites given another one */
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, getenv("WPE_CONSOLE") != NULL);
    webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    /* Back to a page from another site is instant because its web process is
     * kept (two pages for the whole browser, WebKit lets them go only when
     * their tab closes): up to a gigabyte for two heavy pages, a quarter of a
     * 4 GB machine.  Below 6 GB, going back loads the page again from the
     * disk cache instead. */
    if (ram_mb < 6144) webkit_settings_set_enable_page_cache(settings, FALSE);
    heavy_setup();
    {   /* cookies and site data where WebKit always kept them (logins stay);
         * the HTTP cache in memory (CACHE_DIR) */
        char *data = g_build_filename(g_get_user_data_dir(), "wpe", NULL);
        const char *cache = getenv("SURFER_CACHE_DIR") ? getenv("SURFER_CACHE_DIR") : CACHE_DIR;
        session = webkit_network_session_new(data, cache);
        slog("cache: %s (site data: %s)", cache, data);
        g_free(data);
    }
    g_signal_connect(session, "download-started", G_CALLBACK(on_download), NULL);
    /* Sound plays when a page starts it, as on other browsers once a page has
     * been used: WebKit's default (only muted videos start on their own)
     * refused Wikipedia's player, which starts its sound after loading the
     * player the click asked for. */
    policies = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);

    add_tab(url, NULL);
    if (getenv("WPE_EVAL")) g_timeout_add_seconds(getenv("WPE_EVAL_EVERY") ? (guint)atoi(getenv("WPE_EVAL_EVERY")) : 60, eval_tick, NULL);
    set_status("", 1);

    g_timeout_add(10, poll_window, NULL);
    g_timeout_add(MEM_CHECK_MS, memory_watch, NULL);
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
