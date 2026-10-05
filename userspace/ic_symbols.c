







#include "ic_ui.h"

#define SYM_MAX 64

typedef struct {
    uint8_t cov[SYM_MAX * SYM_MAX];
    int     n;        
    float   scale;    
    float   ox, oy;   
    float   stroke;   
} sym_mask_t;

static inline float sym_clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline void sym_put(sym_mask_t *m, int x, int y, float cov) {
    uint8_t v;
    if (x < 0 || y < 0 || x >= m->n || y >= m->n || cov <= 0.0f) return;
    v = (uint8_t)(sym_clamp01(cov) * 255.0f + 0.5f);
    if (v > m->cov[y * m->n + x]) m->cov[y * m->n + x] = v;
}

static inline void sym_erase(sym_mask_t *m, int x, int y, float cov) {
    if (x < 0 || y < 0 || x >= m->n || y >= m->n || cov <= 0.0f) return;
    m->cov[y * m->n + x] = (uint8_t)((float)m->cov[y * m->n + x] * (1.0f - sym_clamp01(cov)));
}


static inline float sx(const sym_mask_t *m, float u) { return m->ox + u * m->scale; }
static inline float sy(const sym_mask_t *m, float v) { return m->oy + v * m->scale; }

static void sym_seg_px(sym_mask_t *m, float ax, float ay, float bx, float by, float width) {
    float hw = width * 0.5f;
    float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    int x0 = (int)((ax < bx ? ax : bx) - hw) - 1, x1 = (int)((ax > bx ? ax : bx) + hw) + 2;
    int y0 = (int)((ay < by ? ay : by) - hw) - 1, y1 = (int)((ay > by ? ay : by) + hw) + 2;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float t = len2 > 0.0f ? ((fx - ax) * vx + (fy - ay) * vy) / len2 : 0.0f;
            float qx, qy;
            t = sym_clamp01(t);
            qx = ax + vx * t - fx;
            qy = ay + vy * t - fy;
            sym_put(m, x, y, hw - ic_sqrtf(qx * qx + qy * qy) + 0.5f);
        }
    }
}

static void sym_line(sym_mask_t *m, float ax, float ay, float bx, float by) {
    sym_seg_px(m, sx(m, ax), sy(m, ay), sx(m, bx), sy(m, by), m->stroke);
}

static void sym_line_w(sym_mask_t *m, float ax, float ay, float bx, float by, float wmul) {
    sym_seg_px(m, sx(m, ax), sy(m, ay), sx(m, bx), sy(m, by), m->stroke * wmul);
}

static void sym_poly(sym_mask_t *m, const float *pts, int n, int closed) {
    for (int i = 0; i + 1 < n; i++) {
        sym_line(m, pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2], pts[i * 2 + 3]);
    }
    if (closed && n > 2) sym_line(m, pts[(n - 1) * 2], pts[(n - 1) * 2 + 1], pts[0], pts[1]);
}


static void sym_disc(sym_mask_t *m, float cu, float cv, float r) {
    float cx = sx(m, cu), cy = sy(m, cv), rp = r * m->scale;
    for (int y = (int)(cy - rp) - 1; y <= (int)(cy + rp) + 1; y++) {
        for (int x = (int)(cx - rp) - 1; x <= (int)(cx + rp) + 1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            sym_put(m, x, y, rp - ic_sqrtf(dx * dx + dy * dy) + 0.5f);
        }
    }
}

static void sym_disc_erase(sym_mask_t *m, float cu, float cv, float r) {
    float cx = sx(m, cu), cy = sy(m, cv), rp = r * m->scale;
    for (int y = (int)(cy - rp) - 1; y <= (int)(cy + rp) + 1; y++) {
        for (int x = (int)(cx - rp) - 1; x <= (int)(cx + rp) + 1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            sym_erase(m, x, y, rp - ic_sqrtf(dx * dx + dy * dy) + 0.5f);
        }
    }
}


static void sym_ring(sym_mask_t *m, float cu, float cv, float r) {
    float cx = sx(m, cu), cy = sy(m, cv), rp = r * m->scale, hw = m->stroke * 0.5f;
    for (int y = (int)(cy - rp - hw) - 1; y <= (int)(cy + rp + hw) + 1; y++) {
        for (int x = (int)(cx - rp - hw) - 1; x <= (int)(cx + rp + hw) + 1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = ic_sqrtf(dx * dx + dy * dy) - rp;
            if (d < 0) d = -d;
            sym_put(m, x, y, hw - d + 0.5f);
        }
    }
}



static void sym_arc(sym_mask_t *m, float cu, float cv, float r, float a0, float a1) {
    
    int steps = 24;
    float da = (a1 - a0) / (float)steps;
    float c = 1.0f - da * da / 2.0f + da * da * da * da / 24.0f;
    float s = da - da * da * da / 6.0f + da * da * da * da * da / 120.0f;
    
    float ang = a0, px, py;
    float c0, s0;
    while (ang > 3.14159265f) ang -= 6.2831853f;
    while (ang < -3.14159265f) ang += 6.2831853f;
    {
        
        float h = ang * 0.5f, h2 = h * h;
        float ch = 1 - h2 / 2 + h2 * h2 / 24 - h2 * h2 * h2 / 720 + h2 * h2 * h2 * h2 / 40320;
        float shv = h * (1 - h2 / 6 + h2 * h2 / 120 - h2 * h2 * h2 / 5040 + h2 * h2 * h2 * h2 / 362880);
        c0 = ch * ch - shv * shv;
        s0 = 2 * ch * shv;
    }
    px = c0;
    py = s0;
    for (int i = 0; i < steps; i++) {
        float nx = px * c - py * s;
        float ny = px * s + py * c;
        sym_line(m, cu + px * r, cv + py * r, cu + nx * r, cv + ny * r);
        px = nx;
        py = ny;
    }
}


static void sym_fill_convex(sym_mask_t *m, const float *pts, int n) {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    float P[16];
    for (int i = 0; i < n && i < 8; i++) {
        P[i * 2] = sx(m, pts[i * 2]);
        P[i * 2 + 1] = sy(m, pts[i * 2 + 1]);
        if (P[i * 2] < minx) minx = P[i * 2];
        if (P[i * 2] > maxx) maxx = P[i * 2];
        if (P[i * 2 + 1] < miny) miny = P[i * 2 + 1];
        if (P[i * 2 + 1] > maxy) maxy = P[i * 2 + 1];
    }
    for (int y = (int)miny; y <= (int)maxy + 1; y++) {
        for (int x = (int)minx; x <= (int)maxx + 1; x++) {
            int hits = 0;
            for (int sy2 = 0; sy2 < 4; sy2++) {
                for (int sx2 = 0; sx2 < 4; sx2++) {
                    float fx = (float)x + 0.125f + 0.25f * (float)sx2;
                    float fy = (float)y + 0.125f + 0.25f * (float)sy2;
                    int pos = 0, neg = 0;
                    for (int i = 0; i < n; i++) {
                        int j = (i + 1) % n;
                        float cr = (P[j * 2] - P[i * 2]) * (fy - P[i * 2 + 1]) -
                                   (P[j * 2 + 1] - P[i * 2 + 1]) * (fx - P[i * 2]);
                        if (cr > 0) pos++; else if (cr < 0) neg++;
                    }
                    if (pos == 0 || neg == 0) hits++;
                }
            }
            sym_put(m, x, y, (float)hits / 16.0f);
        }
    }
}


static void sym_rrect(sym_mask_t *m, float x0, float y0, float x1, float y1, float r) {
    const float k = 1.5707963f;
    sym_line(m, x0 + r, y0, x1 - r, y0);
    sym_line(m, x1, y0 + r, x1, y1 - r);
    sym_line(m, x1 - r, y1, x0 + r, y1);
    sym_line(m, x0, y1 - r, x0, y0 + r);
    if (r > 0.0f) {
        sym_arc(m, x1 - r, y0 + r, r, -k, 0.0f);
        sym_arc(m, x1 - r, y1 - r, r, 0.0f, k);
        sym_arc(m, x0 + r, y1 - r, r, k, 2 * k);
        sym_arc(m, x0 + r, y0 + r, r, 2 * k, 3 * k);
    }
}


static void sym_globe_meridian(sym_mask_t *m) {
    const int steps = 28;
    float prevx = 0.0f, prevy = -0.36f;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;       
        float a = -1.5707963f + t * 3.1415926f;
        
        float a2 = a * a;
        float ca = 1 - a2 / 2 + a2 * a2 / 24 - a2 * a2 * a2 / 720 + a2 * a2 * a2 * a2 / 40320;
        float sa = a * (1 - a2 / 6 + a2 * a2 / 120 - a2 * a2 * a2 / 5040);
        float x = ca * 0.15f, y = sa * 0.36f;
        sym_line(m, prevx, prevy, x, y);
        sym_line(m, -prevx, prevy, -x, y);
        prevx = x;
        prevy = y;
    }
}

static void sym_build(sym_mask_t *m, ic_symbol_t sym) {
    const float PI = 3.14159265f;
    switch (sym) {
    case IC_SYM_CLOSE:
        sym_line(m, -0.26f, -0.26f, 0.26f, 0.26f);
        sym_line(m, 0.26f, -0.26f, -0.26f, 0.26f);
        break;
    case IC_SYM_MINIMIZE:
        sym_line(m, -0.30f, 0.0f, 0.30f, 0.0f);
        break;
    case IC_SYM_MAXIMIZE:
        sym_rrect(m, -0.29f, -0.29f, 0.29f, 0.29f, 0.07f);
        break;
    case IC_SYM_RESTORE:
        sym_rrect(m, -0.31f, -0.16f, 0.16f, 0.31f, 0.06f);
        sym_line(m, -0.16f, -0.31f, 0.25f, -0.31f);
        sym_line(m, 0.31f, -0.25f, 0.31f, 0.16f);
        sym_arc(m, 0.25f, -0.25f, 0.06f, -PI / 2, 0.0f);
        break;
    case IC_SYM_CHEVRON_LEFT: {
        const float p[] = { 0.10f, -0.30f, -0.14f, 0.0f, 0.10f, 0.30f };
        sym_poly(m, p, 3, 0);
        break;
    }
    case IC_SYM_CHEVRON_RIGHT: {
        const float p[] = { -0.10f, -0.30f, 0.14f, 0.0f, -0.10f, 0.30f };
        sym_poly(m, p, 3, 0);
        break;
    }
    case IC_SYM_CHEVRON_DOWN: {
        const float p[] = { -0.28f, -0.10f, 0.0f, 0.16f, 0.28f, -0.10f };
        sym_poly(m, p, 3, 0);
        break;
    }
    case IC_SYM_CHEVRON_UP: {
        const float p[] = { -0.28f, 0.10f, 0.0f, -0.16f, 0.28f, 0.10f };
        sym_poly(m, p, 3, 0);
        break;
    }
    case IC_SYM_CHECK: {
        const float p[] = { -0.30f, 0.02f, -0.09f, 0.24f, 0.31f, -0.25f };
        sym_poly(m, p, 3, 0);
        break;
    }
    case IC_SYM_PLUS:
        sym_line(m, -0.30f, 0.0f, 0.30f, 0.0f);
        sym_line(m, 0.0f, -0.30f, 0.0f, 0.30f);
        break;
    case IC_SYM_MINUS:
        sym_line(m, -0.30f, 0.0f, 0.30f, 0.0f);
        break;
    case IC_SYM_SEARCH:
        sym_ring(m, -0.06f, -0.06f, 0.24f);
        sym_line(m, 0.12f, 0.12f, 0.33f, 0.33f);
        break;
    case IC_SYM_RELOAD:
        sym_arc(m, 0.0f, 0.0f, 0.30f, -PI * 0.35f, PI * 1.45f);
        {
            const float p[] = { 0.22f, -0.40f, 0.26f, -0.19f, 0.05f, -0.17f };
            sym_poly(m, p, 3, 0);
        }
        break;
    case IC_SYM_POWER:
        sym_arc(m, 0.0f, 0.04f, 0.30f, -PI * 0.28f, PI * 1.28f);
        sym_line(m, 0.0f, -0.36f, 0.0f, -0.02f);
        break;
    case IC_SYM_RESTART:
        sym_arc(m, 0.0f, 0.02f, 0.30f, -PI * 0.5f, PI * 1.25f);
        {
            const float p[] = { -0.05f, -0.42f, 0.07f, -0.28f, -0.07f, -0.15f };
            sym_poly(m, p, 3, 0);
        }
        break;
    case IC_SYM_PLAY: {
        const float p[] = { -0.20f, -0.32f, 0.32f, 0.0f, -0.20f, 0.32f };
        sym_fill_convex(m, p, 3);
        break;
    }
    case IC_SYM_PAUSE:
        sym_line_w(m, -0.13f, -0.26f, -0.13f, 0.26f, 1.6f);
        sym_line_w(m, 0.13f, -0.26f, 0.13f, 0.26f, 1.6f);
        break;
    case IC_SYM_STOP: {
        const float p[] = { -0.24f, -0.24f, 0.24f, -0.24f, 0.24f, 0.24f, -0.24f, 0.24f };
        sym_fill_convex(m, p, 4);
        break;
    }
    case IC_SYM_SPEAKER: {
        const float body[] = { -0.36f, -0.10f, -0.20f, -0.10f, 0.0f, -0.30f,
                               0.0f, 0.30f, -0.20f, 0.10f, -0.36f, 0.10f };
        sym_poly(m, body, 6, 1);
        sym_arc(m, 0.02f, 0.0f, 0.16f, -PI * 0.28f, PI * 0.28f);
        sym_arc(m, 0.02f, 0.0f, 0.31f, -PI * 0.30f, PI * 0.30f);
        break;
    }
    case IC_SYM_GEAR: {
        
        const float dirs[8][2] = {
            { 1, 0 }, { 0.7071f, 0.7071f }, { 0, 1 }, { -0.7071f, 0.7071f },
            { -1, 0 }, { -0.7071f, -0.7071f }, { 0, -1 }, { 0.7071f, -0.7071f } };
        sym_ring(m, 0.0f, 0.0f, 0.24f);
        for (int i = 0; i < 8; i++) {
            sym_line_w(m, dirs[i][0] * 0.27f, dirs[i][1] * 0.27f,
                       dirs[i][0] * 0.37f, dirs[i][1] * 0.37f, 1.5f);
        }
        sym_ring(m, 0.0f, 0.0f, 0.09f);
        break;
    }
    case IC_SYM_GRID:
        sym_rrect(m, -0.34f, -0.34f, -0.06f, -0.06f, 0.06f);
        sym_rrect(m, 0.06f, -0.34f, 0.34f, -0.06f, 0.06f);
        sym_rrect(m, -0.34f, 0.06f, -0.06f, 0.34f, 0.06f);
        sym_rrect(m, 0.06f, 0.06f, 0.34f, 0.34f, 0.06f);
        break;
    case IC_SYM_FOLDER: {
        const float p[] = { -0.38f, -0.26f, -0.12f, -0.26f, -0.04f, -0.17f, 0.38f, -0.17f,
                            0.38f, 0.28f, -0.38f, 0.28f };
        sym_poly(m, p, 6, 1);
        sym_line(m, -0.38f, -0.07f, 0.38f, -0.07f);
        break;
    }
    case IC_SYM_DOCUMENT: {
        const float p[] = { -0.26f, -0.38f, 0.08f, -0.38f, 0.26f, -0.20f, 0.26f, 0.38f,
                            -0.26f, 0.38f };
        const float fold[] = { 0.08f, -0.38f, 0.08f, -0.20f, 0.26f, -0.20f };
        sym_poly(m, p, 5, 1);
        sym_poly(m, fold, 3, 0);
        break;
    }
    case IC_SYM_DISK:
        sym_rrect(m, -0.38f, -0.22f, 0.38f, 0.22f, 0.08f);
        sym_line(m, -0.38f, 0.04f, 0.38f, 0.04f);
        sym_disc(m, 0.22f, 0.13f, 0.045f);
        break;
    case IC_SYM_TERMINAL: {
        const float p[] = { -0.20f, -0.12f, -0.06f, 0.01f, -0.20f, 0.14f };
        sym_rrect(m, -0.38f, -0.30f, 0.38f, 0.30f, 0.08f);
        sym_poly(m, p, 3, 0);
        sym_line(m, 0.02f, 0.15f, 0.20f, 0.15f);
        break;
    }
    case IC_SYM_GLOBE:
        sym_ring(m, 0.0f, 0.0f, 0.36f);
        sym_line(m, -0.36f, 0.0f, 0.36f, 0.0f);
        sym_globe_meridian(m);
        break;
    case IC_SYM_MUSIC:
        sym_disc(m, -0.18f, 0.24f, 0.11f);
        sym_disc(m, 0.22f, 0.16f, 0.11f);
        sym_line(m, -0.08f, 0.24f, -0.08f, -0.26f);
        sym_line(m, 0.32f, 0.16f, 0.32f, -0.34f);
        sym_line_w(m, -0.08f, -0.26f, 0.32f, -0.34f, 1.5f);
        break;
    case IC_SYM_ACTIVITY: {
        const float p[] = { -0.40f, 0.02f, -0.18f, 0.02f, -0.08f, -0.28f, 0.06f, 0.30f,
                            0.16f, 0.02f, 0.40f, 0.02f };
        sym_poly(m, p, 6, 0);
        break;
    }
    case IC_SYM_SUN: {
        const float dirs[8][2] = {
            { 1, 0 }, { 0.7071f, 0.7071f }, { 0, 1 }, { -0.7071f, 0.7071f },
            { -1, 0 }, { -0.7071f, -0.7071f }, { 0, -1 }, { 0.7071f, -0.7071f } };
        sym_ring(m, 0.0f, 0.0f, 0.15f);
        for (int i = 0; i < 8; i++) {
            sym_line(m, dirs[i][0] * 0.27f, dirs[i][1] * 0.27f,
                     dirs[i][0] * 0.38f, dirs[i][1] * 0.38f);
        }
        break;
    }
    case IC_SYM_MOON:
        sym_disc(m, 0.0f, 0.0f, 0.32f);
        sym_disc_erase(m, 0.17f, -0.13f, 0.27f);
        break;
    case IC_SYM_INFO:
        sym_ring(m, 0.0f, 0.0f, 0.36f);
        sym_disc(m, 0.0f, -0.15f, 0.045f);
        sym_line(m, 0.0f, -0.02f, 0.0f, 0.18f);
        break;
    case IC_SYM_WARNING: {
        const float p[] = { 0.0f, -0.36f, 0.38f, 0.30f, -0.38f, 0.30f };
        sym_poly(m, p, 3, 1);
        sym_line(m, 0.0f, -0.12f, 0.0f, 0.08f);
        sym_disc(m, 0.0f, 0.19f, 0.04f);
        break;
    }
    case IC_SYM_TRASH: {
        const float body[] = { -0.26f, -0.20f, -0.20f, 0.36f, 0.20f, 0.36f, 0.26f, -0.20f };
        sym_line(m, -0.34f, -0.24f, 0.34f, -0.24f);
        sym_line(m, -0.10f, -0.34f, 0.10f, -0.34f);
        sym_poly(m, body, 4, 0);
        sym_line(m, -0.07f, -0.08f, -0.06f, 0.24f);
        sym_line(m, 0.07f, -0.08f, 0.06f, 0.24f);
        break;
    }
    case IC_SYM_WIFI:
        /* three arcs over a dot, opening upwards */
        sym_disc(m, 0.0f, 0.28f, 0.06f);
        sym_arc(m, 0.0f, 0.30f, 0.20f, -PI * 0.75f, -PI * 0.25f);
        sym_arc(m, 0.0f, 0.30f, 0.38f, -PI * 0.75f, -PI * 0.25f);
        sym_arc(m, 0.0f, 0.30f, 0.56f, -PI * 0.75f, -PI * 0.25f);
        break;
    case IC_SYM_LOCK:
        sym_rrect(m, -0.24f, -0.02f, 0.24f, 0.34f, 0.05f);
        sym_arc(m, 0.0f, -0.04f, 0.15f, -PI, 0.0f);
        sym_line(m, -0.15f, -0.04f, -0.15f, -0.02f);
        sym_line(m, 0.15f, -0.04f, 0.15f, -0.02f);
        break;
    case IC_SYM_NONE:
    case IC_SYM_COUNT:
    default:
        break;
    }
}

void ic_symbol_draw(ic_canvas_t *c, ic_symbol_t sym, float cx, float cy, float size,
                    ic_color_t color) {
    static sym_mask_t m;
    int n, x0, y0;
    if (sym <= IC_SYM_NONE || sym >= IC_SYM_COUNT || size < 4.0f) return;
    n = (int)size + 4;
    if (n > SYM_MAX) {
        n = SYM_MAX;
        size = (float)(SYM_MAX - 4);
    }
    x0 = (int)(cx - (float)n * 0.5f);
    y0 = (int)(cy - (float)n * 0.5f);
    for (int i = 0; i < n * n; i++) m.cov[i] = 0;
    m.n = n;
    m.scale = size;
    m.ox = cx - (float)x0;
    m.oy = cy - (float)y0;
    m.stroke = size / 11.0f;
    if (m.stroke < 1.25f) m.stroke = 1.25f;
    sym_build(&m, sym);
    ic_gfx_mask(c, x0, y0, m.cov, n, n, n, color);
}
