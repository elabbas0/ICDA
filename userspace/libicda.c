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

void ic_fill(ic_canvas_t *c, uint32_t color) {
    if (!c || !c->px) return;
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = color;
}

void ic_rect(ic_canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    if (!c || !c->px) return;
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
    if (!c || !c->px || y < 0 || y >= c->h) return;
    int x1 = x;
    int x2 = x + len;
    if (x1 < 0) x1 = 0;
    if (x2 > c->w) x2 = c->w;
    for (int cx = x1; cx < x2; cx++) c->px[y * c->w + cx] = color;
}

void ic_vline(ic_canvas_t *c, int x, int y, int len, uint32_t color) {
    if (!c || !c->px || x < 0 || x >= c->w) return;
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

void ic_rect_r(ic_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color) {
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
    if (!c || !c->px || !s) return;
    int cx = x;
    while (*s) {
        font_draw_char(c->px, c->w, c->h, c->w, cx, y, *s, fg, bg);
        cx += FONT_CELL_WIDTH;
        s++;
    }
}

void ic_text_clip(ic_canvas_t *c, int x, int y, const char *s, uint32_t fg, uint32_t bg, int max_px) {
    if (!c || !c->px || !s || max_px <= 0) return;
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
    if (!c || !c->px || !ic_icon_valid(icon) || dw <= 0 || dh <= 0) return;
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
    static const ic_theme_t t = {
        .title_top = 0x008EA3B7,
        .title_bottom = 0x00475569,
        .title_top_active = 0x00499BFF,
        .title_bottom_active = 0x001D4FA8,
        .border = 0x00475569,
        .border_active = 0x000D47A1,
        .shadow = 0x004B5563,
        .taskbar_top = 0x002B73D7,
        .taskbar_bottom = 0x0015449C,
        .accent = 0x003D8BFF,
        .accent_hi = 0x000C3C88,
        .accent_lo = 0x001F5EBE,
        .panel = 0x00F8FBFF,
        .panel_edge = 0x0092B7E8,
        .text = 0x001F2937,
        .text_muted = 0x0064758B,
        .text_on_accent = 0x00FFFFFF
    };
    return &t;
}

/* =========================== window chrome =========================== */

void ic_draw_chrome(ic_canvas_t *c, const ic_theme_t *t, const ic_window_t *win,
                    const ic_icon_t *icon_close, const ic_icon_t *icon_min) {
    int anim = win->anim;
    int anim_off;
    int wx, wy;
    uint32_t border;
    uint32_t title_top;
    uint32_t title_bottom;

    if (!c || !t || !win || win->minimized) return;
    if (anim < 0) anim = 0;
    if (anim > IC_ANIM_MAX) anim = IC_ANIM_MAX;
    anim_off = (IC_ANIM_MAX - anim) * 2;
    wx = win->x;
    wy = win->y - anim_off;

    border = win->focused ? t->border_active : t->border;
    title_top = win->focused ? t->title_top_active : t->title_top;
    title_bottom = win->focused ? t->title_bottom_active : t->title_bottom;

    ic_rect(c, wx + 7, wy - IC_TITLE_H + 8, win->w + 2, win->h + IC_TITLE_H + 1, t->shadow);
    ic_outline(c, wx - 1, wy - IC_TITLE_H - 1, win->w + 2, win->h + IC_TITLE_H + 2, border);
    ic_gradient_v(c, wx, wy - IC_TITLE_H, win->w, IC_TITLE_H, title_top, title_bottom);
    ic_text_clip(c, wx + 9, wy - IC_TITLE_H + 6, win->title, 0x00FFFFFF, title_bottom,
                 win->w > 80 ? win->w - 74 : win->w);

    /* minimize */
    ic_gradient_v(c, wx + win->w - IC_BTN_MIN_OFF, wy - IC_TITLE_H + 5, IC_BTN_W, IC_BTN_H,
                  0x00E5EEFF, 0x00AFCBFF);
    ic_outline(c, wx + win->w - IC_BTN_MIN_OFF, wy - IC_TITLE_H + 5, IC_BTN_W, IC_BTN_H, 0x000D47A1);
    if (icon_min && ic_icon_valid(icon_min)) {
        ic_icon_draw(c, wx + win->w - IC_BTN_MIN_OFF + 3, wy - IC_TITLE_H + 8, 12, 10, icon_min);
    } else {
        ic_rect(c, wx + win->w - IC_BTN_MIN_OFF + 5, wy - IC_TITLE_H + 16, 8, 2, 0x000D47A1);
    }

    /* close */
    ic_gradient_v(c, wx + win->w - IC_BTN_CLS_OFF, wy - IC_TITLE_H + 5, IC_BTN_W, IC_BTN_H,
                  0x00FF7B7B, 0x00D31616);
    ic_outline(c, wx + win->w - IC_BTN_CLS_OFF, wy - IC_TITLE_H + 5, IC_BTN_W, IC_BTN_H, 0x008B0000);
    if (icon_close && ic_icon_valid(icon_close)) {
        ic_icon_draw(c, wx + win->w - IC_BTN_CLS_OFF + 3, wy - IC_TITLE_H + 8, 12, 10, icon_close);
    } else {
        ic_text(c, wx + win->w - IC_BTN_CLS_OFF + 5, wy - IC_TITLE_H + 5, "x", 0x00FFFFFF, 0x00D31616);
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
    uint32_t top, bottom, edge, fg;
    int tw, tx, ty;

    if (!c || !t) return;
    switch (state) {
        case IC_BTN_ACTIVE:
            top = t->accent;
            bottom = t->accent_lo;
            edge = t->accent_hi;
            fg = t->text_on_accent;
            break;
        case IC_BTN_HOVER:
            top = 0x00F2F8FF;
            bottom = 0x00D9E9FF;
            edge = t->accent_hi;
            fg = t->text;
            break;
        case IC_BTN_DISABLED:
            top = 0x00F0F2F5;
            bottom = 0x00E2E6EC;
            edge = 0x00C7CFDA;
            fg = t->text_muted;
            break;
        default:
            top = 0x00FFFFFF;
            bottom = 0x00DDEBFF;
            edge = t->accent_hi;
            fg = t->text;
            break;
    }

    ic_gradient_v(c, r.x, r.y, r.w, r.h, top, bottom);
    ic_outline(c, r.x, r.y, r.w, r.h, edge);

    tw = ic_text_width(label);
    tx = r.x + (r.w - tw) / 2;
    ty = r.y + (r.h - FONT_HEIGHT) / 2;
    if (tx < r.x + 4) tx = r.x + 4;
    ic_text(c, tx, ty, label, fg, bottom);
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
        /* Repaint after input, and ~5x/sec so animations keep moving. */
        if (changed || (icda_ticks() % 5) == 0) {
            on_draw(ud);
        }
        icda_sleep(1);
    }
}
