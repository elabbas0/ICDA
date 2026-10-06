








#ifndef USERSPACE_WM_FRAME_H
#define USERSPACE_WM_FRAME_H

#include "libicda.h"

#define WM_TITLE_H      IC_H_TITLEBAR

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
    WM_HIT_RESIZE_BR,
    WM_HIT_RESIZE_T,
    WM_HIT_RESIZE_TL,
    WM_HIT_RESIZE_TR
} wm_hit_t;

typedef struct {
    int         x, y, w, h;   
    const char *title;
    int         focused;
    int         maximized;    
    wm_hit_t    hover;        
    wm_hit_t    pressed;      
} wm_frame_t;


ic_rect_t wm_frame_rect(const wm_frame_t *f);

ic_rect_t wm_frame_damage_rect(int x, int y, int w, int h);
ic_rect_t wm_frame_button_rect(const wm_frame_t *f, wm_hit_t which);
float     wm_frame_radius(const wm_frame_t *f);

wm_hit_t  wm_frame_hit(const wm_frame_t *f, int mx, int my);
static inline int wm_hit_is_resize(wm_hit_t h) {
    return h >= WM_HIT_RESIZE_L && h <= WM_HIT_RESIZE_TR;
}


void wm_frame_draw_shadow(ic_canvas_t *c, const wm_frame_t *f, float opacity);




void wm_frame_draw(ic_canvas_t *c, const wm_frame_t *f,
                   const uint32_t *px, int pw, int ph);

#endif 
