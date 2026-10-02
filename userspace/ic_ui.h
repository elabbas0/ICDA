/*
 * ic_ui.h - the ICDA control set.
 *
 * Stateless drawing for every standard control, built only from
 * ic_gfx primitives, ic_font styles and ic_theme tokens, so all apps and
 * the shell share one look.  The caller owns widget state (hover,
 * pressed, value) and passes it in each frame; animated values (a
 * toggle's knob position, a row's hover fade) are plain floats the
 * caller drives with ic_tween_t / ic_spring_t.
 *
 * Hit-testing is ic_ui_hit(rect, x, y) against the same rects used to
 * draw, so layout lives in one place per app.
 */
#ifndef USERSPACE_IC_UI_H
#define USERSPACE_IC_UI_H

#include <stdint.h>
#include "ic_gfx.h"
#include "ic_font.h"
#include "ic_theme.h"

static inline int ic_ui_hit(ic_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline ic_rect_t ic_rect_make(int x, int y, int w, int h) {
    ic_rect_t r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static inline ic_rect_t ic_rect_inset(ic_rect_t r, int dx, int dy) {
    r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy;
    return r;
}

/* ------------------------------------------------------------ symbols */
/* A small vector symbol set drawn with antialiased strokes, sized by
 * the caller (16/20 px typical).  Symbols share one stroke weight per
 * size so they sit with Inter at matching sizes. */

typedef enum {
    IC_SYM_NONE = 0,
    IC_SYM_CLOSE,
    IC_SYM_MINIMIZE,
    IC_SYM_MAXIMIZE,
    IC_SYM_RESTORE,
    IC_SYM_CHEVRON_LEFT,
    IC_SYM_CHEVRON_RIGHT,
    IC_SYM_CHEVRON_DOWN,
    IC_SYM_CHEVRON_UP,
    IC_SYM_CHECK,
    IC_SYM_PLUS,
    IC_SYM_MINUS,
    IC_SYM_SEARCH,
    IC_SYM_RELOAD,
    IC_SYM_POWER,
    IC_SYM_RESTART,
    IC_SYM_PLAY,
    IC_SYM_PAUSE,
    IC_SYM_STOP,
    IC_SYM_SPEAKER,
    IC_SYM_GEAR,
    IC_SYM_GRID,
    IC_SYM_FOLDER,
    IC_SYM_DOCUMENT,
    IC_SYM_DISK,
    IC_SYM_TERMINAL,
    IC_SYM_GLOBE,
    IC_SYM_MUSIC,
    IC_SYM_ACTIVITY,
    IC_SYM_SUN,
    IC_SYM_MOON,
    IC_SYM_INFO,
    IC_SYM_WARNING,
    IC_SYM_TRASH,
    IC_SYM_COUNT
} ic_symbol_t;

/* Draw a symbol centred on (cx, cy) inside a size x size box. */
void ic_symbol_draw(ic_canvas_t *c, ic_symbol_t sym, float cx, float cy, float size,
                    ic_color_t color);

/* ------------------------------------------------------------ controls */

typedef enum {
    IC_STATE_NORMAL = 0,
    IC_STATE_HOVER,
    IC_STATE_PRESSED,
    IC_STATE_DISABLED
} ic_state_t;

/* Derive a state from pointer facts. */
ic_state_t ic_ui_state(int enabled, int hover, int pressed);

typedef enum {
    IC_BUTTON_DEFAULT = 0,  /* neutral push button */
    IC_BUTTON_PRIMARY,      /* the one default action: accent fill */
    IC_BUTTON_DESTRUCTIVE,  /* irreversible action: danger text */
    IC_BUTTON_PLAIN         /* borderless: toolbars, inline actions */
} ic_button_style_t;

void ic_ui_button(ic_canvas_t *c, ic_rect_t r, const char *label, ic_symbol_t sym,
                  ic_button_style_t style, ic_state_t state);
/* Width a button needs for its label (+ optional symbol). */
int  ic_ui_button_width(const char *label, ic_symbol_t sym);

/* Square borderless icon button (toolbar). */
void ic_ui_icon_button(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, ic_state_t state);

/* Switch.  `on` is the animated position 0..1 (drive with a tween of
 * IC_DUR_BASE); the knob slides and the track cross-fades. */
void ic_ui_toggle(ic_canvas_t *c, int x, int y, float on, ic_state_t state);
static inline ic_rect_t ic_ui_toggle_rect(int x, int y) {
    return ic_rect_make(x, y, IC_TOGGLE_W, IC_TOGGLE_H);
}

/* Segmented control; `slide` is the animated selection position
 * (float index) so the selection pill glides between segments. */
void ic_ui_segmented(ic_canvas_t *c, ic_rect_t r, const char *const *labels, int count,
                     float slide, int hover_index);
int  ic_ui_segmented_hit(ic_rect_t r, int count, int x, int y);

/* Horizontal slider, value 0..1. */
void ic_ui_slider(ic_canvas_t *c, ic_rect_t r, float value, ic_state_t state);
float ic_ui_slider_value(ic_rect_t r, int x);

/* Progress bar, value 0..1 (negative: indeterminate stripe at phase). */
void ic_ui_progress(ic_canvas_t *c, ic_rect_t r, float value, ic_color_t tint);

/* Single-line text field.  `scroll_px` is the horizontal text scroll
 * the caller keeps so the caret stays visible (use
 * ic_ui_textfield_scroll to update it). */
typedef struct {
    const char *text;
    int         cursor;        /* byte offset */
    int         sel_start;     /* == sel_end: no selection */
    int         sel_end;
    int         focused;
    int         caret_on;      /* blink phase */
    const char *placeholder;
    ic_symbol_t leading;       /* IC_SYM_SEARCH for search fields */
    int         scroll_px;
} ic_textfield_t;

void ic_ui_textfield(ic_canvas_t *c, ic_rect_t r, const ic_textfield_t *tf);
void ic_ui_textfield_scroll(ic_rect_t r, ic_textfield_t *tf);
/* Byte offset nearest to a click at x. */
int  ic_ui_textfield_index_at(ic_rect_t r, const ic_textfield_t *tf, int x);

/* Focus ring around a control shape. */
void ic_ui_focus_ring(ic_canvas_t *c, ic_rect_t r, float radius);

/* ------------------------------------------------------------ containers */

/* Window content background. */
void ic_ui_window_bg(ic_canvas_t *c, ic_rect_t r);

/* Grouped card (System-Settings style).  Rows inside are ic_ui_group_row
 * rects of equal height stacked from the card's top. */
void ic_ui_group(ic_canvas_t *c, ic_rect_t r);
/* Row chrome: separator above rows other than the first, hover fill
 * that respects the card's rounded corners. */
void ic_ui_group_row(ic_canvas_t *c, ic_rect_t row, int index, int count, float hover);
/* Title (+ optional subtitle) laid out at the row's leading edge. */
void ic_ui_row_text(ic_canvas_t *c, ic_rect_t row, ic_symbol_t sym, ic_color_t sym_tint,
                    const char *title, const char *subtitle);
/* Section header above a group. */
void ic_ui_section_header(ic_canvas_t *c, int x, int y, const char *text);

/* Source-list / sidebar item. */
void ic_ui_sidebar_bg(ic_canvas_t *c, ic_rect_t r);
void ic_ui_sidebar_item(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *label,
                        int selected, float hover);

/* Table/list row: selection pill (accent when the list is focused),
 * hover fill, zebra-free.  Returns the colour to use for row text. */
ic_color_t ic_ui_list_row(ic_canvas_t *c, ic_rect_t r, int selected, int list_focused,
                          float hover);
/* Column header strip for tables. */
void ic_ui_table_header(ic_canvas_t *c, ic_rect_t r, const char *const *titles,
                        const int *widths, int count);

/* Toolbar strip at the top of a window's content (with bottom hairline). */
void ic_ui_toolbar(ic_canvas_t *c, ic_rect_t r);
/* Status bar strip at the bottom of a window's content. */
void ic_ui_statusbar(ic_canvas_t *c, ic_rect_t r, const char *text);

/* Overlay scrollbar: a thin pill inside the right edge of `view`,
 * visible with `alpha` (0..1) so it can fade out when idle. */
void ic_ui_scrollbar(ic_canvas_t *c, ic_rect_t view, int offset, int content_h,
                     float alpha);
/* Thumb rect for hit-testing / dragging. */
ic_rect_t ic_ui_scrollbar_thumb(ic_rect_t view, int offset, int content_h);

/* Empty state: symbol, title and a short message centred in r. */
void ic_ui_empty_state(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                       const char *message);

/* ------------------------------------------------------------ menus */

#define IC_MENU_ITEMS_MAX 16
#define IC_MENU_SEPARATOR ((const char *)1)

typedef struct {
    const char *labels[IC_MENU_ITEMS_MAX];   /* IC_MENU_SEPARATOR for a divider */
    const char *shortcuts[IC_MENU_ITEMS_MAX];/* optional right-aligned hints */
    uint8_t     disabled[IC_MENU_ITEMS_MAX];
    int         count;
    int         hover;                        /* item under the pointer, -1 none */
} ic_menu_model_t;

int  ic_ui_menu_width(const ic_menu_model_t *m);
int  ic_ui_menu_height(const ic_menu_model_t *m);
/* Item index at (x, y) for a menu placed at (mx, my); -1 if none or a
 * separator/disabled item. */
int  ic_ui_menu_hit(const ic_menu_model_t *m, int mx, int my, int x, int y);
/* Draw the menu (material background, shadow, rows); scratch backs
 * the blur (see ic_gfx_backdrop).  Open/close fades are applied by the
 * caller compositing the menu layer (the WM does this). */
void ic_ui_menu(ic_canvas_t *c, const ic_menu_model_t *m, int mx, int my,
                uint32_t *scratch, int scratch_len);

/* Floating material panel (launcher, popovers, alerts). */
void ic_ui_panel(ic_canvas_t *c, ic_rect_t r, float radius, ic_elevation_t elevation,
                 uint32_t *scratch, int scratch_len);

/* Modal alert card: title, message, up to 2 buttons (right aligned,
 * last = primary).  Returns the rects of the drawn buttons. */
void ic_ui_alert(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                 const char *message, const char *const *buttons, int count,
                 int hover_button, ic_rect_t *button_rects);

#endif /* USERSPACE_IC_UI_H */
