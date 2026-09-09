/*
 * libicda.h - the ICDA userland library.
 *
 * This is the "proper userland" layer between applications and the raw
 * int 0x80 syscall ABI (see kernel/syscall/syscall.h and icda_sys.h).
 * Applications written against this header get:
 *   - memory helpers          (ic_memcpy, ic_memmove, ic_memset, ...)
 *   - string helpers          (ic_strlen, ic_strcat, ic_uint_to_str, ...)
 *   - extended string helpers (ic_strnlen, ic_strncpy, ic_strlcat, ...)
 *   - character classification (ic_is_digit, ic_is_space, ic_is_alpha)
 *   - UTF-8 helpers           (ic_utf8_len, ic_utf8_valid)
 *   - bounded formatting      (ic_snprintf_u64, ic_snprintf_hex, ic_ato_u64)
 *   - arena allocator         (ic_arena_t — caller-provided buffer, zero-alloc)
 *   - ring buffer             (ic_ring_u8_t — caller-provided buffer)
 *   - a drawing canvas        (ic_canvas_t + ic_rect/ic_text/ic_gradient_*)
 *   - icons                   (ic_icon_t, .icn format, builtin set)
 *   - a UI theme              (ic_theme_t)
 *   - window chrome           (title bar, minimize/close buttons, hit tests)
 *   - stateless widgets       (ic_draw_button + ic_button_state)
 *   - an app skeleton         (ic_run_app: open window + event loop)
 *   - version information     (ic_version.h, included below)
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
#include "ic_version.h"
#include "icda_sys.h"
#include "gui.h"      /* gui_open_window / gui_pixel_buffer / gui_flush ... */
#include "gui_proto.h"

/* ================================ memory ============================== */

/* Safe memory primitives.  All functions are NULL-safe: if both pointers
 * are NULL the operation succeeds silently; if only one is NULL the
 * function returns without writing.  All sizes are in bytes. */

/* Copy n bytes from src to dst.  Overlapping regions are NOT handled
 * safely — use ic_memmove for overlapping copies.  dst may be NULL only
 * if n is 0. */
void ic_memcpy(void *dst, const void *src, uint64_t n);

/* Copy n bytes from src to dst, safe for overlapping regions. */
void ic_memmove(void *dst, const void *src, uint64_t n);

/* Set n bytes at dst to value (only the low byte is used). */
void ic_memset(void *dst, int value, uint64_t n);

/* Compare n bytes.  Returns <0, 0, or >0 like memcmp. */
int ic_memcmp(const void *a, const void *b, uint64_t n);

/* Zero n bytes at dst. */
void ic_memzero(void *dst, uint64_t n);

/* ================================ strings ============================== */

uint64_t ic_strlen(const char *s);
int      ic_strcmp(const char *a, const char *b);
int      ic_streq(const char *a, const char *b);
char    *ic_strcpy(char *dst, const char *src, uint64_t cap);
char    *ic_strcat(char *dst, const char *src, uint64_t cap);
int      ic_strprefix(const char *s, const char *prefix);
char     ic_lower(char c);
void     ic_uint_to_str(uint64_t v, char *out, uint64_t cap);
int      ic_parse_uint(const char *s, uint64_t *out);

/* =========================== extended strings ========================== */

/* Return the length of s, but never scan past cap bytes. */
uint64_t ic_strnlen(const char *s, uint64_t cap);

/* Copy at most n characters from src to dst, NUL-terminate if cap allows.
 * Returns pointer to dst.  If src is NULL, dst is zero-filled (up to cap). */
char *ic_strncpy(char *dst, const char *src, uint64_t n, uint64_t cap);

/* Append src to dst (finding the NUL in dst first).  NUL-terminates if
 * cap allows.  Returns total length that would have been written
 * (excluding NUL) — like strlcat.  If cap is 0, returns src length. */
uint64_t ic_strlcat(char *dst, const char *src, uint64_t cap);

/* Format val as a decimal string into buf, NUL-terminate (if cap > 0).
 * Returns number of characters written (excluding the NUL). */
uint64_t ic_snprintf_u64(char *buf, uint64_t cap, uint64_t val);

/* Format val as a lowercase hex string into buf, NUL-terminate.
 * Returns number of characters written (excluding the NUL). */
uint64_t ic_snprintf_hex(char *buf, uint64_t cap, uint64_t val);

/* Parse a decimal string to uint64_t.  Returns 1 on success, 0 on
 * failure (empty string, non-digit character, or overflow clamped to
 * UINT64_MAX). */
int ic_ato_u64(const char *s, uint64_t *out);

/* ========================= character classification ==================== */

int ic_is_digit(char c);
int ic_is_space(char c);
int ic_is_alpha(char c);

/* ================================ UTF-8 ================================ */

/* Count the number of Unicode codepoints in s (NUL-terminated). */
uint64_t ic_utf8_len(const char *s);

/* Validate s as well-formed UTF-8.  Returns 1 if valid, 0 if not.
 * An empty string is considered valid.  Rejects overlong encodings,
 * surrogate halves, and codepoints above U+10FFFF. */
int ic_utf8_valid(const char *s);

/* ============================ arena allocator ========================== */

/* A bump/arena allocator over a caller-provided buffer.  No kernel
 * calls, no dynamic memory.  Individual frees are not supported;
 * use ic_arena_reset to reclaim the entire buffer. */

#define IC_ARENA_ALIGN_MAX 64

typedef struct {
    uint8_t  *buf;    /* base of the caller-owned buffer (may NOT be NULL) */
    uint64_t  cap;    /* total capacity in bytes */
    uint64_t  offset; /* next free byte (bump pointer) */
} ic_arena_t;

/* Initialise the arena over the given buffer.  buf may be NULL only if
 * cap is 0 (creating a permanently-full arena).  Returns 0 on success. */
int ic_arena_init(ic_arena_t *a, uint8_t *buf, uint64_t cap);

/* Allocate `size` bytes aligned to `align` (must be power of 2, >= 1).
 * Returns a pointer into the arena buffer, or NULL if there is not
 * enough room.  Never fails for size==0 (returns NULL by convention). */
void *ic_arena_alloc(ic_arena_t *a, uint64_t size, uint64_t align);

/* Reset the bump pointer — logically frees everything. */
void ic_arena_reset(ic_arena_t *a);

/* Bytes used so far. */
uint64_t ic_arena_used(const ic_arena_t *a);

/* Bytes remaining. */
uint64_t ic_arena_remaining(const ic_arena_t *a);

/* ============================= ring buffer ============================= */

/* A single-byte ring buffer (FIFO) over a caller-provided buffer.
 * The buffer capacity must be > 0 and is the maximum number of bytes
 * that can be stored (one slot is wasted to distinguish full from
 * empty).  So for a buffer of `cap` usable bytes the ring can hold
 * at most `cap - 1` bytes. */

typedef struct {
    uint8_t  *buf;   /* base of the caller-owned buffer (may NOT be NULL) */
    uint64_t  cap;   /* usable capacity (ring stores cap-1 bytes max) */
    uint64_t  head;  /* read index */
    uint64_t  tail;  /* write index */
} ic_ring_u8_t;

/* Initialise the ring over the given buffer.  cap must be >= 2.
 * Returns 0 on success, -1 on invalid parameters. */
int ic_ring_u8_init(ic_ring_u8_t *r, uint8_t *buf, uint64_t cap);

/* Push one byte.  Returns 0 on success, -1 if full. */
int ic_ring_u8_push(ic_ring_u8_t *r, uint8_t byte);

/* Pop one byte.  Returns 0 on success, -1 if empty. */
int ic_ring_u8_pop(ic_ring_u8_t *r, uint8_t *byte_out);

/* Number of bytes currently stored. */
uint64_t ic_ring_u8_count(const ic_ring_u8_t *r);

/* Free slots available for pushing. */
uint64_t ic_ring_u8_free_cap(const ic_ring_u8_t *r);

/* Empty the ring (reset head/tail/count to initial state). */
void ic_ring_u8_reset(ic_ring_u8_t *r);

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
