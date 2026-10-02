






#include "ic_font.h"
#include "ic_fonts_gen.h"

#define IC_GLYPH_QUESTION ('?' - 32)
#define IC_GLYPH_ELLIPSIS (95)          

const ic_face_t *ic_font(ic_font_style_t style) {
    if ((int)style < 0 || style >= IC_FONT_STYLE_COUNT) style = IC_FONT_BODY;
    return ic_faces[style];
}



static int ic_next_glyph(const char *s, int n, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    uint32_t cp;
    int l;
    if (u[0] < 0x80) {
        *len = 1;
        if (u[0] >= 32 && u[0] <= 126) return u[0] - 32;
        return IC_GLYPH_QUESTION;
    }
    if ((u[0] & 0xE0) == 0xC0) { cp = u[0] & 0x1F; l = 2; }
    else if ((u[0] & 0xF0) == 0xE0) { cp = u[0] & 0x0F; l = 3; }
    else if ((u[0] & 0xF8) == 0xF0) { cp = u[0] & 0x07; l = 4; }
    else { *len = 1; return IC_GLYPH_QUESTION; }
    if (l > n) { *len = n > 0 ? n : 1; return IC_GLYPH_QUESTION; }
    for (int i = 1; i < l; i++) {
        if ((u[i] & 0xC0) != 0x80) { *len = i; return IC_GLYPH_QUESTION; }
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    *len = l;
    for (int i = 0; i < IC_FONT_EXTRA_COUNT; i++) {
        if (ic_font_extra_cps[i] == cp) return 95 + i;
    }
    return IC_GLYPH_QUESTION;
}

static int ic_kern_q6(const ic_face_t *f, int l, int r) {
    int lo = 0, hi = (int)f->nkern - 1;
    int key = (l << 8) | r;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int k = ((int)f->kern[mid].l << 8) | f->kern[mid].r;
        if (k == key) return (int)f->kern[mid].units * (int)f->px * 64 / (int)f->upem;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int ic_strlen_i(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}


static int ic_measure_q6(const ic_face_t *f, const char *s, int n) {
    int pen = 0, prev = -1, i = 0;
    while (i < n && s[i]) {
        int len;
        int g = ic_next_glyph(s + i, n - i, &len);
        if (prev >= 0 && f->nkern) pen += ic_kern_q6(f, prev, g);
        pen += f->adv[g];
        prev = g;
        i += len;
    }
    return pen;
}

int ic_text_measure_n(const ic_face_t *f, const char *s, int n) {
    if (!f || !s) return 0;
    return (ic_measure_q6(f, s, n) + 63) >> 6;
}

int ic_text_measure(const ic_face_t *f, const char *s) {
    return ic_text_measure_n(f, s, ic_strlen_i(s));
}

int ic_text_fit(const ic_face_t *f, const char *s, int max_w) {
    int pen = 0, prev = -1, i = 0, limit = max_w * 64;
    if (!f || !s) return 0;
    while (s[i]) {
        int len;
        int g = ic_next_glyph(s + i, 8, &len);
        int next = pen + f->adv[g] + (prev >= 0 && f->nkern ? ic_kern_q6(f, prev, g) : 0);
        if (next > limit) break;
        pen = next;
        prev = g;
        i += len;
    }
    return i;
}



static const uint8_t *ic_coverage_lut(ic_color_t color) {
    static uint8_t lift[256];
    static int ready;
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    uint32_t luma = (r * 54 + g * 183 + b * 19) >> 8;
    if (luma < 150) return 0;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            

            float x = (float)i / 255.0f;
            float y = 0.55f * x + 0.45f * ic_sqrtf(x);
            if (y > 1.0f) y = 1.0f;
            lift[i] = (uint8_t)(y * 255.0f + 0.5f);
        }
        ready = 1;
    }
    return lift;
}

int ic_text_draw_n(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                   int n, ic_color_t color) {
    int pen = x * 64, prev = -1, i = 0;
    const uint8_t *lut;
    uint8_t mask[64 * 64];
    if (!f || !s) return 0;
    lut = ic_coverage_lut(color);
    while (i < n && s[i]) {
        int len;
        int g = ic_next_glyph(s + i, n - i, &len);
        const ic_fglyph_t *gl;
        int phase;
        if (prev >= 0 && f->nkern) pen += ic_kern_q6(f, prev, g);
        phase = ((pen & 63) * IC_FONT_SUBPIXEL) >> 6;
        gl = &f->glyphs[g * IC_FONT_SUBPIXEL + phase];
        if (c && gl->w && gl->h) {
            const uint8_t *src = f->alpha + gl->off;
            if (lut && gl->w * gl->h <= (int)sizeof(mask)) {
                for (int k = 0; k < gl->w * gl->h; k++) mask[k] = lut[src[k]];
                src = mask;
            }
            ic_gfx_mask(c, (pen >> 6) + gl->ox, y + gl->oy, src, gl->w, gl->h, gl->w, color);
        }
        pen += f->adv[g];
        prev = g;
        i += len;
    }
    return ((pen - x * 64) + 63) >> 6;
}

int ic_text_draw(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                 ic_color_t color) {
    return ic_text_draw_n(c, f, x, y, s, ic_strlen_i(s), color);
}

int ic_text_center_baseline(const ic_face_t *f, int y, int h) {
    if (!f) return y + h;
    
    return y + (h + f->cap_h + 1) / 2;
}

void ic_text_draw_in(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                     ic_color_t color, ic_align_t align) {
    int n, w, base, x;
    if (!f || !s || r.w <= 0) return;
    n = ic_strlen_i(s);
    w = ic_text_measure_n(f, s, n);
    base = ic_text_center_baseline(f, r.y, r.h);
    if (w > r.w) {
        
        static const char ell[] = "\xE2\x80\xA6";
        int ew = ic_text_measure(f, ell);
        int keep = ic_text_fit(f, s, r.w - ew);
        while (keep > 0 && s[keep - 1] == ' ') keep--;
        x = r.x;
        x += ic_text_draw_n(c, f, x, base, s, keep, color);
        ic_text_draw(c, f, x, base, ell, color);
        return;
    }
    x = r.x;
    if (align == IC_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
    else if (align == IC_ALIGN_RIGHT) x = r.x + r.w - w;
    ic_text_draw_n(c, f, x, base, s, n, color);
}

int ic_text_draw_wrapped(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                         ic_color_t color, int line_gap) {
    int used = 0, i = 0, line_h;
    if (!f || !s) return 0;
    line_h = f->line_h + line_gap;
    while (s[i]) {
        int fit, brk, start = i;
        if (r.h > 0 && used + f->line_h > r.h) break;
        
        {
            int nl = start;
            while (s[nl] && s[nl] != '\n') nl++;
            fit = ic_text_fit(f, s + start, r.w);
            if (start + fit > nl) fit = nl - start;
        }
        brk = fit;
        if (s[start + fit] && s[start + fit] != '\n' && s[start + fit] != ' ') {
            int k = fit;
            while (k > 0 && s[start + k - 1] != ' ') k--;
            if (k > 0) brk = k;
        }
        if (brk <= 0) brk = 1;
        if (c) {
            int n = brk;
            while (n > 0 && s[start + n - 1] == ' ') n--;
            ic_text_draw_n(c, f, r.x, r.y + used + f->ascent, s + start, n, color);
        }
        used += line_h;
        i = start + brk;
        while (s[i] == ' ') i++;
        if (s[i] == '\n') i++;
    }
    return used > 0 ? used - line_gap : 0;
}
