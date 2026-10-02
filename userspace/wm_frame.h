/*
 * wm_frame.h - window frames for the ICDA window manager.
 *
 * A window's (x, y, w, h) is its client rect in screen space; the title
 * bar sits directly above it.  This module owns the frame geometry
 * (title bar, caption buttons, resize zones), draws the frame and its
 * shadow, and answers hit-tests - the compositor in wm.c only decides
 * *when* and *where* to draw.
 */
#ifndef USERSPACE_WM_FRAME_H
#define USERSPACE_WM_FRAME_H

#include "libicda.h"

#define WM_TITLE_H      IC_H_TITLEBAR
/* How far a frame's shadow can reach past its rect (damage margin). */
#define WM_SHADOW_REACH 64

typedef enum {
    WM_HIT_NONE = 0,
    WM_HIT_CLIENT,
    WM_HIT_TITLE,
    WM_HIT_CLOSE,
    WM_HIT_MINIMIZE,
    WM_HIT_MAXIMIZE,
    WM_HIT_RESIZE_L,
    WM_HIT_RESIZE_R,
    WM_HIT_RESIZE_B,
    WM_HIT_RESIZE_BL,
    WM_HIT_RESIZE_BR
} wm_hit_t;

typedef struct {
    int         x, y, w, h;   /* client rect */
    const char *title;
    int         focused;
    int         maximized;    /* square corners, no shadow */
    wm_hit_t    hover;        /* caption button under the pointer */
    wm_hit_t    pressed;      /* caption button held down */
} wm_frame_t;

/* Whole frame (title bar + client) in screen space. */
ic_rect_t wm_frame_rect(const wm_frame_t *f);
/* Screen area the frame may touch, shadow included. */
ic_rect_t wm_frame_damage_rect(int x, int y, int w, int h);
ic_rect_t wm_frame_button_rect(const wm_frame_t *f, wm_hit_t which);
float     wm_frame_radius(const wm_frame_t *f);

wm_hit_t  wm_frame_hit(const wm_frame_t *f, int mx, int my);
static inline int wm_hit_is_resize(wm_hit_t h) {
    return h >= WM_HIT_RESIZE_L && h <= WM_HIT_RESIZE_BR;
}

/* Two-layer elevation shadow; `opacity` scales it for fades. */
void wm_frame_draw_shadow(ic_canvas_t *c, const wm_frame_t *f, float opacity);

/* Title bar, caption buttons, hairlines and client content.  `px` is
 * the app's buffer (pw x ph, pitch pw); areas it does not cover are
 * filled with the window background. */
void wm_frame_draw(ic_canvas_t *c, const wm_frame_t *f,
                   const uint32_t *px, int pw, int ph);

#endif /* USERSPACE_WM_FRAME_H */
