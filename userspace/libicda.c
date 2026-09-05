#include "libicda.h"
#include "font.h"
#include "icon_data.h"

/* ============================== strings ============================== */

uint64_t ic_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int ic_strcmp(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return (a == b) ? 0 : -1;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

int ic_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

char *ic_strcpy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return dst;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return dst;
}

char *ic_strcat(char *dst, const char *src, uint64_t cap) {
    uint64_t at = ic_strlen(dst);
    uint64_t i = 0;
    if (!dst || cap == 0 || at >= cap) return dst;
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
    return dst;
}

int ic_strprefix(const char *s, const char *prefix) {
    uint64_t i = 0;
    if (!s || !prefix) return 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

char ic_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

void ic_uint_to_str(uint64_t v, char *out, uint64_t cap) {
    char tmp[32];
    uint64_t len = 0;
    uint64_t i = 0;

    if (!out || cap == 0) return;
    if (v == 0) {
        ic_strcpy(out, "0", cap);
        return;
    }
    while (v && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (len && i + 1 < cap) {
        out[i++] = tmp[--len];
    }
    out[i] = 0;
}

int ic_parse_uint(const char *s, uint64_t *out) {
    uint64_t v = 0;
    uint64_t i = 0;

    if (!s || !*s || !out) return 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (uint64_t)(s[i] - '0');
        i++;
    }
    *out = v;
    return 1;
}

/* ============================== canvas =============================== */

uint32_t ic_blend(uint32_t a, uint32_t b, int n, int d) {
    int ar = (int)((a >> 16) & 0xFF);
    int ag = (int)((a >> 8) & 0xFF);
    int ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF);
    int bg = (int)((b >> 8) & 0xFF);
    int bb = (int)(b & 0xFF);
    int r = ar + ((br - ar) * n) / d;
    int g = ag + ((bg - ag) * n) / d;
    int bl = ab + ((bb - ab) * n) / d;
    return (uint32_t)((r << 16) | (g << 8) | bl);
}

static int ic_in_bounds(const ic_canvas_t *c, int x, int y) {
    return c && c->px && x >= 0 && y >= 0 && x < c->w && y < c->h;
}

/* A canvas dimension can never legitimately exceed the 2560x1600 back
 * buffer; the window manager has been observed passing wildly corrupted
 * dims (tens of millions of pixels) when its stack frame is disturbed,
 * which turns every draw into an out-of-bounds write past the buffer
 * and panics the kernel.  Reject anything implausible outright. */
static int ic_canvas_sane(const ic_canvas_t *c) {
    return c && c->px && c->w > 0 && c->h > 0 && c->w <= 8192 && c->h <= 8192;
}

void ic_fill(ic_canvas_t *c, uint32_t color) {
    if (!ic_canvas_sane(c)) return;
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = color;
}

void ic_rect(ic_canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    if (!ic_canvas_sane(c)) return;
    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > c->w) x2 = c->w;
    if (y2 > c->h) y2 = c->h;
    for (int cy = y1; cy < y2; cy++) {
        for (int cx = x1; cx < x2; cx++) {
            c->px[cy * c->w + cx] = color;
        }
    }
}

void ic_hline(ic_canvas_t *c, int x, int y, int len, uint32_t color) {
    if (!ic_canvas_sane(c) || y < 0 || y >= c->h) return;
    int x1 = x;
    int x2 = x + len;
    if (x1 < 0) x1 = 0;
    if (x2 > c->w) x2 = c->w;
    for (int cx = x1; cx < x2; cx++) c->px[y * c->w + cx] = color;
}

void ic_vline(ic_canvas_t *c, int x, int y, int len, uint32_t color) {
    if (!ic_canvas_sane(c) || x < 0 || x >= c->w) return;
    int y1 = y;
    int y2 = y + len;
    if (y1 < 0) y1 = 0;
    if (y2 > c->h) y2 = c->h;
    for (int cy = y1; cy < y2; cy++) c->px[cy * c->w + x] = color;
}

void ic_outline(ic_canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    ic_hline(c, x, y, w, color);
    ic_hline(c, x, y + h - 1, w, color);
    ic_vline(c, x, y, h, color);
    ic_vline(c, x + w - 1, y, h, color);
}

/* Is (dx,dy) within a rounded rect of size w x h and corner radius r?
 * (dx,dy) are offsets from the rect origin; used for both fill and outline. */
static int ic_in_round(const int dx, const int dy, const int w, const int h, const int r) {
    if (dx < 0 || dy < 0 || dx >= w || dy >= h) return 0;
    int cx, cy;
    if (dx < r && dy < r) { cx = r - 1; cy = r - 1; }
    else if (dx >= w - r && dy < r) { cx = w - r; cy = r - 1; }
    else if (dx < r && dy >= h - r) { cx = r - 1; cy = h - r; }
    else if (dx >= w - r && dy >= h - r) { cx = w - r; cy = h - r; }
    else return 1;
    {
        int ox = dx - cx;
        int oy = dy - cy;
        return ox * ox + oy * oy <= r * r;
    }
}

/* Soft drop shadow: a real blurred-feel falloff (quadratic alpha ramp)
 * around a rounded rect, blended over whatever is already in the canvas.
 * Only the perimeter band is iterated (2r*(w+h) pixels), so it is cheap
 * enough to redraw per frame.  This is the "actual shadow" - not a flat
 * grey outline - modern desktops draw under windows. */
void ic_draw_shadow(ic_canvas_t *c, int x, int y, int w, int h, int radius, uint32_t color) {
    int m = radius;
    int sh_r;
    uint32_t sr, sg, sb;

    if (!ic_canvas_sane(c) || m <= 0) return;
    if (m > 24) m = 24;
    if (w <= 0 || h <= 0) return;
    sh_r = m < 10 ? m : 10;      /* corner radius of the shadow body */
    sr = (color >> 16) & 0xFF;
    sg = (color >> 8) & 0xFF;
    sb = color & 0xFF;

    /* Row loop over the expanded rect; the band is the outer m pixels. */
    for (int yy = y - m; yy < y + h + m; yy++) {
        int x0, x1;
        if (yy < 0 || yy >= c->h) continue;
        if (yy >= y && yy < y + h) {
            /* Side bands only */
            x0 = x - m; x1 = x;
        } else {
            /* Top/bottom strips span the full width */
            x0 = x - m; x1 = x + w + m;
        }
        for (int xx = x0; xx < x1; xx++) {
            int dx, dy, d2;
            int ax;
            if (xx < 0 || xx >= c->w) continue;
            if (xx >= x && xx < x + w && yy >= y && yy < y + h) continue;

            /* Distance to the rounded-rect body (Euclidean at corners). */
            dx = 0; dy = 0;
            if (xx < x) dx = x - xx; else if (xx >= x + w) dx = xx - (x + w - 1);
            if (yy < y) dy = y - yy; else if (yy >= y + h) dy = yy - (y + h - 1);
            d2 = dx * dx + dy * dy;
            /* In the corner regions subtract the body radius for a
             * rounded-corner shadow silhouette. */
            if (dx > 0 && dy > 0 && d2 <= (sh_r - 1) * (sh_r - 1)) continue;
            {
                int dist = 0;
                while (d2 > dist * dist) dist++;
                if (dist <= 0) continue;
                if (dist >= m) continue;
                /* Quadratic falloff: sharp near the window, soft far out. */
                ax = (m - dist) * (m - dist) * 255 / (m * m);
                if (ax > 255) ax = 255;
                if (ax < 4) continue;
                {
                    uint32_t old = c->px[yy * c->w + xx];
                    uint32_t r = (((old >> 16) & 0xFF) * (255 - ax) + sr * ax) / 255;
                    uint32_t g = (((old >> 8) & 0xFF) * (255 - ax) + sg * ax) / 255;
                    uint32_t b = ((old & 0xFF) * (255 - ax) + sb * ax) / 255;
                    c->px[yy * c->w + xx] = (r << 16) | (g << 8) | b;
                }
            }
        }
    }
}

void ic_rect_r(ic_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color) {
    if (!ic_canvas_sane(c) || r < 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int dy = 0; dy < h; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= c->h) continue;
        for (int dx = 0; dx < w; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= c->w) continue;
            if (ic_in_round(dx, dy, w, h, r)) {
                c->px[yy * c->w + xx] = color;
            }
        }
    }
}

void ic_outline_r(ic_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color) {
    if (!c || !c->px || r < 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int dy = 0; dy < h; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= c->h) continue;
        for (int dx = 0; dx < w; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= c->w) continue;
            if (ic_in_round(dx, dy, w, h, r)) {
                int edge = dx == 0 || dy == 0 || dx == w - 1 || dy == h - 1;
                if (!edge) {
                    /* 1px in from each edge */
                    edge = dx == 1 || dy == 1 || dx == w - 2 || dy == h - 2;
                    if (!edge) continue;
                }
                c->px[yy * c->w + xx] = color;
            }
        }
    }
}

void ic_gradient_v(ic_canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (!c || !c->px || h <= 0) return;
    if (h == 1) {
        ic_rect(c, x, y, w, 1, top);
        return;
    }
    for (int row = 0; row < h; row++) {
        ic_rect(c, x, y + row, w, 1, ic_blend(top, bottom, row, h - 1));
    }
}

void ic_gradient_h(ic_canvas_t *c, int x, int y, int w, int h, uint32_t left, uint32_t right) {
    if (!c || !c->px || w <= 0) return;
    if (w == 1) {
        ic_rect(c, x, y, 1, h, left);
        return;
    }
    for (int col = 0; col < w; col++) {
        ic_rect(c, x + col, y, 1, h, ic_blend(left, right, col, w - 1));
    }
}

void ic_blend_px(ic_canvas_t *c, int x, int y, uint32_t color) {
    if (!ic_in_bounds(c, x, y)) return;
    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) {
        c->px[y * c->w + x] = color;
        return;
    }
    {
        uint32_t dst = c->px[y * c->w + x];
        int dr = (int)((dst >> 16) & 0xFF);
        int dg = (int)((dst >> 8) & 0xFF);
        int db = (int)(dst & 0xFF);
        int sr = (int)((color >> 16) & 0xFF);
        int sg = (int)((color >> 8) & 0xFF);
        int sb = (int)(color & 0xFF);
        int inv = 255 - (int)a;
        c->px[y * c->w + x] = (uint32_t)(
            (((sr * (int)a + dr * inv) / 255) << 16) |
            (((sg * (int)a + dg * inv) / 255) << 8) |
            ((sb * (int)a + db * inv) / 255));
    }
}

int ic_text_width(const char *s) {
    return (int)(ic_strlen(s) * FONT_CELL_WIDTH);
}

void ic_text(ic_canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    if (!ic_canvas_sane(c) || !s) return;
    int cx = x;
    while (*s) {
        font_draw_char(c->px, c->w, c->h, c->w, cx, y, *s, fg, bg);
        cx += FONT_CELL_WIDTH;
        s++;
    }
}

void ic_text_clip(ic_canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg, int max_px) {
    if (!ic_canvas_sane(c) || !s || max_px <= 0) return;
    int cx = x;
    while (*s && cx + FONT_CELL_WIDTH <= x + max_px) {
        font_draw_char(c->px, c->w, c->h, c->w, cx, y, *s, fg, bg);
        cx += FONT_CELL_WIDTH;
        s++;
    }
}

/* =============================== icons =============================== */

#define ICON_MAGIC0 'I'
#define ICON_MAGIC1 'C'
#define ICON_MAGIC2 'D'
#define ICON_MAGIC3 'A'
#define ICON_VER 1
#define ICON_HDR 12

int ic_icon_parse(const uint8_t *blob, uint64_t size, ic_icon_t *out) {
    if (!blob || !out || size < ICON_HDR) return -1;
    if (blob[0] != ICON_MAGIC0 || blob[1] != ICON_MAGIC1 ||
        blob[2] != ICON_MAGIC2 || blob[3] != ICON_MAGIC3) return -1;
    {
        uint16_t ver = (uint16_t)(blob[4] | (blob[5] << 8));
        uint16_t w = (uint16_t)(blob[6] | (blob[7] << 8));
        uint16_t h = (uint16_t)(blob[8] | (blob[9] << 8));
        uint64_t need;
        if (ver != ICON_VER || w == 0 || h == 0) return -1;
        need = ICON_HDR + (uint64_t)w * (uint64_t)h * 4;
        if (need > size) return -1;
        out->w = w;
        out->h = h;
        out->rgba = blob + ICON_HDR;
        return 0;
    }
}

int ic_icon_valid(const ic_icon_t *icon) {
    return icon && icon->rgba && icon->w > 0 && icon->h > 0;
}

void ic_icon_draw(ic_canvas_t *c, int x, int y, int dw, int dh, const ic_icon_t *icon) {
    if (!ic_canvas_sane(c) || !ic_icon_valid(icon) || dw <= 0 || dh <= 0) return;
    for (int dy = 0; dy < dh; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= c->h) continue;
        int sy = (int)((uint64_t)dy * icon->h / dh);
        if (sy >= icon->h) sy = icon->h - 1;
        for (int dx = 0; dx < dw; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= c->w) continue;
            int sx = (int)((uint64_t)dx * icon->w / dw);
            if (sx >= icon->w) sx = icon->w - 1;
            {
                const uint8_t *p = icon->rgba + (uint64_t)(sy * icon->w + sx) * 4;
                ic_blend_px(c, xx, yy, (uint32_t)((p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2]));
            }
        }
    }
}

/* ========================= .ico files =============================== */
/* Icons dropped into a folder (e.g. /usr/share/icons) as .ico files are
 * parsed at runtime and take precedence over the builtin set.  A plain
 * 32-bit BMP-compressed ICO is supported; PNG-compressed entries are
 * skipped so the largest decodable size is always chosen. */

#define IC_FOLDER_ICON_MAX 24
#define IC_ICO_DECODE_MAX  128
#define IC_ICO_FILE_BUF    (256 * 1024)

typedef struct {
    char      name[32];
    ic_icon_t icon;
} ic_folder_icon_t;

static ic_folder_icon_t folder_icons[IC_FOLDER_ICON_MAX];
static uint8_t folder_icon_rgba[IC_FOLDER_ICON_MAX][IC_ICO_DECODE_MAX * IC_ICO_DECODE_MAX * 4];
static int folder_icon_count = 0;
static uint8_t ico_file_buf[IC_ICO_FILE_BUF];

static int ic_suffix_ci(const char *text, const char *suffix) {
    uint64_t tl = ic_strlen(text);
    uint64_t sl = ic_strlen(suffix);
    if (sl > tl) return 0;
    for (uint64_t i = 0; i < sl; i++) {
        if (ic_lower(text[tl - sl + i]) != ic_lower(suffix[i])) return 0;
    }
    return 1;
}

static void ic_lower_copy(char *dst, uint64_t cap, const char *src) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = ic_lower(src[i]);
        i++;
    }
    dst[i] = 0;
}

/* Little-endian 16/32-bit readers for the binary formats. */
static uint16_t ico_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ico_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int32_t ico_i32(const uint8_t *p) { return (int32_t)ico_u32(p); }

const ic_icon_t *ic_icon_builtin(const char *name) {
    static ic_icon_t views[IC_BUILTIN_ICON_COUNT];
    static int init = 0;
    if (!name) return 0;
    for (int i = 0; i < folder_icon_count; i++) {
        if (ic_streq(folder_icons[i].name, name)) return &folder_icons[i].icon;
    }
    if (!init) {
        for (int i = 0; i < IC_BUILTIN_ICON_COUNT; i++) {
            views[i].w = ic_builtin_icons[i].w;
            views[i].h = ic_builtin_icons[i].h;
            views[i].rgba = ic_builtin_icons[i].rgba;
        }
        init = 1;
    }
    for (int i = 0; i < IC_BUILTIN_ICON_COUNT; i++) {
        if (ic_streq(ic_builtin_icons[i].name, name)) return &views[i];
    }
    return 0;
}

int ic_ico_parse(const uint8_t *blob, uint64_t size, int max_decode,
                 ic_icon_t *out, uint8_t *rgba_out, uint64_t rgba_cap) {
    uint16_t count;
    int best = -1;
    uint64_t best_area = 0;
    uint64_t i;

    if (!blob || !out || !rgba_out || size < 6) return -1;
    if (ico_u16(blob) != 0 || ico_u16(blob + 2) != 1) return -1;   /* reserved + type */
    count = ico_u16(blob + 4);
    if (count == 0) return -1;
    if (6 + (uint64_t)count * 16 > size) return -1;

    /* Pick the largest decodable BMP entry that fits max_decode, so
     * multi-size .ico files keep their best quality. */
    for (i = 0; i < count; i++) {
        const uint8_t *e = blob + 6 + i * 16;
        int ew = e[0] == 0 ? 256 : (int)e[0];
        int eh = e[1] == 0 ? 256 : (int)e[1];
        uint32_t bytes = ico_u32(e + 8);
        uint32_t off = ico_u32(e + 12);
        uint32_t bi_size;
        int32_t dib_h;
        int bpp;
        uint64_t area;

        if (bytes < 40 || off + bytes > size) continue;
        if (blob[off] == 0x89 && blob[off + 1] == 0x50 &&
            blob[off + 2] == 0x4E && blob[off + 3] == 0x47) {
            continue;   /* PNG-compressed entry: not supported */
        }
        bi_size = ico_u32(blob + off);
        if (bi_size < 40 || bi_size + 12 > bytes) continue;
        dib_h = ico_i32(blob + off + 8);
        if (dib_h == 0) continue;
        {
            uint16_t planes = ico_u16(blob + off + 12);
            bpp = ico_u16(blob + off + 14);
            if (planes != 1 || (bpp != 24 && bpp != 32)) continue;
            if (ico_u32(blob + off + 16) != 0) continue;   /* BI_RGB only */
        }
        if ((int)ico_u32(blob + off + 4) > max_decode) continue;
        if (dib_h < 0 && -dib_h > max_decode) continue;
        if (dib_h > 0 && dib_h / 2 > max_decode) continue;
        area = (uint64_t)ew * (uint64_t)eh;
        if (area > best_area) {
            best_area = area;
            best = (int)i;
        }
    }
    {
        int chosen = best;
        if (chosen == -1) return -1;

        {
            const uint8_t *e = blob + 6 + (uint64_t)chosen * 16;
            uint32_t off = ico_u32(e + 12);
            int32_t dib_h = ico_i32(blob + off + 8);
            int w = (int)ico_u32(blob + off + 4);
            int h = dib_h > 0 ? dib_h / 2 : -dib_h;
            int bpp = ico_u16(blob + off + 14);
            int row_bytes = ((w * bpp + 31) / 32) * 4;
            const uint8_t *xor_data = blob + off + ico_u32(blob + off);
            uint64_t need = (uint64_t)w * (uint64_t)h * 4;
            int y;

            if (w <= 0 || h <= 0 || w > max_decode || h > max_decode) return -1;
            if (need > rgba_cap) return -1;
            /* Guard against malformed files walking off the entry. */
            {
                uint32_t entry_bytes = ico_u32(e + 8);
                if ((uint64_t)(h - 1) * row_bytes + (uint64_t)w * (uint64_t)(bpp / 8)
                        + ico_u32(blob + off) > entry_bytes) {
                    return -1;
                }
            }
            for (y = 0; y < h; y++) {
                int src_row = dib_h > 0 ? (h - 1 - y) : y;   /* ICO DIBs are bottom-up */
                const uint8_t *row = xor_data + (uint64_t)src_row * row_bytes;
                uint8_t *dst = rgba_out + (uint64_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = row + (uint64_t)x * (bpp / 8);
                    if (bpp == 32) {
                        dst[x * 4 + 0] = p[2];   /* B G R A -> R G B A */
                        dst[x * 4 + 1] = p[1];
                        dst[x * 4 + 2] = p[0];
                        dst[x * 4 + 3] = p[3];
                    } else {
                        dst[x * 4 + 0] = p[2];
                        dst[x * 4 + 1] = p[1];
                        dst[x * 4 + 2] = p[0];
                        dst[x * 4 + 3] = 255;
                    }
                }
            }
            out->w = (uint16_t)w;
            out->h = (uint16_t)h;
            out->rgba = rgba_out;
            return 0;
        }
    }
}

int ic_icon_load_folder(const char *dir) {
    char list[4096];
    uint64_t rc;
    uint64_t pos = 0;

    if (!dir) return -1;
    folder_icon_count = 0;
    rc = icda_list_dir(dir, list, sizeof(list));
    if ((long)rc < 0) return -1;

    while (pos < rc && folder_icon_count < IC_FOLDER_ICON_MAX) {
        char entry[64];
        char path[256];
        char stem[32];
        uint64_t ei = 0;
        uint64_t n;
        ic_icon_t icon;
        uint64_t stem_len;

        while (pos < rc && list[pos] != '\n' && ei + 1 < sizeof(entry)) {
            entry[ei++] = list[pos++];
        }
        while (pos < rc && list[pos] != '\n') pos++;
        if (pos < rc && list[pos] == '\n') pos++;
        entry[ei] = 0;
        if (ei == 0) continue;
        if (!ic_suffix_ci(entry, ".ico")) continue;

        ic_lower_copy(stem, sizeof(stem), entry);
        stem_len = ic_strlen(stem);
        if (stem_len <= 4) continue;                /* just ".ico" */
        stem[stem_len - 4] = 0;                     /* strip extension */
        if (stem[0] == 0) continue;

        ic_strcpy(path, dir, sizeof(path));
        {
            uint64_t dl = ic_strlen(path);
            if (dl == 0 || path[dl - 1] != '/') ic_strcat(path, "/", sizeof(path));
        }
        ic_strcat(path, entry, sizeof(path));

        n = icda_read_file(path, (char *)ico_file_buf, sizeof(ico_file_buf));
        if ((long)n < 0) continue;
        if (ic_ico_parse(ico_file_buf, n, IC_ICO_DECODE_MAX, &icon,
                         folder_icon_rgba[folder_icon_count],
                         sizeof(folder_icon_rgba[0])) != 0) {
            continue;
        }
        ic_strcpy(folder_icons[folder_icon_count].name, stem, sizeof(folder_icons[0].name));
        folder_icons[folder_icon_count].icon = icon;
        folder_icon_count++;
    }
    return folder_icon_count > 0 ? 0 : -1;
}

/* =============================== theme =============================== */

const ic_theme_t *ic_theme_default(void) {
    /* Dark slate + electric-blue accent — charcoal surfaces, one vivid
     * highlight, rounded corners everywhere. */
    static const ic_theme_t t = {
        .title_top = 0x001E293B,
        .title_bottom = 0x001E293B,
        .title_top_active = 0x0023344D,
        .title_bottom_active = 0x0023344D,
        .border = 0x00334155,
        .border_active = 0x0038BDF8,
        .shadow = 0x00000000,
        .taskbar_top = 0x00111D2E,
        .taskbar_bottom = 0x00111D2E,
        .accent = 0x0038BDF8,
        .accent_hi = 0x000EA5E9,
        .accent_lo = 0x000284BE,
        .panel = 0x001E293B,
        .panel_edge = 0x00334155,
        .text = 0x00F1F5F9,
        .text_muted = 0x0094A3B8,
        .text_on_accent = 0x000F172A,
        .wall_top = IC_WALL_TOP,
        .wall_bottom = IC_WALL_BOTTOM,
        .desktop_bg = 0x000F172A,
        .surface = 0x001E293B,
        .surface_hover = 0x0023344D,
        .surface_active = 0x0038BDF8
    };
    return &t;
}

/* =========================== window chrome =========================== */

void ic_draw_chrome(ic_canvas_t *c, const ic_theme_t *t, const ic_window_t *win,
                    const ic_icon_t *icon_close, const ic_icon_t *icon_min,
                    const ic_icon_t *icon_max) {
    int anim = win->anim;
    int wx, wy;
    uint32_t title_bg;
    uint32_t title_fg;
    uint32_t border;
    int max_x;
    int max_y;
    int min_x;
    int min_y;
    int cls_x;
    int cls_y;

    if (!c || !t || !win || win->minimized) return;
    if (anim < 0) anim = 0;
    if (anim > IC_ANIM_MAX) anim = IC_ANIM_MAX;
    wx = win->x;
    wy = win->y;

    border = win->focused ? t->border_active : t->border;
    title_bg = win->focused ? t->title_top_active : t->title_top;
    title_fg = win->focused ? t->text : t->text_muted;

    max_x = wx + win->w - IC_BTN_MAX_OFF;
    max_y = wy - IC_TITLE_H + 5;
    min_x = wx + win->w - IC_BTN_MIN_OFF;
    min_y = wy - IC_TITLE_H + 5;
    cls_x = wx + win->w - IC_BTN_CLS_OFF;
    cls_y = wy - IC_TITLE_H + 5;

    ic_draw_shadow(c, wx - 1, wy - IC_TITLE_H - 1, win->w + 2, win->h + IC_TITLE_H + 2, IC_SHADOW_RADIUS, IC_SHADOW_COLOR);
    ic_rect_r(c, wx - 1, wy - IC_TITLE_H - 1, win->w + 2, IC_TITLE_H + 2, IC_RADIUS_WINDOW, border);
    ic_rect_r(c, wx, wy - IC_TITLE_H, win->w, IC_TITLE_H, IC_RADIUS_WINDOW - 1, title_bg);
    if (win->focused) ic_hline(c, wx + IC_RADIUS_WINDOW, wy - 1, win->w - IC_RADIUS_WINDOW*2, t->accent);
    ic_text_clip(c, wx + 12, wy - IC_TITLE_H + 7, win->title, title_fg, title_bg,
                 win->w > 104 ? win->w - 104 : win->w);
    ic_vline(c, wx - 1, wy, win->h + 1, border);
    ic_vline(c, wx + win->w, wy, win->h + 1, border);
    ic_hline(c, wx - 1, wy + win->h, win->w + 2, border);

    if (win->hover_max) {
        ic_rect_r(c, max_x, max_y, IC_BTN_W, IC_BTN_H, IC_RADIUS_BUTTON, t->accent);
    }
    if (icon_max && ic_icon_valid(icon_max)) {
        ic_icon_draw(c, max_x + 3, max_y + 3, 12, 10, icon_max);
    } else {
        ic_outline(c, max_x + 5, max_y + 4, 8, 7, win->focused ? 0x00E2E8F0 : 0x0094A3B8);
    }

    if (win->hover_min) {
        ic_rect_r(c, min_x, min_y, IC_BTN_W, IC_BTN_H, IC_RADIUS_BUTTON, t->accent);
    }
    if (icon_min && ic_icon_valid(icon_min)) {
        ic_icon_draw(c, min_x + 3, min_y + 3, 12, 10, icon_min);
    } else {
        ic_rect(c, min_x + 5, min_y + 14, 8, 2, win->focused ? 0x00E2E8F0 : 0x0094A3B8);
    }

    if (win->hover_close) {
        ic_rect_r(c, cls_x, cls_y, IC_BTN_W, IC_BTN_H, IC_RADIUS_BUTTON, 0x00EF4444);
        if (icon_close && ic_icon_valid(icon_close)) {
            ic_icon_draw(c, cls_x + 3, cls_y + 3, 12, 10, icon_close);
        } else {
            ic_text(c, cls_x + 5, cls_y + 1, "x", 0x00FFFFFF, 0x00EF4444);
        }
    } else {
        if (icon_close && ic_icon_valid(icon_close)) {
            ic_icon_draw(c, cls_x + 3, cls_y + 3, 12, 10, icon_close);
        } else {
            ic_text(c, cls_x + 5, cls_y + 1, "x", win->focused ? 0x00E2E8F0 : 0x0094A3B8, title_bg);
        }
    }
}

int ic_hit_title(const ic_window_t *win, int mx, int my) {
    return win && mx >= win->x && my >= win->y - IC_TITLE_H &&
           mx < win->x + win->w && my < win->y;
}

int ic_hit_minimize(const ic_window_t *win, int mx, int my) {
    return win && mx >= win->x + win->w - IC_BTN_MIN_OFF &&
           my >= win->y - IC_TITLE_H + 5 &&
           mx < win->x + win->w - IC_BTN_MIN_OFF + IC_BTN_W &&
           my < win->y - IC_TITLE_H + 5 + IC_BTN_H;
}

int ic_hit_maximize(const ic_window_t *win, int mx, int my) {
    return win && mx >= win->x + win->w - IC_BTN_MAX_OFF &&
           my >= win->y - IC_TITLE_H + 5 &&
           mx < win->x + win->w - IC_BTN_MAX_OFF + IC_BTN_W &&
           my < win->y - IC_TITLE_H + 5 + IC_BTN_H;
}

int ic_hit_close(const ic_window_t *win, int mx, int my) {
    return win && mx >= win->x + win->w - IC_BTN_CLS_OFF &&
           my >= win->y - IC_TITLE_H + 5 &&
           mx < win->x + win->w - IC_BTN_CLS_OFF + IC_BTN_W &&
           my < win->y - IC_TITLE_H + 5 + IC_BTN_H;
}

int ic_hit_client(const ic_window_t *win, int mx, int my) {
    return win && mx >= win->x && my >= win->y &&
           mx < win->x + win->w && my < win->y + win->h;
}

/* ============================== widgets ============================== */

int ic_hit_rect(int mx, int my, ic_rect_t r) {
    return mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
}

ic_btn_state_t ic_button_state(int enabled, int hover, int pressed) {
    if (!enabled) return IC_BTN_DISABLED;
    if (pressed) return IC_BTN_ACTIVE;
    if (hover) return IC_BTN_HOVER;
    return IC_BTN_NORMAL;
}

void ic_draw_button(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t r,
                    const char *label, ic_btn_state_t state) {
    uint32_t fill, edge, fg;
    int tw, tx, ty;

    if (!c || !t) return;
    switch (state) {
        case IC_BTN_ACTIVE:
            fill = t->accent;
            edge = t->accent_hi;
            fg = t->text_on_accent;
            break;
        case IC_BTN_HOVER:
            fill = 0x0023344D;
            edge = t->accent;
            fg = t->text;
            break;
        case IC_BTN_DISABLED:
            fill = 0x001E293B;
            edge = 0x00334155;
            fg = t->text_muted;
            break;
        default:
            fill = 0x001E293B;
            edge = 0x00334155;
            fg = t->text;
            break;
    }

    ic_rect_r(c, r.x, r.y, r.w, r.h, IC_RADIUS_BUTTON, fill);
    ic_outline_r(c, r.x, r.y, r.w, r.h, IC_RADIUS_BUTTON, edge);

    tw = ic_text_width(label);
    tx = r.x + (r.w - tw) / 2;
    ty = r.y + (r.h - FONT_HEIGHT) / 2;
    if (tx < r.x + 4) tx = r.x + 4;
    ic_text(c, tx, ty, label, fg, fill);
}

/* ============================ menu / dialog / slider ================= */

int ic_menu_row_h(void) {
    return FONT_HEIGHT + 8;
}

int ic_menu_width(const ic_menu_t *m) {
    int w = 0;
    int i;

    if (!m) return 0;
    for (i = 0; i < m->count && i < IC_MENU_MAX_ITEMS; i++) {
        int tw;
        if (!m->items[i]) continue;
        tw = ic_text_width(m->items[i]);
        if (tw > w) w = tw;
    }
    return w + 32;
}

int ic_menu_height(const ic_menu_t *m) {
    int n;

    if (!m) return 0;
    n = m->count;
    if (n < 0) n = 0;
    if (n > IC_MENU_MAX_ITEMS) n = IC_MENU_MAX_ITEMS;
    return n * ic_menu_row_h() + 12;
}

void ic_menu_draw(ic_canvas_t *c, const ic_theme_t *t, int x, int y,
                  const ic_menu_t *m) {
    int w;
    int h;
    int row;
    int i;
    int n;

    if (!c || !t || !m) return;
    n = m->count;
    if (n <= 0) return;
    if (n > IC_MENU_MAX_ITEMS) n = IC_MENU_MAX_ITEMS;
    w = ic_menu_width(m);
    h = ic_menu_height(m);
    if (w <= 0 || h <= 0) return;
    row = ic_menu_row_h();

    ic_rect_r(c, x, y, w, h, IC_RADIUS_MENU, t->panel);
    ic_outline_r(c, x, y, w, h, IC_RADIUS_MENU, t->panel_edge);
    for (i = 0; i < n; i++) {
        int iy = y + 6 + i * row;
        if (!m->items[i]) continue;
        if (i == m->selected) {
            ic_rect_r(c, x + 4, iy, w - 8, row, 6, t->accent);
            ic_text(c, x + 16, iy + 4, m->items[i], t->text_on_accent, t->accent);
        } else {
            ic_text(c, x + 16, iy + 4, m->items[i], t->text, t->panel);
        }
    }
}

int ic_menu_hit(const ic_menu_t *m, int x, int y, int mx, int my) {
    int row;
    int n;
    int i;

    if (!m) return -1;
    n = m->count;
    if (n <= 0) return -1;
    if (n > IC_MENU_MAX_ITEMS) n = IC_MENU_MAX_ITEMS;
    row = ic_menu_row_h();
    for (i = 0; i < n; i++) {
        ic_rect_t r;
        r.x = x + 4;
        r.y = y + 6 + i * row;
        r.w = ic_menu_width(m) - 8;
        r.h = row;
        if (ic_hit_rect(mx, my, r)) return i;
    }
    return -1;
}

void ic_dialog_draw(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t r,
                    const char *title, const char *body) {
    if (!c || !t) return;
    if (r.w <= 0 || r.h <= 0) return;
    ic_draw_shadow(c, r.x, r.y, r.w, r.h, IC_RADIUS_MENU, IC_SHADOW_COLOR);
    ic_rect_r(c, r.x, r.y, r.w, r.h, IC_RADIUS_MENU, t->panel);
    ic_outline_r(c, r.x, r.y, r.w, r.h, IC_RADIUS_MENU, t->panel_edge);
    if (title) {
        ic_text(c, r.x + 16, r.y + 12, title, t->text, t->panel);
        ic_rect(c, r.x + 16, r.y + 12 + FONT_HEIGHT + 6, r.w - 32, 1,
                t->panel_edge);
    }
    if (body) {
        ic_text_clip(c, r.x + 16, r.y + 12 + FONT_HEIGHT + 6 + 10, body,
                     t->text_muted, t->panel, r.w - 32);
    }
}

void ic_slider_draw(ic_canvas_t *c, const ic_theme_t *t, ic_rect_t track,
                    int value, int vmin, int vmax) {
    int span;
    int frac_num;
    int thumb_x;
    int fill_w;

    if (!c || !t) return;
    if (track.w < 32 || track.h < 16) return;
    span = vmax - vmin;
    if (span <= 0) {
        value = vmin;
        span = 1;
    }
    if (value < vmin) value = vmin;
    if (value > vmax) value = vmax;
    frac_num = (value - vmin) * (track.w - 16);

    ic_rect_r(c, track.x, track.y + track.h / 2 - 4, track.w, 8, 4,
              t->surface_hover);
    ic_outline_r(c, track.x, track.y + track.h / 2 - 4, track.w, 8, 4,
                 t->border);
    fill_w = frac_num / span;
    if (fill_w > 0) {
        ic_rect_r(c, track.x, track.y + track.h / 2 - 4, fill_w, 8, 4,
                  t->accent);
    }
    thumb_x = track.x + frac_num / span;
    ic_rect_r(c, thumb_x, track.y, 16, track.h, 6, t->text);
    ic_outline_r(c, thumb_x, track.y, 16, track.h, 6, t->accent);
}

int ic_slider_hit(ic_rect_t track, int mx, int my) {
    ic_rect_t big;
    big.x = track.x - 8;
    big.y = track.y - 8;
    big.w = track.w + 16;
    big.h = track.h + 16;
    return ic_hit_rect(mx, my, big);
}

int ic_slider_value_from_x(ic_rect_t track, int vmin, int vmax, int mx) {
    int span = vmax - vmin;
    int denom = track.w - 16;
    int rel;

    if (span <= 0 || denom <= 0) return vmin;
    rel = mx - (track.x + 8);
    if (rel < 0) rel = 0;
    if (rel > denom) rel = denom;
    return vmin + rel * span / denom;
}

/* ============================ app skeleton =========================== */

int ic_run_app(const char *title, int w, int h,
               ic_event_fn on_event, ic_draw_fn on_draw, void *ud) {
    if (!on_draw) return -1;
    if (gui_open_window(title, w, h) != 0) return -1;

    on_draw(ud); /* initial paint */

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
            if (on_event && !on_event(ud, &msg)) {
                gui_close_window();
                return 0;
            }
        }
        /* Repaint after input, and on the 8-tick cadence so nothing
         * animates the WM at a fixed 20fps while idle. */
        if (changed || (icda_ticks() % 8) == 0) {
            on_draw(ud);
        }
        icda_sleep(1);
    }
}
