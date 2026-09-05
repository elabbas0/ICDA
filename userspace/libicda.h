/*
 * libicda.h - the ICDA userland library.
 *
 * This is the "proper userland" layer between applications and the raw
 * int 0x80 syscall ABI (see kernel/syscall/syscall.h and icda_sys.h).
 * Applications written against this header get:
 *   - string helpers          (ic_strlen, ic_strcat, ic_uint_to_str, ...)
 *   - a drawing canvas        (ic_canvas_t + ic_rect/ic_text/ic_gradient_*)
 *   - icons                   (ic_icon_t, .icn format, builtin set)
 *   - a UI theme              (ic_theme_t)
 *   - window chrome           (title bar, minimize/close buttons, hit tests)
 *   - stateless widgets       (ic_draw_button + ic_button_state)
 *   - an app skeleton         (ic_run_app: open window + event loop)
 *
 * Every app is a plain C program:
 *
 *     #include "libicda.h"
 *     int main(int argc, char **argv) { ... }
 *
 * linked against crt0.o + libicda.o and packaged as an ELF .app file.
 */
#ifndef USERSPACE_LIBICDA_H
#define USERSPACE_LIBICDA_H

#include <stddef.h>
#include <stdint.h>
#include "icda_sys.h"
#include "gui.h"      /* gui_open_window / gui_pixel_buffer / gui_flush ... */
#include "gui_proto.h"

/* ============================== strings ============================== */

uint64_t ic_strlen(const char *s);
int      ic_strcmp(const char *a, const char *b);
int      ic_streq(const char *a, const char *b);
char    *ic_strcpy(char *dst, const char *src, uint64_t cap);
char    *ic_strcat(char *dst, const char *src, uint64_t cap);
int      ic_strprefix(const char *s, const char *prefix);
char     ic_lower(char c);
void     ic_uint_to_str(uint64_t v, char *out, uint64_t cap);
int      ic_parse_uint(const char *s, uint64_t *out);

/* ============================== canvas =============================== */
/* A 32bpp (0xAARRGGBB) pixel surface. The WM and GUI apps both draw
 * through this abstraction: the WM into its screen back-buffer, apps
 * into their window buffer (gui_pixel_buffer). pitch == w always. */

typedef struct {
    uint32_t *px;
    int       w;
    int       h;
} ic_canvas_t;

void     ic_fill(ic_canvas_t *c, uint32_t color);
void     ic_rect(ic_canvas_t *c, int x, int y, int w, int h, uint32_t color);
void     ic_rect_r(ic_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color);
/* Soft drop shadow (quadratic alpha falloff band around a rounded rect). */
void     ic_draw_shadow(ic_canvas_t *c, int x, int y, int w, int h, int radius, uint32_t color);
void     ic_hline(ic_canvas_t *c, int x, int y, int len, uint32_t color);
void     ic_vline(ic_canvas_t *c, int x, int y, int len, uint32_t color);
void     ic_outline(ic_canvas_t *c, int x, int y, int w, int h, uint32_t color);
void     ic_outline_r(ic_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color);
void     ic_gradient_v(ic_canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void     ic_gradient_h(ic_canvas_t *c, int x, int y, int w, int h, uint32_t left, uint32_t right);
uint32_t ic_blend(uint32_t a, uint32_t b, int n, int d);
void     ic_blend_px(ic_canvas_t *c, int x, int y, uint32_t color);
void     ic_text(ic_canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg);
void     ic_text_clip(ic_canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg, int max_px);
int      ic_text_width(const char *s);

/* =============================== icons =============================== */
/* .icn format (straight-alpha RGBA, top-left origin):
 *   0   'I' 'C' 'D' 'A'
 *   4   u16 version (1)
 *   6   u16 width
 *   8   u16 height
 *   10  u16 reserved (0)
 *   12  width*height*4 bytes RGBA
 */

typedef struct {
    uint16_t       w;
    uint16_t       h;
    const uint8_t *rgba;
} ic_icon_t;

int  ic_icon_parse(const uint8_t *blob, uint64_t size, ic_icon_t *out);
int  ic_icon_valid(const ic_icon_t *icon);
/* Draw scaled to dw x dh with nearest-neighbor sampling + alpha blend. */
void ic_icon_draw(ic_canvas_t *c, int x, int y, int dw, int dh, const ic_icon_t *icon);

/* Load every *.ico in a folder into the runtime icon registry, keyed by the
 * file stem lowercased (folder.ico -> "folder").  The registry is checked
 * before the builtin set, so dropping folder.ico into /usr/share/icons
 * replaces the stock icon.  Returns 0 if at least one icon loaded. */
int ic_icon_load_folder(const char *dir);

/* Parse a classic BMP-compressed .ico blob and decode one image into
 * rgba_out (top-left origin RGBA, up to max_decode px on a side).  Picks
 * the largest entry that fits, so multi-size .ico files keep their best
 * quality.  PNG-compressed entries are skipped. */
int ic_ico_parse(const uint8_t *blob, uint64_t size, int max_decode,
                 ic_icon_t *out, uint8_t *rgba_out, uint64_t rgba_cap);

/* Folder registry first, then the builtin set (userspace/icon_data.h).
 * Returns 0 for unknown names. */
const ic_icon_t *ic_icon_builtin(const char *name);

/* =============================== theme =============================== */

/* Centralized design tokens — radius, spacing, shadow. */
#define IC_RADIUS_WINDOW  12
#define IC_RADIUS_PANEL   12
#define IC_RADIUS_BUTTON   8
#define IC_RADIUS_TILE     8
#define IC_RADIUS_MENU    12
#define IC_RADIUS_TASKBAR 10
#define IC_SHADOW_RADIUS   8
#define IC_SHADOW_COLOR   0x000000
#define IC_TASKBAR_H      42
#define IC_SPACE_XS        4
#define IC_SPACE_SM        8
#define IC_SPACE_MD       12
#define IC_SPACE_LG       16
#define IC_WALL_TOP       0x000F172A
#define IC_WALL_BOTTOM    0x001E293B
#define IC_WALL_ACCENT    0x000EA5E9

typedef struct {
    uint32_t title_top, title_bottom;
    uint32_t title_top_active, title_bottom_active;
    uint32_t border, border_active;
    uint32_t shadow;
    uint32_t taskbar_top, taskbar_bottom;
    uint32_t accent, accent_hi, accent_lo;
    uint32_t panel, panel_edge;
    uint32_t text, text_muted, text_on_accent;
    uint32_t wall_top, wall_bottom;
    uint32_t desktop_bg;
    uint32_t surface;
    uint32_t surface_hover;
    uint32_t surface_active;
} ic_theme_t;

const ic_theme_t *ic_theme_default(void);

/* =========================== window chrome =========================== */
/* A window's x/y is its client origin; the title bar sits above it.
 * These helpers draw the frame a WM composes around a client area and
 * answer hit-tests in screen coordinates. */

#define IC_TITLE_H     26
#define IC_BTN_W       18
#define IC_BTN_H       16
#define IC_BTN_MAX_OFF 72   /* x offsets of title-bar buttons from the right edge */
#define IC_BTN_MIN_OFF 48
#define IC_BTN_CLS_OFF 24
#define IC_ANIM_MAX    8

typedef struct {
    int      x, y;        /* client origin */
    int      w, h;        /* client size */
    int      focused;
    int      minimized;
    int      anim;        /* 0..IC_ANIM_MAX window open/restore animation */
    const char *title;
    int      hover_close; /* pointer over the close button (chrome hover) */
    int      hover_min;   /* pointer over the minimize button */
    int      hover_max;   /* pointer over the maximize/restore button */
} ic_window_t;

void ic_draw_chrome(ic_canvas_t *c, const ic_theme_t *t, const ic_window_t *win,
                    const ic_icon_t *icon_close, const ic_icon_t *icon_min,
                    const ic_icon_t *icon_max);
int  ic_hit_title(const ic_window_t *win, int mx, int my);
int  ic_hit_minimize(const ic_window_t *win, int mx, int my);
int  ic_hit_maximize(const ic_window_t *win, int mx, int my);
int  ic_hit_close(const ic_window_t *win, int mx, int my);
int  ic_hit_client(const ic_window_t *win, int mx, int my);

/* ============================== widgets ============================== */

typedef struct { int x, y, w, h; } ic_rect_t;

int ic_hit_rect(int mx, int my, ic_rect_t r);

typedef enum {
    IC_BTN_NORMAL = 0,
    IC_BTN_HOVER,
    IC_BTN_ACTIVE,     /* pressed */
    IC_BTN_DISABLED
} ic_btn_state_t;

void ic_draw_button(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t r,
                    const char *label, ic_btn_state_t state);
ic_btn_state_t ic_button_state(int enabled, int hover, int pressed);

/* ============================ menu / dialog / slider ================= */
/* Stateless primitives (hit-testing kept separate, like buttons): the
 * caller owns open/close/value state and redraws on change. */
#define IC_MENU_MAX_ITEMS 12

typedef struct {
    const char *items[IC_MENU_MAX_ITEMS];
    int count;
    int selected;   /* highlighted index, -1 for none */
} ic_menu_t;

int ic_menu_row_h(void);
int ic_menu_width(const ic_menu_t *m);
int ic_menu_height(const ic_menu_t *m);
void ic_menu_draw(ic_canvas_t *c, const ic_theme_t *t, int x, int y,
                  const ic_menu_t *m);
int ic_menu_hit(const ic_menu_t *m, int x, int y, int mx, int my);

void ic_dialog_draw(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t r,
                    const char *title, const char *body);

void ic_slider_draw(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t track,
                    int value, int vmin, int vmax);
int ic_slider_hit(ic_rect_t track, int mx, int my);
int ic_slider_value_from_x(ic_rect_t track, int vmin, int vmax, int mx);

/* ============================ app skeleton =========================== */
/* Runs the standard GUI app loop: opens a window, polls the event queue,
 * calls on_event() for every event (return 0 to exit), and calls on_draw()
 * after events and periodically so animations keep running. */

typedef int  (*ic_event_fn)(void *ud, const gui_msg_t *msg);
typedef void (*ic_draw_fn)(void *ud);

int ic_run_app(const char *title, int w, int h,
               ic_event_fn on_event, ic_draw_fn on_draw, void *ud);

#endif /* USERSPACE_LIBICDA_H */
