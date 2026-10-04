

















#ifndef USERSPACE_IC_GFX_H
#define USERSPACE_IC_GFX_H

#include <stdint.h>



typedef struct { int x, y, w, h; } ic_rect_t;




typedef struct {
    uint32_t *px;
    int       w;
    int       h;
    int       clip_x, clip_y, clip_w, clip_h;
    int       scale;
} ic_canvas_t;

typedef uint32_t ic_color_t;

#define IC_RGB(hex)       ((ic_color_t)(0xFF000000u | ((uint32_t)(hex) & 0xFFFFFFu)))
#define IC_RGBA(hex, a8)  ((ic_color_t)(((uint32_t)(a8) << 24) | ((uint32_t)(hex) & 0xFFFFFFu)))
#define IC_ALPHA(c)       ((uint32_t)(c) >> 24)
#define IC_WHITE          IC_RGB(0xFFFFFF)
#define IC_BLACK          IC_RGB(0x000000)
#define IC_BLACK_A(a8)    IC_RGBA(0x000000, a8)
#define IC_WHITE_A(a8)    IC_RGBA(0xFFFFFF, a8)

ic_color_t ic_color_with_alpha(ic_color_t c, uint32_t a8);

ic_color_t ic_color_fade(ic_color_t c, float f);

ic_color_t ic_color_mix(ic_color_t a, ic_color_t b, float t);

ic_color_t ic_color_over(ic_color_t base, ic_color_t top);



ic_canvas_t ic_canvas_make(uint32_t *px, int w, int h);
void ic_canvas_set_clip(ic_canvas_t *c, int x, int y, int w, int h);

void ic_canvas_push_clip(ic_canvas_t *c, int x, int y, int w, int h, ic_rect_t *saved);
void ic_canvas_pop_clip(ic_canvas_t *c, const ic_rect_t *saved);
void ic_canvas_clear_clip(ic_canvas_t *c);


int  ic_canvas_bounds(const ic_canvas_t *c, int *x0, int *y0, int *x1, int *y1);

static inline float ic_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}



void ic_gfx_fill(ic_canvas_t *c, int x, int y, int w, int h, ic_color_t color);
void ic_gfx_hline(ic_canvas_t *c, int x, int y, int w, ic_color_t color);
void ic_gfx_vline(ic_canvas_t *c, int x, int y, int h, ic_color_t color);



void ic_gfx_rrect(ic_canvas_t *c, int x, int y, int w, int h, float r, ic_color_t color);

void ic_gfx_rrect4(ic_canvas_t *c, int x, int y, int w, int h,
                   float tl, float tr, float br, float bl, ic_color_t color);

void ic_gfx_rrect_stroke(ic_canvas_t *c, int x, int y, int w, int h, float r,
                         float width, ic_color_t color);
void ic_gfx_rrect4_stroke(ic_canvas_t *c, int x, int y, int w, int h,
                          float tl, float tr, float br, float bl,
                          float width, ic_color_t color);

void ic_gfx_circle(ic_canvas_t *c, float cx, float cy, float r, ic_color_t color);
void ic_gfx_ring(ic_canvas_t *c, float cx, float cy, float r, float width, ic_color_t color);

void ic_gfx_line(ic_canvas_t *c, float x0, float y0, float x1, float y1,
                 float width, ic_color_t color);

void ic_gfx_gradient_v(ic_canvas_t *c, int x, int y, int w, int h,
                       ic_color_t top, ic_color_t bottom);

void ic_gfx_rrect_gradient_v(ic_canvas_t *c, int x, int y, int w, int h, float r,
                             ic_color_t top, ic_color_t bottom);




void ic_gfx_blit(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                 int pitch, uint32_t opacity);


void ic_gfx_blit_rrect4(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                        int pitch, float tl, float tr, float br, float bl, uint32_t opacity);


void ic_gfx_blit_scaled(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                        const uint32_t *src, int sw, int sh, int pitch,
                        float radius, uint32_t opacity);

void ic_gfx_blit_scaled4(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                         const uint32_t *src, int sw, int sh, int pitch,
                         float tl, float tr, float br, float bl, uint32_t opacity);

void ic_gfx_image_rgba(ic_canvas_t *c, int x, int y, int dw, int dh,
                       const uint8_t *rgba, int sw, int sh, uint32_t opacity);

void ic_gfx_mask(ic_canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh,
                 int pitch, ic_color_t color);





void ic_gfx_blur(ic_canvas_t *c, int x, int y, int w, int h, int radius,
                 uint32_t *scratch, int scratch_len);





void ic_gfx_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                   int blur, int dy, uint32_t alpha);




void ic_gfx_backdrop(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     int blur, ic_color_t tint, uint32_t *scratch, int scratch_len);

#endif 
