/*
 * ic_theme.h - ICDA design tokens.
 *
 * One source of truth for colour, spacing, radii, control sizes, motion
 * and elevation.  Colours are *semantic* ("label", "separator",
 * "accent"), never literal: apps ask for the role and the palette
 * answers for the current appearance (dark or light) and accent.
 * See docs/DESIGN.md for how the roles are meant to be used.
 */
#ifndef USERSPACE_IC_THEME_H
#define USERSPACE_IC_THEME_H

#include <stdint.h>
#include "ic_gfx.h"
#include "ic_anim.h"

/* ------------------------------------------------------------ spacing */
/* 4 px grid.  Use the named steps; odd values are a smell. */
#define IC_SP_1   4
#define IC_SP_2   8
#define IC_SP_3  12
#define IC_SP_4  16
#define IC_SP_5  20
#define IC_SP_6  24
#define IC_SP_8  32

/* ------------------------------------------------------------ radii */
#define IC_R_WINDOW     10.0f   /* window frame */
#define IC_R_PANEL      12.0f   /* launcher, popovers, dialogs */
#define IC_R_MENU        8.0f   /* context menus */
#define IC_R_MENU_ITEM   5.0f   /* highlighted row inside a menu */
#define IC_R_GROUP       9.0f   /* grouped settings cards */
#define IC_R_CONTROL     6.0f   /* buttons, fields, segmented controls */
#define IC_R_ROW         6.0f   /* selected list/sidebar row */
#define IC_R_TILE       12.0f   /* desktop/launcher icon highlight */

/* ------------------------------------------------------------ sizes */
#define IC_H_TITLEBAR   34      /* window title bar */
#define IC_H_TASKBAR    48      /* shell bar */
#define IC_H_CONTROL    26      /* push button, field, popup */
#define IC_H_CONTROL_SM 22      /* compact controls in toolbars */
#define IC_H_ROW        28      /* list / sidebar row */
#define IC_H_ROW_TALL   44      /* grouped row with a subtitle */
#define IC_H_MENU_ITEM  24
#define IC_H_TOOLBAR    44      /* in-window toolbar strip */
#define IC_W_SIDEBAR   180
#define IC_TOGGLE_W     38
#define IC_TOGGLE_H     22
#define IC_ICON_SM      16
#define IC_ICON_MD      20
#define IC_ICON_LG      32
#define IC_ICON_XL      48

/* ------------------------------------------------------------ motion */
#define IC_DUR_INSTANT   80     /* hover / press feedback */
#define IC_DUR_FAST     140     /* menus, small fades */
#define IC_DUR_BASE     220     /* window open, toggles */
#define IC_DUR_SLOW     340     /* minimize, large moves */
#define IC_EASE_ENTER   IC_EASE_DECELERATE
#define IC_EASE_EXIT    IC_EASE_ACCELERATE
#define IC_EASE_MOVE    IC_EASE_EMPHASIZED
/* Springs: response (s), damping ratio. */
#define IC_SPRING_WINDOW_RESPONSE 0.34f
#define IC_SPRING_WINDOW_DAMPING  0.86f
#define IC_SPRING_SNAPPY_RESPONSE 0.24f
#define IC_SPRING_SNAPPY_DAMPING  0.92f

/* ------------------------------------------------------------ elevation */
typedef struct {
    int      blur;   /* spread */
    int      dy;     /* downward offset */
    uint32_t alpha;  /* peak opacity 0..255 */
} ic_shadow_spec_t;

typedef enum {
    IC_ELEV_CONTROL = 0,   /* raised controls, toggle knob */
    IC_ELEV_MENU,          /* menus, popovers, launcher */
    IC_ELEV_WINDOW,        /* focused window */
    IC_ELEV_WINDOW_IDLE,   /* unfocused window */
    IC_ELEV_COUNT
} ic_elevation_t;

/* Paint the two-layer shadow (tight contact + soft ambient) for a
 * rounded rect at the given elevation. */
void ic_theme_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     ic_elevation_t level);
/* Same, with the shadow's opacity scaled (0..1) for fades. */
void ic_theme_shadow_faded(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                           ic_elevation_t level, float opacity);

/* ------------------------------------------------------------ palette */

typedef enum {
    IC_ACCENT_BLUE = 0,
    IC_ACCENT_PURPLE,
    IC_ACCENT_PINK,
    IC_ACCENT_RED,
    IC_ACCENT_ORANGE,
    IC_ACCENT_GREEN,
    IC_ACCENT_GRAPHITE,
    IC_ACCENT_COUNT
} ic_accent_t;

typedef struct {
    int        dark;

    /* surfaces */
    ic_color_t desktop;          /* behind everything (no wallpaper) */
    ic_color_t window;           /* window content background */
    ic_color_t content;          /* inset content: lists, text areas */
    ic_color_t sidebar;          /* sidebars and source lists */
    ic_color_t group;            /* grouped rows (cards) */
    ic_color_t titlebar;         /* title bar and unified toolbars */
    ic_color_t material_menu;    /* tint over blur: menus, launcher */
    ic_color_t material_bar;     /* tint over blur: taskbar */

    /* controls */
    ic_color_t control;          /* push button face */
    ic_color_t control_hover;
    ic_color_t control_pressed;
    ic_color_t control_stroke;   /* 1 px edge of controls and fields */
    ic_color_t field;            /* text field face */
    ic_color_t toggle_off;       /* switch track when off */
    ic_color_t knob;             /* switch knob, slider thumb */
    ic_color_t fill_hover;       /* translucent row/icon hover */
    ic_color_t fill_pressed;
    ic_color_t fill_selected_idle; /* selection in an unfocused list */
    ic_color_t segment_track;    /* segmented control track */
    ic_color_t segment_selected; /* segmented control selected pill */
    ic_color_t scroller;         /* overlay scrollbar thumb */

    /* text */
    ic_color_t label;
    ic_color_t label_secondary;
    ic_color_t label_tertiary;
    ic_color_t label_disabled;
    ic_color_t label_on_accent;

    /* lines */
    ic_color_t separator;        /* hairlines between rows / sections */
    ic_color_t frame;            /* outer hairline around windows / menus */
    ic_color_t highlight;        /* inner top highlight on raised surfaces */
    ic_color_t group_stroke;     /* edge of grouped cards */
    ic_color_t bar_edge;         /* bottom edge of title bars / toolbars */

    /* accent and status */
    ic_color_t accent;
    ic_color_t accent_hover;
    ic_color_t accent_pressed;
    ic_color_t accent_soft;      /* tinted fills: selection behind text */
    ic_color_t focus_ring;
    ic_color_t success;
    ic_color_t warning;
    ic_color_t danger;
    ic_color_t close_hover;      /* title-bar close button hover */
} ic_palette_t;

/* Current palette (loads appearance/accent from the settings store the
 * first time).  The pointer stays valid; its contents change on reload. */
const ic_palette_t *ic_palette(void);
/* Re-read /cfg/icda-settings; returns 1 if the palette changed. */
int  ic_palette_reload(void);
/* Build a palette explicitly (Settings previews, tests). */
void ic_palette_build(ic_palette_t *p, int dark, ic_accent_t accent);
/* Swatch colour for an accent choice (Settings UI). */
ic_color_t ic_accent_swatch(ic_accent_t accent);

#endif /* USERSPACE_IC_THEME_H */
