/*
 * ic_gfx.h - the ICDA 2D rasteriser.
 *
 * Software rendering primitives shared by the window manager and apps:
 * clip-aware fills, anti-aliased rounded shapes and strokes, gradients,
 * opacity/scaled blits, separable blur, cached soft shadows and
 * translucent "material" backdrops.
 *
 * Pixels are 0x00RRGGBB (the alpha byte of a canvas pixel is ignored).
 * Colours passed to ic_gfx_* are 0xAARRGGBB with *straight* alpha where
 * 0xFF is opaque - build them with IC_RGB()/IC_RGBA() so a bare hex
 * literal is never mistaken for a transparent colour.
 *
 * Every ic_gfx_* call honours the canvas clip rectangle.  The window
 * manager composites damage regions by setting the clip to each region
 * and redrawing the scene into it, so primitives must never touch
 * pixels outside the clip.
 */
#ifndef USERSPACE_IC_GFX_H
#define USERSPACE_IC_GFX_H

#include <stdint.h>

/* ------------------------------------------------------------ types */

typedef struct { int x, y, w, h; } ic_rect_t;

/* A 32bpp surface.  pitch == w.  The clip rect is active when
 * clip_w > 0 && clip_h > 0; a zeroed clip means "whole canvas", so
 * canvases built with { px, w, h } keep working unchanged. */
typedef struct {
    uint32_t *px;
    int       w;
    int       h;
    int       clip_x, clip_y, clip_w, clip_h;
} ic_canvas_t;

typedef uint32_t ic_color_t;

#define IC_RGB(hex)       ((ic_color_t)(0xFF000000u | ((uint32_t)(hex) & 0xFFFFFFu)))
#define IC_RGBA(hex, a8)  ((ic_color_t)(((uint32_t)(a8) << 24) | ((uint32_t)(hex) & 0xFFFFFFu)))
#define IC_ALPHA(c)       ((uint32_t)(c) >> 24)

ic_color_t ic_color_with_alpha(ic_color_t c, uint32_t a8);
/* Scale a colour's alpha by f (0..1). */
ic_color_t ic_color_fade(ic_color_t c, float f);
/* Mix two colours in RGB and alpha: t=0 -> a, t=1 -> b. */
ic_color_t ic_color_mix(ic_color_t a, ic_color_t b, float t);
/* Composite `top` over opaque `base`; result is opaque. */
ic_color_t ic_color_over(ic_color_t base, ic_color_t top);

/* ------------------------------------------------------------ canvas */

ic_canvas_t ic_canvas_make(uint32_t *px, int w, int h);
void ic_canvas_set_clip(ic_canvas_t *c, int x, int y, int w, int h);
/* Intersect the current clip with a rect (for nested drawing). */
void ic_canvas_push_clip(ic_canvas_t *c, int x, int y, int w, int h, ic_rect_t *saved);
void ic_canvas_pop_clip(ic_canvas_t *c, const ic_rect_t *saved);
void ic_canvas_clear_clip(ic_canvas_t *c);
/* Effective drawable bounds (clip & canvas), as x0,y0 inclusive / x1,y1
 * exclusive.  Returns 0 when nothing is drawable. */
int  ic_canvas_bounds(const ic_canvas_t *c, int *x0, int *y0, int *x1, int *y1);

static inline float ic_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* ------------------------------------------------------------ shapes */

void ic_gfx_fill(ic_canvas_t *c, int x, int y, int w, int h, ic_color_t color);
void ic_gfx_hline(ic_canvas_t *c, int x, int y, int w, ic_color_t color);
void ic_gfx_vline(ic_canvas_t *c, int x, int y, int h, ic_color_t color);

/* Anti-aliased rounded rectangle.  Straight edges sit on pixel
 * boundaries (integer geometry); only the corner arcs are antialiased. */
void ic_gfx_rrect(ic_canvas_t *c, int x, int y, int w, int h, float r, ic_color_t color);
/* Per-corner radii: top-left, top-right, bottom-right, bottom-left. */
void ic_gfx_rrect4(ic_canvas_t *c, int x, int y, int w, int h,
                   float tl, float tr, float br, float bl, ic_color_t color);
/* Stroke `width` px wide, drawn inside the rect bounds. */
void ic_gfx_rrect_stroke(ic_canvas_t *c, int x, int y, int w, int h, float r,
                         float width, ic_color_t color);
void ic_gfx_rrect4_stroke(ic_canvas_t *c, int x, int y, int w, int h,
                          float tl, float tr, float br, float bl,
                          float width, ic_color_t color);

void ic_gfx_circle(ic_canvas_t *c, float cx, float cy, float r, ic_color_t color);
void ic_gfx_ring(ic_canvas_t *c, float cx, float cy, float r, float width, ic_color_t color);
/* Round-capped line (capsule) - the building block for vector glyphs. */
void ic_gfx_line(ic_canvas_t *c, float x0, float y0, float x1, float y1,
                 float width, ic_color_t color);

void ic_gfx_gradient_v(ic_canvas_t *c, int x, int y, int w, int h,
                       ic_color_t top, ic_color_t bottom);
/* Vertical gradient clipped to a rounded rect. */
void ic_gfx_rrect_gradient_v(ic_canvas_t *c, int x, int y, int w, int h, float r,
                             ic_color_t top, ic_color_t bottom);

/* ------------------------------------------------------------- blits */

/* Copy an opaque source (pitch in pixels) at `opacity` (0..255). */
void ic_gfx_blit(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                 int pitch, uint32_t opacity);
/* As ic_gfx_blit, but the source is masked to a rounded rect with the
 * given per-corner radii (window content under rounded chrome). */
void ic_gfx_blit_rrect4(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                        int pitch, float tl, float tr, float br, float bl, uint32_t opacity);
/* Bilinear scale of an opaque source into dst rect, with opacity and a
 * rounded mask radius (in destination pixels, all four corners). */
void ic_gfx_blit_scaled(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                        const uint32_t *src, int sw, int sh, int pitch,
                        float radius, uint32_t opacity);
/* Per-corner mask radii: top-left, top-right, bottom-right, bottom-left. */
void ic_gfx_blit_scaled4(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                         const uint32_t *src, int sw, int sh, int pitch,
                         float tl, float tr, float br, float bl, uint32_t opacity);
/* Blit a straight-alpha RGBA byte image (icons), scaled bilinearly. */
void ic_gfx_image_rgba(ic_canvas_t *c, int x, int y, int dw, int dh,
                       const uint8_t *rgba, int sw, int sh, uint32_t opacity);
/* Blend an 8-bit coverage mask tinted with `color`. */
void ic_gfx_mask(ic_canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh,
                 int pitch, ic_color_t color);

/* ------------------------------------------------------------ effects */

/* Approximate Gaussian blur (three box passes) of a region in place.
 * `scratch` must hold max(w, h) pixels. */
void ic_gfx_blur(ic_canvas_t *c, int x, int y, int w, int h, int radius,
                 uint32_t *scratch, int scratch_len);

/* Soft drop shadow for a rounded rect.  `blur` is the Gaussian-like
 * spread in px, `dy` a downward offset, `alpha` the peak opacity.
 * Only the region outside the shape is painted (opaque windows cover
 * the rest).  Shadow tiles are cached per (radius, blur). */
void ic_gfx_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                   int blur, int dy, uint32_t alpha);

/* Translucent backdrop: blur the pixels under a rounded rect, then lay a
 * tint over them.  `scratch` must hold w * h + max(w, h) pixels; with
 * too small a scratch the blur is skipped and only the tint is drawn. */
void ic_gfx_backdrop(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     int blur, ic_color_t tint, uint32_t *scratch, int scratch_len);

#endif /* USERSPACE_IC_GFX_H */
