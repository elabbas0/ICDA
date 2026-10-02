/*
 * wm_shell.h - the ICDA shell surfaces drawn by the window manager:
 * wallpaper, desktop icons, taskbar, launcher and transient overlays.
 *
 * Everything here is stateless drawing plus geometry/hit-testing over
 * small models the compositor (wm.c) fills in each frame.  Layout lives
 * in exactly one place: the *_rect() helpers are used both to draw and
 * to hit-test.
 */
#ifndef USERSPACE_WM_SHELL_H
#define USERSPACE_WM_SHELL_H

#include "libicda.h"

#define WM_BAR_H IC_H_TASKBAR

/* ------------------------------------------------------------ apps */

typedef struct {
    const char *label;
    const char *icon;    /* ic_icon_builtin() name */
    const char *path;
} wm_app_t;

extern const wm_app_t wm_apps[];
extern const int      wm_app_count;

/* Icon for a window title (the app registry's label is the key). */
const ic_icon_t *wm_app_icon_for_title(const char *title);

/* ------------------------------------------------------------ wallpaper */

/* Paint the wallpaper for the current appearance over the canvas clip
 * (or the whole canvas).  Deterministic, so partial repaints match. */
void wm_wallpaper_paint(ic_canvas_t *c, int sw, int sh);

/* ------------------------------------------------------------ desktop icons */

#define WM_DESK_CELL_W  92
#define WM_DESK_CELL_H 100
#define WM_DESK_X0      16
#define WM_DESK_Y0      18

/* Cell origin for a grid position. */
static inline ic_rect_t wm_desk_cell_rect(int cx, int cy) {
    return ic_rect_make(WM_DESK_X0 + cx * WM_DESK_CELL_W, WM_DESK_Y0 + cy * WM_DESK_CELL_H,
                        WM_DESK_CELL_W, WM_DESK_CELL_H);
}
/* Hit area (icon + label) inside a cell. */
ic_rect_t wm_desk_icon_hit_rect(ic_rect_t cell);
void wm_desk_icon_draw(ic_canvas_t *c, ic_rect_t cell, const char *label,
                       const ic_icon_t *icon, int selected, float opacity);

/* ------------------------------------------------------------ taskbar */

typedef struct {
    const char      *title;
    const ic_icon_t *icon;
    int              focused;
    int              minimized;
} wm_task_t;

#define WM_BAR_NONE      (-1)
#define WM_BAR_LAUNCHER  (-2)
#define WM_BAR_STATUS    (-3)

typedef struct {
    const wm_task_t *tasks;
    int              count;
    int              launcher_open;
    int              hover;        /* task index or WM_BAR_* */
    int              pressed;
    const char      *time_text;    /* "14:05" or NULL */
    const char      *date_text;    /* "Mon 28 Sep" or NULL */
    const char      *audio_text;   /* now playing, or NULL */
} wm_bar_t;

ic_rect_t wm_bar_rect(int sw, int sh);
ic_rect_t wm_bar_launcher_rect(int sw, int sh);
ic_rect_t wm_bar_task_rect(int sw, int sh, int count, int index);
int       wm_bar_hit(int sw, int sh, const wm_bar_t *b, int mx, int my);
void      wm_bar_draw(ic_canvas_t *c, int sw, int sh, const wm_bar_t *b,
                      uint32_t *scratch, int scratch_len);

/* ------------------------------------------------------------ launcher */

#define WM_LAUNCH_NONE      (-1)
#define WM_LAUNCH_OUTSIDE   (-2)
#define WM_LAUNCH_SHUTDOWN  (-10)
#define WM_LAUNCH_RESTART   (-11)

ic_rect_t wm_launcher_rect(int sw, int sh);
/* App index, WM_LAUNCH_SHUTDOWN/RESTART, WM_LAUNCH_NONE (inside, on
 * nothing) or WM_LAUNCH_OUTSIDE. */
int  wm_launcher_hit(int sw, int sh, int mx, int my);
void wm_launcher_draw(ic_canvas_t *c, int sw, int sh, int hover,
                      uint32_t *scratch, int scratch_len);

/* ------------------------------------------------------------ overlays */

void wm_rubber_band_draw(ic_canvas_t *c, int x0, int y0, int x1, int y1);

/* Shutdown/restart curtain: t runs 0..1 as the scene fades out. */
void wm_power_overlay_draw(ic_canvas_t *c, int sw, int sh, float t, int restart);

/* Diagnostics panel (F12). */
void wm_debug_draw(ic_canvas_t *c, const char *const *lines, int count);

#endif /* USERSPACE_WM_SHELL_H */
