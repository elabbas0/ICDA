










#include "ic_gfx.h"
#include "ic_mem.h"



static inline uint32_t ic_mix_px(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t ia = 255u - a;
    uint32_t rb = (s & 0xFF00FFu) * a + (d & 0xFF00FFu) * ia + 0x800080u;
    uint32_t g = (s & 0x00FF00u) * a + (d & 0x00FF00u) * ia + 0x008000u;
    rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
    g = ((g + ((g >> 8) & 0x00FF00u)) >> 8) & 0x00FF00u;
    return rb | g;
}

static inline float ic_clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

ic_color_t ic_color_with_alpha(ic_color_t c, uint32_t a8) {
    if (a8 > 255u) a8 = 255u;
    return (c & 0xFFFFFFu) | (a8 << 24);
}

ic_color_t ic_color_fade(ic_color_t c, float f) {
    return ic_color_with_alpha(c, (uint32_t)((float)IC_ALPHA(c) * ic_clamp01(f) + 0.5f));
}

ic_color_t ic_color_mix(ic_color_t a, ic_color_t b, float t) {
    uint32_t w = (uint32_t)(ic_clamp01(t) * 255.0f + 0.5f);
    uint32_t rgb = ic_mix_px(a & 0xFFFFFFu, b & 0xFFFFFFu, w);
    uint32_t al = (IC_ALPHA(a) * (255u - w) + IC_ALPHA(b) * w + 127u) / 255u;
    return (al << 24) | rgb;
}

ic_color_t ic_color_over(ic_color_t base, ic_color_t top) {
    return 0xFF000000u | ic_mix_px(base & 0xFFFFFFu, top & 0xFFFFFFu, IC_ALPHA(top));
}



ic_canvas_t ic_canvas_make(uint32_t *px, int w, int h) {
    ic_canvas_t c;
    c.px = px;
    c.w = w;
    c.h = h;
    c.clip_x = 0;
    c.clip_y = 0;
    c.clip_w = 0;
    c.clip_h = 0;
    c.scale = 1;
    return c;
}

void ic_canvas_set_clip(ic_canvas_t *c, int x, int y, int w, int h) {
    if (!c) return;
    if (c->scale > 1) {
        x *= c->scale;
        y *= c->scale;
        w *= c->scale;
        h *= c->scale;
    }
    c->clip_x = x;
    c->clip_y = y;
    c->clip_w = w > 0 ? w : 0;
    c->clip_h = h > 0 ? h : 0;
    
    if (c->clip_w == 0 || c->clip_h == 0) {
        c->clip_x = -1;
        c->clip_y = -1;
        c->clip_w = 1;
        c->clip_h = 1;
    }
}

void ic_canvas_clear_clip(ic_canvas_t *c) {
    if (!c) return;
    c->clip_x = c->clip_y = c->clip_w = c->clip_h = 0;
}

int ic_canvas_bounds(const ic_canvas_t *c, int *x0, int *y0, int *x1, int *y1) {
    int ax = 0, ay = 0, bx, by;
    if (!c || !c->px || c->w <= 0 || c->h <= 0 || c->w > 8192 || c->h > 8192) return 0;
    bx = c->w;
    by = c->h;
    if (c->clip_w > 0 && c->clip_h > 0) {
        if (c->clip_x > ax) ax = c->clip_x;
        if (c->clip_y > ay) ay = c->clip_y;
        if (c->clip_x + c->clip_w < bx) bx = c->clip_x + c->clip_w;
        if (c->clip_y + c->clip_h < by) by = c->clip_y + c->clip_h;
    }
    if (ax >= bx || ay >= by) return 0;
    *x0 = ax;
    *y0 = ay;
    *x1 = bx;
    *y1 = by;
    return 1;
}

void ic_canvas_push_clip(ic_canvas_t *c, int x, int y, int w, int h, ic_rect_t *saved) {
    int x0, y0, x1, y1;
    if (!c) return;
    if (saved) {
        saved->x = c->clip_x;
        saved->y = c->clip_y;
        saved->w = c->clip_w;
        saved->h = c->clip_h;
    }
    if (!ic_canvas_bounds(c, &x0, &y0, &x1, &y1)) return;
    if (c->scale > 1) {
        int s = c->scale;
        x *= s;
        y *= s;
        w *= s;
        h *= s;
        if (x > x0) x0 = x;
        if (y > y0) y0 = y;
        if (x + w < x1) x1 = x + w;
        if (y + h < y1) y1 = y + h;
        c->scale = 1;
        ic_canvas_set_clip(c, x0, y0, x1 - x0, y1 - y0);
        c->scale = s;
        return;
    }
    if (x > x0) x0 = x;
    if (y > y0) y0 = y;
    if (x + w < x1) x1 = x + w;
    if (y + h < y1) y1 = y + h;
    ic_canvas_set_clip(c, x0, y0, x1 - x0, y1 - y0);
}

void ic_canvas_pop_clip(ic_canvas_t *c, const ic_rect_t *saved) {
    if (!c || !saved) return;
    c->clip_x = saved->x;
    c->clip_y = saved->y;
    c->clip_w = saved->w;
    c->clip_h = saved->h;
}


static int ic_clip_rect(const ic_canvas_t *c, int x, int y, int w, int h,
                        int *x0, int *y0, int *x1, int *y1) {
    if (w <= 0 || h <= 0) return 0;
    if (!ic_canvas_bounds(c, x0, y0, x1, y1)) return 0;
    if (x > *x0) *x0 = x;
    if (y > *y0) *y0 = y;
    if (x + w < *x1) *x1 = x + w;
    if (y + h < *y1) *y1 = y + h;
    return *x0 < *x1 && *y0 < *y1;
}

static inline void ic_span(uint32_t *row, int x0, int x1, uint32_t rgb, uint32_t a) {
    if (a >= 255u) {
        for (int x = x0; x < x1; x++) row[x] = rgb;
    } else if (a > 0u) {
        for (int x = x0; x < x1; x++) row[x] = ic_mix_px(row[x], rgb, a);
    }
}

static inline void ic_plot(uint32_t *row, int x, uint32_t rgb, uint32_t a, float cov) {
    uint32_t k = (uint32_t)((float)a * cov + 0.5f);
    if (k >= 255u) row[x] = rgb;
    else if (k > 0u) row[x] = ic_mix_px(row[x], rgb, k);
}



void ic_gfx_fill(ic_canvas_t *c, int x, int y, int w, int h, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_fill(&d, x * s, y * s, w * s, h * s, color);
        return;
    }
    int x0, y0, x1, y1;
    uint32_t a = IC_ALPHA(color);
    if (a == 0 || !ic_clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        ic_span(c->px + (int64_t)py * c->w, x0, x1, color & 0xFFFFFFu, a);
    }
}

void ic_gfx_hline(ic_canvas_t *c, int x, int y, int w, ic_color_t color) {
    ic_gfx_fill(c, x, y, w, 1, color);
}

void ic_gfx_vline(ic_canvas_t *c, int x, int y, int h, ic_color_t color) {
    ic_gfx_fill(c, x, y, 1, h, color);
}


typedef struct {
    int   x, y, w, h;
    float tl, tr, br, bl;
} ic_rr_t;

static void ic_rr_make(ic_rr_t *g, int x, int y, int w, int h,
                       float tl, float tr, float br, float bl) {
    float lim = (float)(w < h ? w : h) * 0.5f;
    g->x = x;
    g->y = y;
    g->w = w;
    g->h = h;
    g->tl = tl < 0 ? 0 : (tl > lim ? lim : tl);
    g->tr = tr < 0 ? 0 : (tr > lim ? lim : tr);
    g->br = br < 0 ? 0 : (br > lim ? lim : br);
    g->bl = bl < 0 ? 0 : (bl > lim ? lim : bl);
}


static float ic_rr_cov(const ic_rr_t *g, float fx, float fy) {
    float l = (float)g->x, t = (float)g->y;
    float r = (float)(g->x + g->w), b = (float)(g->y + g->h);
    float rad, cx, cy, dx, dy, d;
    if (fx < l || fy < t || fx > r || fy > b) return 0.0f;
    if (fx < l + g->tl && fy < t + g->tl) {
        rad = g->tl; cx = l + rad; cy = t + rad;
    } else if (fx > r - g->tr && fy < t + g->tr) {
        rad = g->tr; cx = r - rad; cy = t + rad;
    } else if (fx > r - g->br && fy > b - g->br) {
        rad = g->br; cx = r - rad; cy = b - rad;
    } else if (fx < l + g->bl && fy > b - g->bl) {
        rad = g->bl; cx = l + rad; cy = b - rad;
    } else {
        return 1.0f;
    }
    dx = fx - cx;
    dy = fy - cy;
    d = ic_sqrtf(dx * dx + dy * dy);
    return ic_clamp01(rad - d + 0.5f);
}

static inline int ic_ceil_i(float v) {
    int i = (int)v;
    return (float)i < v ? i + 1 : i;
}


static void ic_rr_row_zones(const ic_rr_t *g, int py, int *lz, int *rz) {
    float fy = (float)py + 0.5f;
    float lr = 0.0f, rr = 0.0f;
    if (fy < (float)g->y + g->tl) lr = g->tl;
    else if (fy > (float)(g->y + g->h) - g->bl) lr = g->bl;
    if (fy < (float)g->y + g->tr) rr = g->tr;
    else if (fy > (float)(g->y + g->h) - g->br) rr = g->br;
    *lz = ic_ceil_i(lr);
    *rz = ic_ceil_i(rr);
}

static void ic_rr_fill(ic_canvas_t *c, const ic_rr_t *g, ic_color_t top, ic_color_t bottom) {
    int x0, y0, x1, y1;
    int gradient = top != bottom;
    if (!ic_clip_rect(c, g->x, g->y, g->w, g->h, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (int64_t)py * c->w;
        ic_color_t col = top;
        uint32_t rgb, a;
        int lz, rz, mid0, mid1;
        if (gradient) {
            col = ic_color_mix(top, bottom, g->h > 1 ? (float)(py - g->y) / (float)(g->h - 1) : 0.0f);
        }
        rgb = col & 0xFFFFFFu;
        a = IC_ALPHA(col);
        if (a == 0) continue;
        ic_rr_row_zones(g, py, &lz, &rz);
        mid0 = g->x + lz;
        mid1 = g->x + g->w - rz;
        if (mid0 > mid1) mid0 = mid1 = g->x + g->w / 2;
        for (int px = x0; px < x1 && px < mid0; px++) {
            ic_plot(row, px, rgb, a, ic_rr_cov(g, (float)px + 0.5f, (float)py + 0.5f));
        }
        ic_span(row, mid0 > x0 ? mid0 : x0, mid1 < x1 ? mid1 : x1, rgb, a);
        for (int px = mid1 > x0 ? mid1 : x0; px < x1; px++) {
            ic_plot(row, px, rgb, a, ic_rr_cov(g, (float)px + 0.5f, (float)py + 0.5f));
        }
    }
}

void ic_gfx_rrect4(ic_canvas_t *c, int x, int y, int w, int h,
                   float tl, float tr, float br, float bl, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_rrect4(&d, x * s, y * s, w * s, h * s, tl * s, tr * s, br * s, bl * s, color);
        return;
    }
    ic_rr_t g;
    if (IC_ALPHA(color) == 0 || w <= 0 || h <= 0) return;
    ic_rr_make(&g, x, y, w, h, tl, tr, br, bl);
    ic_rr_fill(c, &g, color, color);
}

void ic_gfx_rrect(ic_canvas_t *c, int x, int y, int w, int h, float r, ic_color_t color) {
    ic_gfx_rrect4(c, x, y, w, h, r, r, r, r, color);
}

void ic_gfx_rrect_gradient_v(ic_canvas_t *c, int x, int y, int w, int h, float r,
                             ic_color_t top, ic_color_t bottom) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_rrect_gradient_v(&d, x * s, y * s, w * s, h * s, r * s, top, bottom);
        return;
    }
    ic_rr_t g;
    if (w <= 0 || h <= 0) return;
    ic_rr_make(&g, x, y, w, h, r, r, r, r);
    ic_rr_fill(c, &g, top, bottom);
}

void ic_gfx_rrect4_stroke(ic_canvas_t *c, int x, int y, int w, int h,
                          float tl, float tr, float br, float bl,
                          float width, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_rrect4_stroke(&d, x * s, y * s, w * s, h * s, tl * s, tr * s, br * s, bl * s,
                             width * s, color);
        return;
    }
    ic_rr_t outer, inner;
    int x0, y0, x1, y1;
    int iw = (int)(width + 0.5f);
    uint32_t rgb = color & 0xFFFFFFu;
    uint32_t a = IC_ALPHA(color);
    int band_top, band_bot;
    if (a == 0 || w <= 0 || h <= 0 || iw <= 0) return;
    ic_rr_make(&outer, x, y, w, h, tl, tr, br, bl);
    if (w <= 2 * iw || h <= 2 * iw) {
        ic_rr_fill(c, &outer, color, color);
        return;
    }
    ic_rr_make(&inner, x + iw, y + iw, w - 2 * iw, h - 2 * iw,
               outer.tl - (float)iw, outer.tr - (float)iw,
               outer.br - (float)iw, outer.bl - (float)iw);
    if (!ic_clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    {
        float rt = outer.tl > outer.tr ? outer.tl : outer.tr;
        float rb = outer.bl > outer.br ? outer.bl : outer.br;
        band_top = y + (ic_ceil_i(rt) > iw ? ic_ceil_i(rt) : iw);
        band_bot = y + h - (ic_ceil_i(rb) > iw ? ic_ceil_i(rb) : iw);
    }
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (int64_t)py * c->w;
        float fy = (float)py + 0.5f;
        if (py < band_top || py >= band_bot) {
            for (int px = x0; px < x1; px++) {
                float fx = (float)px + 0.5f;
                float cov = ic_rr_cov(&outer, fx, fy) - ic_rr_cov(&inner, fx, fy);
                if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
            }
        } else {
            int lz, rz, zl, zr;
            ic_rr_row_zones(&outer, py, &lz, &rz);
            zl = x + (lz > iw ? lz : iw);
            zr = x + w - (rz > iw ? rz : iw);
            for (int px = x0; px < x1 && px < zl; px++) {
                float fx = (float)px + 0.5f;
                float cov = ic_rr_cov(&outer, fx, fy) - ic_rr_cov(&inner, fx, fy);
                if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
            }
            for (int px = zr > x0 ? zr : x0; px < x1; px++) {
                float fx = (float)px + 0.5f;
                float cov = ic_rr_cov(&outer, fx, fy) - ic_rr_cov(&inner, fx, fy);
                if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
            }
        }
    }
}

void ic_gfx_rrect_stroke(ic_canvas_t *c, int x, int y, int w, int h, float r,
                         float width, ic_color_t color) {
    ic_gfx_rrect4_stroke(c, x, y, w, h, r, r, r, r, width, color);
}

static void ic_bbox_iter_bounds(const ic_canvas_t *c, float minx, float miny, float maxx,
                                float maxy, int *x0, int *y0, int *x1, int *y1, int *ok) {
    int bx = (int)minx - 1, by = (int)miny - 1;
    int bw = (int)maxx + 2 - bx, bh = (int)maxy + 2 - by;
    *ok = ic_clip_rect(c, bx, by, bw, bh, x0, y0, x1, y1);
}

void ic_gfx_circle(ic_canvas_t *c, float cx, float cy, float r, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_circle(&d, cx * s, cy * s, r * s, color);
        return;
    }
    int x0, y0, x1, y1, ok;
    uint32_t rgb = color & 0xFFFFFFu, a = IC_ALPHA(color);
    if (a == 0 || r <= 0.0f) return;
    ic_bbox_iter_bounds(c, cx - r, cy - r, cx + r, cy + r, &x0, &y0, &x1, &y1, &ok);
    if (!ok) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (int64_t)py * c->w;
        float dy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx;
            float cov = ic_clamp01(r - ic_sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
        }
    }
}

void ic_gfx_ring(ic_canvas_t *c, float cx, float cy, float r, float width, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_ring(&d, cx * s, cy * s, r * s, width * s, color);
        return;
    }
    int x0, y0, x1, y1, ok;
    uint32_t rgb = color & 0xFFFFFFu, a = IC_ALPHA(color);
    if (a == 0 || r <= 0.0f || width <= 0.0f) return;
    ic_bbox_iter_bounds(c, cx - r, cy - r, cx + r, cy + r, &x0, &y0, &x1, &y1, &ok);
    if (!ok) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (int64_t)py * c->w;
        float dy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx;
            float d = ic_sqrtf(dx * dx + dy * dy);
            float cov = ic_clamp01(r - d + 0.5f) - ic_clamp01(r - width - d + 0.5f);
            if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
        }
    }
}

void ic_gfx_line(ic_canvas_t *c, float ax, float ay, float bx, float by,
                 float width, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_line(&d, ax * s, ay * s, bx * s, by * s, width * s, color);
        return;
    }
    int x0, y0, x1, y1, ok;
    uint32_t rgb = color & 0xFFFFFFu, a = IC_ALPHA(color);
    float hw = width * 0.5f;
    float vx = bx - ax, vy = by - ay;
    float len2 = vx * vx + vy * vy;
    float minx = (ax < bx ? ax : bx) - hw, maxx = (ax > bx ? ax : bx) + hw;
    float miny = (ay < by ? ay : by) - hw, maxy = (ay > by ? ay : by) + hw;
    if (a == 0 || width <= 0.0f) return;
    ic_bbox_iter_bounds(c, minx, miny, maxx, maxy, &x0, &y0, &x1, &y1, &ok);
    if (!ok) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (int64_t)py * c->w;
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f;
            float t = len2 > 0.0f ? ((fx - ax) * vx + (fy - ay) * vy) / len2 : 0.0f;
            float qx, qy, d, cov;
            t = ic_clamp01(t);
            qx = ax + vx * t - fx;
            qy = ay + vy * t - fy;
            d = ic_sqrtf(qx * qx + qy * qy);
            cov = ic_clamp01(hw - d + 0.5f);
            if (cov > 0.0f) ic_plot(row, px, rgb, a, cov);
        }
    }
}

void ic_gfx_gradient_v(ic_canvas_t *c, int x, int y, int w, int h,
                       ic_color_t top, ic_color_t bottom) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_gradient_v(&d, x * s, y * s, w * s, h * s, top, bottom);
        return;
    }
    int x0, y0, x1, y1;
    if (!ic_clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        ic_color_t col = ic_color_mix(top, bottom, h > 1 ? (float)(py - y) / (float)(h - 1) : 0.0f);
        ic_span(c->px + (int64_t)py * c->w, x0, x1, col & 0xFFFFFFu, IC_ALPHA(col));
    }
}



void ic_gfx_blit(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                 int pitch, uint32_t opacity) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_blit_scaled4(&d, x * s, y * s, sw * s, sh * s, src, sw, sh, pitch, 0, 0, 0, 0,
                            opacity);
        return;
    }
    int x0, y0, x1, y1;
    if (!src || opacity == 0 || !ic_clip_rect(c, x, y, sw, sh, &x0, &y0, &x1, &y1)) return;
    if (opacity > 255u) opacity = 255u;
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        const uint32_t *s = src + (int64_t)(py - y) * pitch - x;
        if (opacity == 255u) {
            for (int px = x0; px < x1; px++) d[px] = s[px] & 0xFFFFFFu;
        } else {
            for (int px = x0; px < x1; px++) d[px] = ic_mix_px(d[px], s[px] & 0xFFFFFFu, opacity);
        }
    }
}

void ic_gfx_blit_rrect4(ic_canvas_t *c, int x, int y, const uint32_t *src, int sw, int sh,
                        int pitch, float tl, float tr, float br, float bl, uint32_t opacity) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_blit_scaled4(&d, x * s, y * s, sw * s, sh * s, src, sw, sh, pitch,
                            tl * s, tr * s, br * s, bl * s, opacity);
        return;
    }
    ic_rr_t g;
    int x0, y0, x1, y1;
    if (!src || opacity == 0 || !ic_clip_rect(c, x, y, sw, sh, &x0, &y0, &x1, &y1)) return;
    if (opacity > 255u) opacity = 255u;
    ic_rr_make(&g, x, y, sw, sh, tl, tr, br, bl);
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        const uint32_t *s = src + (int64_t)(py - y) * pitch - x;
        int lz, rz, mid0, mid1;
        ic_rr_row_zones(&g, py, &lz, &rz);
        mid0 = x + lz;
        mid1 = x + sw - rz;
        for (int px = x0; px < x1; px++) {
            uint32_t k = opacity;
            if (px < mid0 || px >= mid1) {
                float cov = ic_rr_cov(&g, (float)px + 0.5f, (float)py + 0.5f);
                k = (uint32_t)((float)opacity * cov + 0.5f);
                if (k == 0) continue;
            }
            d[px] = k >= 255u ? (s[px] & 0xFFFFFFu) : ic_mix_px(d[px], s[px] & 0xFFFFFFu, k);
        }
    }
}

static inline uint32_t ic_bilerp(const uint32_t *src, int sw, int sh, int pitch,
                                 int32_t fx, int32_t fy) {
    
    int ix = fx >> 16, iy = fy >> 16;
    uint32_t wx = (uint32_t)(fx & 0xFFFF) >> 8, wy = (uint32_t)(fy & 0xFFFF) >> 8;
    int ix1 = ix + 1, iy1 = iy + 1;
    uint32_t p00, p10, p01, p11, top, bot;
    if (ix < 0) { ix = 0; wx = 0; }
    if (iy < 0) { iy = 0; wy = 0; }
    if (ix >= sw - 1) { ix = sw - 1; ix1 = ix; }
    if (iy >= sh - 1) { iy = sh - 1; iy1 = iy; }
    if (ix1 >= sw) ix1 = sw - 1;
    if (iy1 >= sh) iy1 = sh - 1;
    p00 = src[(int64_t)iy * pitch + ix];
    p10 = src[(int64_t)iy * pitch + ix1];
    p01 = src[(int64_t)iy1 * pitch + ix];
    p11 = src[(int64_t)iy1 * pitch + ix1];
    top = ic_mix_px(p00 & 0xFFFFFFu, p10 & 0xFFFFFFu, wx > 255u ? 255u : wx);
    bot = ic_mix_px(p01 & 0xFFFFFFu, p11 & 0xFFFFFFu, wx > 255u ? 255u : wx);
    return ic_mix_px(top, bot, wy > 255u ? 255u : wy);
}

void ic_gfx_blit_scaled4(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                         const uint32_t *src, int sw, int sh, int pitch,
                         float tl, float tr, float br, float bl, uint32_t opacity) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_blit_scaled4(&d, dx * s, dy * s, dw * s, dh * s, src, sw, sh, pitch,
                            tl * s, tr * s, br * s, bl * s, opacity);
        return;
    }
    ic_rr_t g;
    int x0, y0, x1, y1;
    int32_t stepx, stepy;
    if (!src || sw <= 0 || sh <= 0 || opacity == 0) return;
    if (!ic_clip_rect(c, dx, dy, dw, dh, &x0, &y0, &x1, &y1)) return;
    if (opacity > 255u) opacity = 255u;
    if (dw == sw && dh == sh) {
        ic_gfx_blit_rrect4(c, dx, dy, src, sw, sh, pitch, tl, tr, br, bl, opacity);
        return;
    }
    ic_rr_make(&g, dx, dy, dw, dh, tl, tr, br, bl);
    stepx = (int32_t)(((int64_t)sw << 16) / dw);
    stepy = (int32_t)(((int64_t)sh << 16) / dh);
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        int32_t fy = (int32_t)(((int64_t)(py - dy) * stepy) + (stepy >> 1) - 0x8000);
        int lz, rz, mid0, mid1;
        ic_rr_row_zones(&g, py, &lz, &rz);
        mid0 = dx + lz;
        mid1 = dx + dw - rz;
        for (int px = x0; px < x1; px++) {
            int32_t fx = (int32_t)(((int64_t)(px - dx) * stepx) + (stepx >> 1) - 0x8000);
            uint32_t k = opacity;
            if (px < mid0 || px >= mid1) {
                float cov = ic_rr_cov(&g, (float)px + 0.5f, (float)py + 0.5f);
                k = (uint32_t)((float)opacity * cov + 0.5f);
                if (k == 0) continue;
            }
            {
                uint32_t s = ic_bilerp(src, sw, sh, pitch, fx, fy);
                d[px] = k >= 255u ? s : ic_mix_px(d[px], s, k);
            }
        }
    }
}

void ic_gfx_blit_scaled(ic_canvas_t *c, int dx, int dy, int dw, int dh,
                        const uint32_t *src, int sw, int sh, int pitch,
                        float radius, uint32_t opacity) {
    ic_gfx_blit_scaled4(c, dx, dy, dw, dh, src, sw, sh, pitch, radius, radius, radius, radius,
                        opacity);
}

void ic_gfx_image_rgba(ic_canvas_t *c, int x, int y, int dw, int dh,
                       const uint8_t *rgba, int sw, int sh, uint32_t opacity) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_image_rgba(&d, x * s, y * s, dw * s, dh * s, rgba, sw, sh, opacity);
        return;
    }
    int x0, y0, x1, y1;
    int32_t stepx, stepy;
    if (!rgba || sw <= 0 || sh <= 0 || opacity == 0) return;
    if (!ic_clip_rect(c, x, y, dw, dh, &x0, &y0, &x1, &y1)) return;
    if (opacity > 255u) opacity = 255u;
    stepx = (int32_t)(((int64_t)sw << 16) / dw);
    stepy = (int32_t)(((int64_t)sh << 16) / dh);
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        int32_t fy = (int32_t)(((int64_t)(py - y) * stepy) + (stepy >> 1) - 0x8000);
        int iy = fy >> 16;
        uint32_t wy = (uint32_t)(fy & 0xFFFF) >> 8;
        int iy1;
        if (fy < 0) { iy = 0; wy = 0; }
        iy1 = iy + 1 < sh ? iy + 1 : sh - 1;
        if (iy >= sh) iy = sh - 1;
        for (int px = x0; px < x1; px++) {
            int32_t fx = (int32_t)(((int64_t)(px - x) * stepx) + (stepx >> 1) - 0x8000);
            int ix = fx >> 16;
            uint32_t wx = (uint32_t)(fx & 0xFFFF) >> 8;
            int ix1;
            uint32_t acc[4] = {0, 0, 0, 0};
            const uint8_t *p[4];
            uint32_t wt[4];
            if (fx < 0) { ix = 0; wx = 0; }
            ix1 = ix + 1 < sw ? ix + 1 : sw - 1;
            if (ix >= sw) ix = sw - 1;
            p[0] = rgba + ((int64_t)iy * sw + ix) * 4;
            p[1] = rgba + ((int64_t)iy * sw + ix1) * 4;
            p[2] = rgba + ((int64_t)iy1 * sw + ix) * 4;
            p[3] = rgba + ((int64_t)iy1 * sw + ix1) * 4;
            wt[0] = (256u - wx) * (256u - wy);
            wt[1] = wx * (256u - wy);
            wt[2] = (256u - wx) * wy;
            wt[3] = wx * wy;
            

            for (int k = 0; k < 4; k++) {
                uint32_t al = p[k][3];
                acc[0] += p[k][0] * al * (wt[k] >> 8);
                acc[1] += p[k][1] * al * (wt[k] >> 8);
                acc[2] += p[k][2] * al * (wt[k] >> 8);
                acc[3] += al * (wt[k] >> 8);
            }
            if (acc[3] == 0) continue;
            {
                uint32_t r = acc[0] / acc[3], gg = acc[1] / acc[3], b = acc[2] / acc[3];
                uint32_t al = (acc[3] >> 8) * opacity / 255u;
                uint32_t rgb = (r << 16) | (gg << 8) | b;
                if (al >= 255u) d[px] = rgb;
                else if (al > 0u) d[px] = ic_mix_px(d[px], rgb, al);
            }
        }
    }
}

static void ic_mask_scaled(ic_canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh,
                           int pitch, int s, ic_color_t color) {
    int x0, y0, x1, y1;
    uint32_t rgb = color & 0xFFFFFFu, a = IC_ALPHA(color);
    if (!mask || a == 0 || !ic_clip_rect(c, x, y, mw * s, mh * s, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        const uint8_t *m = mask + (int64_t)((py - y) / s) * pitch;
        for (int px = x0; px < x1; px++) {
            uint32_t k = m[(px - x) / s];
            if (!k) continue;
            k = (k * a + 127u) / 255u;
            d[px] = k >= 255u ? rgb : ic_mix_px(d[px], rgb, k);
        }
    }
}

void ic_gfx_mask(ic_canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh,
                 int pitch, ic_color_t color) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_mask_scaled(&d, x * s, y * s, mask, mw, mh, pitch, s, color);
        return;
    }
    int x0, y0, x1, y1;
    uint32_t rgb = color & 0xFFFFFFu, a = IC_ALPHA(color);
    if (!mask || a == 0 || !ic_clip_rect(c, x, y, mw, mh, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        const uint8_t *m = mask + (int64_t)(py - y) * pitch - x;
        for (int px = x0; px < x1; px++) {
            uint32_t k = m[px];
            if (!k) continue;
            k = (k * a + 127u) / 255u;
            d[px] = k >= 255u ? rgb : ic_mix_px(d[px], rgb, k);
        }
    }
}




static void ic_box_pass(uint32_t *base, int n, int stride, int r, uint32_t *line) {
    uint32_t sr = 0, sg = 0, sb = 0;
    uint32_t div = (uint32_t)(2 * r + 1);
    
    uint32_t inv = (65536u + div / 2) / div;
    for (int i = 0; i < n; i++) line[i] = base[(int64_t)i * stride];
    for (int i = -r; i <= r; i++) {
        uint32_t p = line[i < 0 ? 0 : (i >= n ? n - 1 : i)];
        sr += (p >> 16) & 0xFF;
        sg += (p >> 8) & 0xFF;
        sb += p & 0xFF;
    }
    for (int i = 0; i < n; i++) {
        uint32_t add, sub;
        int ia = i + r + 1, is = i - r;
        base[(int64_t)i * stride] = (((sr * inv) >> 16) << 16) | (((sg * inv) >> 16) << 8) |
                                    ((sb * inv) >> 16);
        add = line[ia >= n ? n - 1 : ia];
        sub = line[is < 0 ? 0 : is];
        sr += ((add >> 16) & 0xFF) - ((sub >> 16) & 0xFF);
        sg += ((add >> 8) & 0xFF) - ((sub >> 8) & 0xFF);
        sb += (add & 0xFF) - (sub & 0xFF);
    }
}

static void ic_blur_buf(uint32_t *px, int w, int h, int pitch, int radius,
                        uint32_t *line) {
    int r = radius / 2;
    if (r < 1) r = 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) ic_box_pass(px + (int64_t)y * pitch, w, 1, r, line);
        for (int x = 0; x < w; x++) ic_box_pass(px + x, h, pitch, r, line);
    }
}

void ic_gfx_blur(ic_canvas_t *c, int x, int y, int w, int h, int radius,
                 uint32_t *scratch, int scratch_len) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_blur(&d, x * s, y * s, w * s, h * s, radius * s, scratch, scratch_len);
        return;
    }
    int x0, y0, x1, y1;
    if (radius <= 0 || !scratch) return;
    if (!ic_clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    if (scratch_len < (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0)) return;
    ic_blur_buf(c->px + (int64_t)y0 * c->w + x0, x1 - x0, y1 - y0, c->w, radius, scratch);
}



#define IC_SHADOW_SLOTS    6
#define IC_SHADOW_TILE_MAX 136

typedef struct {
    int     radius_q;  
    int     blur;
    int     extent;    
    int     size;      
    uint8_t alpha[IC_SHADOW_TILE_MAX * IC_SHADOW_TILE_MAX];
} ic_shadow_tile_t;

static ic_shadow_tile_t ic_shadow_cache[IC_SHADOW_SLOTS];
static int ic_shadow_next;

static const ic_shadow_tile_t *ic_shadow_tile(float radius, int blur) {
    static uint32_t work[IC_SHADOW_TILE_MAX * IC_SHADOW_TILE_MAX];
    static uint32_t line[IC_SHADOW_TILE_MAX];
    int key = (int)(radius * 4.0f + 0.5f);
    ic_shadow_tile_t *t;
    int ext, size, rr;
    ic_rr_t g;
    for (int i = 0; i < IC_SHADOW_SLOTS; i++) {
        if (ic_shadow_cache[i].size && ic_shadow_cache[i].radius_q == key &&
            ic_shadow_cache[i].blur == blur) {
            return &ic_shadow_cache[i];
        }
    }
    ext = blur * 2;
    rr = ic_ceil_i(radius);
    size = ext + rr + ext;
    if (size > IC_SHADOW_TILE_MAX) {
        ext = (IC_SHADOW_TILE_MAX - rr) / 2;
        size = ext + rr + ext;
        if (ext < 1) return 0;
    }
    t = &ic_shadow_cache[ic_shadow_next];
    ic_shadow_next = (ic_shadow_next + 1) % IC_SHADOW_SLOTS;
    


    ic_rr_make(&g, ext, ext, size * 4, size * 4, radius, radius, radius, radius);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float cov = ic_rr_cov(&g, (float)x + 0.5f, (float)y + 0.5f);
            work[y * size + x] = (uint32_t)(cov * 255.0f + 0.5f);
        }
    }
    ic_blur_buf(work, size, size, size, blur, line);
    for (int i = 0; i < size * size; i++) t->alpha[i] = (uint8_t)(work[i] & 0xFF);
    t->radius_q = key;
    t->blur = blur;
    t->extent = ext;
    t->size = size;
    return t;
}

void ic_gfx_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                   int blur, int dy, uint32_t alpha) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_shadow(&d, x * s, y * s, w * s, h * s, radius * s, blur * s, dy * s, alpha);
        return;
    }
    const ic_shadow_tile_t *t;
    int ext, size, sx, sy, sw, sh;
    int x0, y0, x1, y1;
    float rclamp;
    if (w <= 0 || h <= 0 || alpha == 0 || blur <= 0) return;
    rclamp = radius;
    if (rclamp * 2.0f > (float)w) rclamp = (float)w * 0.5f;
    if (rclamp * 2.0f > (float)h) rclamp = (float)h * 0.5f;
    t = ic_shadow_tile(rclamp, blur);
    if (!t) return;
    ext = t->extent;
    size = t->size;
    sx = x - ext;
    sy = y + dy - ext;
    sw = w + 2 * ext;
    sh = h + 2 * ext;
    if (!ic_clip_rect(c, sx, sy, sw, sh, &x0, &y0, &x1, &y1)) return;
    {
        

        int rr = ic_ceil_i(rclamp);
        for (int py = y0; py < y1; py++) {
            uint32_t *row = c->px + (int64_t)py * c->w;
            int v = py - sy;
            int ty = v < size ? v : (v >= sh - size ? sh - 1 - v : size - 1);
            int skip0 = x1, skip1 = x1;
            if (ty < 0) ty = 0;
            if (ty > size - 1) ty = size - 1;
            if (py >= y + rr && py < y + h - rr) {
                skip0 = x;
                skip1 = x + w;
            } else if (py >= y && py < y + h) {
                skip0 = x + rr;
                skip1 = x + w - rr;
            }
            if (skip1 < skip0) skip1 = skip0;
            for (int px = x0; px < x1; px++) {
                int u, tx;
                uint32_t k;
                if (px >= skip0 && px < skip1) {
                    px = skip1 - 1;
                    continue;
                }
                u = px - sx;
                tx = u < size ? u : (u >= sw - size ? sw - 1 - u : size - 1);
                if (tx < 0) tx = 0;
                if (tx > size - 1) tx = size - 1;
                k = ((uint32_t)t->alpha[ty * size + tx] * alpha + 127u) / 255u;
                if (k) row[px] = ic_mix_px(row[px], 0x000000u, k);
            }
        }
    }
}

/* Backdrop cache.  Glass surfaces (menus, the taskbar) sit over content that
 * rarely changes while they are open, but hover feedback repaints them many
 * times a second.  Each slot keeps the unblurred source and the blurred result
 * for one rectangle; a repaint compares only the pixels it is about to cover
 * with the stored source and re-blurs only when something underneath changed. */
#define IC_BD_SLOTS 4

typedef struct {
    int       x, y, w, h, blur, valid;
    uint32_t *src, *out;
    uint64_t  used;
} ic_bd_slot_t;

static ic_bd_slot_t ic_bd_slots[IC_BD_SLOTS];
static uint64_t     ic_bd_clock;

static void ic_bd_composite(ic_canvas_t *c, const ic_rr_t *g, const ic_bd_slot_t *s, int x, int w,
                            int x0, int y0, int x1, int y1) {
    for (int py = y0; py < y1; py++) {
        uint32_t *d = c->px + (int64_t)py * c->w;
        const uint32_t *row = s->out + (int64_t)(py - s->y) * s->w - s->x;
        int lz, rz, mid0, mid1;
        ic_rr_row_zones(g, py, &lz, &rz);
        mid0 = x + lz;
        mid1 = x + w - rz;
        for (int px = x0; px < x1; px++) {
            if (px < mid0 || px >= mid1) {
                float cov = ic_rr_cov(g, (float)px + 0.5f, (float)py + 0.5f);
                uint32_t k = (uint32_t)(cov * 255.0f + 0.5f);
                if (k) d[px] = k >= 255u ? row[px] : ic_mix_px(d[px], row[px], k);
            } else {
                d[px] = row[px];
            }
        }
    }
}

/* Returns 1 if the clip region (x0..x1, y0..y1) was painted from the cache. */
static int ic_backdrop_cached(ic_canvas_t *c, const ic_rr_t *g, int x, int y, int w, int h, int blur,
                              int x0, int y0, int x1, int y1, uint32_t *scratch, int scratch_len) {
    int fx0 = x < 0 ? 0 : x, fy0 = y < 0 ? 0 : y;
    int fx1 = x + w > c->w ? c->w : x + w, fy1 = y + h > c->h ? c->h : y + h;
    int fw = fx1 - fx0, fh = fy1 - fy0, changed = 0;
    ic_bd_slot_t *s = 0;
    if (fw <= 0 || fh <= 0 || scratch_len < (fw > fh ? fw : fh)) return 0;
    for (int i = 0; i < IC_BD_SLOTS; i++) {
        ic_bd_slot_t *t = &ic_bd_slots[i];
        if (t->src && t->x == fx0 && t->y == fy0 && t->w == fw && t->h == fh && t->blur == blur) { s = t; break; }
    }
    if (!s) {
        /* a new rectangle can only be cached once it is painted in full */
        if (x0 > fx0 || y0 > fy0 || x1 < fx1 || y1 < fy1) return 0;
        s = &ic_bd_slots[0];
        for (int i = 1; i < IC_BD_SLOTS; i++) {
            if (!ic_bd_slots[i].src || ic_bd_slots[i].used < s->used) s = &ic_bd_slots[i];
        }
        if (s->src) ic_free(s->src);
        if (s->out) ic_free(s->out);
        s->src = (uint32_t *)ic_malloc((uint64_t)fw * (uint64_t)fh * 4u);
        s->out = (uint32_t *)ic_malloc((uint64_t)fw * (uint64_t)fh * 4u);
        if (!s->src || !s->out) {
            if (s->src) ic_free(s->src);
            if (s->out) ic_free(s->out);
            s->src = s->out = 0;
            return 0;
        }
        s->x = fx0; s->y = fy0; s->w = fw; s->h = fh; s->blur = blur; s->valid = 0;
    }
    s->used = ++ic_bd_clock;
    /* refresh the stored source where this paint covers it */
    for (int py = y0; py < y1; py++) {
        const uint32_t *src = c->px + (int64_t)py * c->w;
        uint32_t *keep = s->src + (int64_t)(py - fy0) * fw - fx0;
        for (int px = x0; px < x1; px++) {
            if (keep[px] != src[px]) { keep[px] = src[px]; changed = 1; }
        }
    }
    if (!s->valid || changed) {
        for (int64_t i = 0; i < (int64_t)fw * fh; i++) s->out[i] = s->src[i];
        ic_blur_buf(s->out, fw, fh, fw, blur, scratch);
        s->valid = 1;
    }
    ic_bd_composite(c, g, s, x, w, x0, y0, x1, y1);
    return 1;
}

void ic_gfx_backdrop(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     int blur, ic_color_t tint, uint32_t *scratch, int scratch_len) {
    if (c && c->scale > 1) {
        ic_canvas_t d = *c;
        int s = c->scale;
        d.scale = 1;
        ic_gfx_backdrop(&d, x * s, y * s, w * s, h * s, radius * s, blur * s, tint, scratch,
                        scratch_len);
        return;
    }
    int bx0, by0, bx1, by1;
    int x0, y0, x1, y1;
    ic_rr_t g;
    if (w <= 0 || h <= 0) return;
    if (!ic_clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    ic_rr_make(&g, x, y, w, h, radius, radius, radius, radius);
    

    {
        int m = 3 * (blur / 2 < 1 ? 1 : blur / 2) + 2;
        bx0 = x0 - m > x ? x0 - m : x;
        by0 = y0 - m > y ? y0 - m : y;
        bx1 = x1 + m < x + w ? x1 + m : x + w;
        by1 = y1 + m < y + h ? y1 + m : y + h;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > c->w) bx1 = c->w;
        if (by1 > c->h) by1 = c->h;
    }
    if (blur > 0 && scratch && ic_backdrop_cached(c, &g, x, y, w, h, blur, x0, y0, x1, y1, scratch, scratch_len)) {
        /* served from the cache */
    } else if (blur > 0 && scratch && bx1 > bx0 && by1 > by0) {
        int bw = bx1 - bx0, bh = by1 - by0;
        int need = bw * bh + (bw > bh ? bw : bh);
        if (scratch_len >= need) {
            uint32_t *img = scratch;
            uint32_t *line = scratch + bw * bh;
            for (int py = 0; py < bh; py++) {
                const uint32_t *s = c->px + (int64_t)(by0 + py) * c->w + bx0;
                for (int px = 0; px < bw; px++) img[py * bw + px] = s[px];
            }
            ic_blur_buf(img, bw, bh, bw, blur, line);
            for (int py = y0; py < y1; py++) {
                uint32_t *d = c->px + (int64_t)py * c->w;
                const uint32_t *s = img + (int64_t)(py - by0) * bw - bx0;
                int lz, rz, mid0, mid1;
                ic_rr_row_zones(&g, py, &lz, &rz);
                mid0 = x + lz;
                mid1 = x + w - rz;
                for (int px = x0; px < x1; px++) {
                    if (px < mid0 || px >= mid1) {
                        float cov = ic_rr_cov(&g, (float)px + 0.5f, (float)py + 0.5f);
                        uint32_t k = (uint32_t)(cov * 255.0f + 0.5f);
                        if (k) d[px] = k >= 255u ? s[px] : ic_mix_px(d[px], s[px], k);
                    } else {
                        d[px] = s[px];
                    }
                }
            }
        }
    }
    ic_rr_fill(c, &g, tint, tint);
}
