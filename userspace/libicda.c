#include "libicda.h"
#include "icon_data.h"



void ic_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d;
    const uint8_t *s;
    uint64_t i;
    if (!dst || !src || n == 0) return;
    d = (uint8_t *)dst;
    s = (const uint8_t *)src;
    for (i = 0; i < n; i++) d[i] = s[i];
}

void ic_memmove(void *dst, const void *src, uint64_t n) {
    uint8_t *d;
    const uint8_t *s;
    uint64_t i;
    if (!dst || !src || n == 0) return;
    d = (uint8_t *)dst;
    s = (const uint8_t *)src;
    if ((uintptr_t)d < (uintptr_t)s) {
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        i = n;
        while (i > 0) { i--; d[i] = s[i]; }
    }
}

void ic_memset(void *dst, int value, uint64_t n) {
    uint8_t *d;
    uint64_t i;
    if (!dst || n == 0) return;
    d = (uint8_t *)dst;
    for (i = 0; i < n; i++) d[i] = (uint8_t)(unsigned char)value;
}

int ic_memcmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *x;
    const uint8_t *y;
    uint64_t i;
    if (n == 0) return 0;
    if (!a || !b) return (!a && !b) ? 0 : (!a ? -1 : 1);
    x = (const uint8_t *)a;
    y = (const uint8_t *)b;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void ic_memzero(void *dst, uint64_t n) {
    ic_memset(dst, 0, n);
}



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
    uint64_t at = ic_strnlen(dst, cap);
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
        uint64_t d;
        if (s[i] < '0' || s[i] > '9') return 0;
        d = (uint64_t)(s[i] - '0');
        if (v > UINT64_MAX / 10 || (v == UINT64_MAX / 10 && d > 5)) return 0;
        v = v * 10 + d;
        i++;
    }
    *out = v;
    return 1;
}



uint64_t ic_strnlen(const char *s, uint64_t cap) {
    uint64_t n = 0;
    if (!s || cap == 0) return 0;
    while (n < cap && s[n]) n++;
    return n;
}

char *ic_strncpy(char *dst, const char *src, uint64_t n, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return dst;
    if (!src) { dst[0] = 0; return dst; }
    while (i < n && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    if (i < cap) dst[i] = 0;
    return dst;
}

uint64_t ic_strlcat(char *dst, const char *src, uint64_t cap) {
    uint64_t dlen;
    uint64_t i = 0;
    uint64_t total;

    if (!src) return 0;
    if (!dst) return ic_strlen(src);
    dlen = ic_strnlen(dst, cap);
    total = dlen + ic_strlen(src);
    if (cap == 0) return total;
    i = 0;
    while (src[i] && dlen + 1 < cap) {
        dst[dlen++] = src[i++];
    }
    if (dlen < cap) dst[dlen] = 0;
    return total;
}

uint64_t ic_snprintf_u64(char *buf, uint64_t cap, uint64_t val) {
    char tmp[32];
    uint64_t len = 0;
    uint64_t i = 0;

    if (cap == 0) {
        
        if (val == 0) return 1;
        while (val) { len++; val /= 10; }
        return len;
    }
    if (!buf) return 0;

    if (val == 0) {
        if (cap == 1) { buf[0] = 0; return 1; }
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    while (val && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (val % 10));
        val /= 10;
    }
    
    i = 0;
    while (i < len && i + 1 < cap) {
        buf[i] = tmp[len - 1 - i];
        i++;
    }
    if (i < cap) buf[i] = 0;
    return len;
}

uint64_t ic_snprintf_hex(char *buf, uint64_t cap, uint64_t val) {
    static const char hexdigits[] = "0123456789abcdef";
    char tmp[16];  
    uint64_t len = 0;
    uint64_t i;

    if (cap == 0) {
        if (val == 0) return 1;
        while (val) { len++; val >>= 4; }
        return len;
    }
    if (!buf) return 0;

    if (val == 0) {
        if (cap == 1) { buf[0] = 0; return 1; }
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    while (val && len < sizeof(tmp)) {
        tmp[len++] = hexdigits[val & 0x0F];
        val >>= 4;
    }
    i = 0;
    while (i < len && i + 1 < cap) {
        buf[i] = tmp[len - 1 - i];
        i++;
    }
    if (i < cap) buf[i] = 0;
    return len;
}

int ic_ato_u64(const char *s, uint64_t *out) {
    uint64_t v = 0;
    uint64_t i = 0;

    if (!s || !*s || !out) return 0;
    while (s[i]) {
        uint64_t d;
        if (s[i] < '0' || s[i] > '9') return 0;
        d = (uint64_t)(s[i] - '0');
        
        if (v > UINT64_MAX / 10 || (v == UINT64_MAX / 10 && d > 5)) {
            v = UINT64_MAX;
            
            i++;
            while (s[i] >= '0' && s[i] <= '9') i++;
            *out = v;
            return 1;
        }
        v = v * 10 + d;
        i++;
    }
    *out = v;
    return 1;
}



int ic_is_digit(char c) {
    unsigned char uc = (unsigned char)c;
    return uc >= '0' && uc <= '9';
}

int ic_is_space(char c) {
    unsigned char uc = (unsigned char)c;
    return uc == ' ' || uc == '\t' || uc == '\n' ||
           uc == '\r' || uc == '\f' || uc == '\v';
}

int ic_is_alpha(char c) {
    unsigned char uc = (unsigned char)c;
    return (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z');
}



uint64_t ic_utf8_len(const char *s) {
    uint64_t count = 0;
    uint64_t i = 0;

    if (!s) return 0;
    while (s[i]) {
        unsigned char b = (unsigned char)s[i];
        
        if ((b & 0xC0) != 0x80) count++;
        i++;
    }
    return count;
}

int ic_utf8_valid(const char *s) {
    uint64_t i = 0;

    if (!s) return 1; 
    while (s[i]) {
        unsigned char b = (unsigned char)s[i];
        uint64_t need;
        uint32_t cp;
        uint64_t j;

        if (b < 0x80) {
            
            i++;
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            need = 2; cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            need = 3; cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            need = 4; cp = b & 0x07;
        } else {
            return 0; 
        }

        
        for (j = 1; j < need; j++) {
            unsigned char c2;
            if (s[i + j] == 0) return 0; 
            c2 = (unsigned char)s[i + j];
            if ((c2 & 0xC0) != 0x80) return 0; 
            cp = (cp << 6) | (c2 & 0x3F);
        }

        
        if (need == 2 && cp < 0x80) return 0;
        if (need == 3 && cp < 0x800) return 0;
        if (need == 4 && cp < 0x10000) return 0;

        
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;

        
        if (cp > 0x10FFFF) return 0;

        i += need;
    }
    return 1;
}



int ic_arena_init(ic_arena_t *a, uint8_t *buf, uint64_t cap) {
    if (!a) return -1;
    if (!buf && cap > 0) return -1;
    a->buf    = buf;
    a->cap    = cap;
    a->offset = 0;
    return 0;
}

void *ic_arena_alloc(ic_arena_t *a, uint64_t size, uint64_t align) {
    uint64_t pad;
    void *ptr;

    if (!a || !a->buf || size == 0 || align == 0) return NULL;
    
    if (align & (align - 1)) return NULL;
    if (align > IC_ARENA_ALIGN_MAX) return NULL;
    
    if (a->offset > a->cap) return NULL;

    
    pad = (align - (a->offset % align)) % align;
    
    if (pad > a->cap - a->offset) return NULL;
    if (size > a->cap - a->offset - pad) return NULL;

    ptr = a->buf + a->offset + pad;
    a->offset = a->offset + pad + size;
    return ptr;
}

void ic_arena_reset(ic_arena_t *a) {
    if (!a) return;
    a->offset = 0;
}

uint64_t ic_arena_used(const ic_arena_t *a) {
    if (!a) return 0;
    return a->offset;
}

uint64_t ic_arena_remaining(const ic_arena_t *a) {
    if (!a || a->offset >= a->cap) return 0;
    return a->cap - a->offset;
}



int ic_ring_u8_init(ic_ring_u8_t *r, uint8_t *buf, uint64_t cap) {
    if (!r) return -1;
    if (!buf || cap < 2) return -1;
    r->buf  = buf;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
    return 0;
}

int ic_ring_u8_push(ic_ring_u8_t *r, uint8_t byte) {
    uint64_t next;
    if (!r || !r->buf) return -1;
    if (r->cap < 2 || r->head >= r->cap || r->tail >= r->cap) return -1;
    next = (r->tail + 1) % r->cap;
    if (next == r->head) return -1; 
    r->buf[r->tail] = byte;
    r->tail = next;
    return 0;
}

int ic_ring_u8_pop(ic_ring_u8_t *r, uint8_t *byte_out) {
    if (!r || !r->buf || !byte_out) return -1;
    if (r->cap < 2 || r->head >= r->cap || r->tail >= r->cap) return -1;
    if (r->head == r->tail) return -1; 
    *byte_out = r->buf[r->head];
    r->head = (r->head + 1) % r->cap;
    return 0;
}

uint64_t ic_ring_u8_count(const ic_ring_u8_t *r) {
    if (!r) return 0;
    if (r->cap < 2 || r->head >= r->cap || r->tail >= r->cap) return 0;
    if (r->tail >= r->head) return r->tail - r->head;
    return r->cap - r->head + r->tail;
}

uint64_t ic_ring_u8_free_cap(const ic_ring_u8_t *r) {
    if (!r) return 0;
    if (r->cap < 2 || r->head >= r->cap || r->tail >= r->cap) return 0;
    return r->cap - 1 - ic_ring_u8_count(r);
}

void ic_ring_u8_reset(ic_ring_u8_t *r) {
    if (!r) return;
    r->head = 0;
    r->tail = 0;
}



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
    if (!c || !c->px || c->w <= 0 || c->h <= 0 || !ic_icon_valid(icon) || dw <= 0 || dh <= 0) return;
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
                uint32_t src = (uint32_t)((p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2]);
                c->px[yy * c->w + xx] = ic_color_over(c->px[yy * c->w + xx], src);
            }
        }
    }
}







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
    if (ico_u16(blob) != 0 || ico_u16(blob + 2) != 1) return -1;   
    count = ico_u16(blob + 4);
    if (count == 0) return -1;
    if (6 + (uint64_t)count * 16 > size) return -1;

    

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
            continue;   
        }
        bi_size = ico_u32(blob + off);
        if (bi_size < 40 || bi_size + 12 > bytes) continue;
        dib_h = ico_i32(blob + off + 8);
        if (dib_h == 0) continue;
        {
            uint16_t planes = ico_u16(blob + off + 12);
            bpp = ico_u16(blob + off + 14);
            if (planes != 1 || (bpp != 24 && bpp != 32)) continue;
            if (ico_u32(blob + off + 16) != 0) continue;   
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
            
            {
                uint32_t entry_bytes = ico_u32(e + 8);
                if ((uint64_t)(h - 1) * row_bytes + (uint64_t)w * (uint64_t)(bpp / 8)
                        + ico_u32(blob + off) > entry_bytes) {
                    return -1;
                }
            }
            for (y = 0; y < h; y++) {
                int src_row = dib_h > 0 ? (h - 1 - y) : y;   
                const uint8_t *row = xor_data + (uint64_t)src_row * row_bytes;
                uint8_t *dst = rgba_out + (uint64_t)y * w * 4;
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = row + (uint64_t)x * (bpp / 8);
                    if (bpp == 32) {
                        dst[x * 4 + 0] = p[2];   
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
        if (stem_len <= 4) continue;                
        stem[stem_len - 4] = 0;                     
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



int ic_read_file_b(const char *path, char *buf, uint64_t cap, uint64_t *len_out) {
    uint64_t rc;
    if (!path || !buf || cap == 0) {
        if (len_out) *len_out = 0;
        return -U_EINVAL;
    }
    rc = icda_read_file(path, buf, cap);
    if ((long)rc < 0) {
        if (len_out) *len_out = 0;
        return (int)(long)rc;
    }
    if (len_out) *len_out = rc;
    return 0;
}

int ic_write_file_b(const char *path, const char *buf, uint64_t len) {
    uint64_t rc;
    if (!path) return -U_EINVAL;
    if (len == 0) return 0;
    if (!buf) return -U_EINVAL;
    rc = icda_write_file(path, buf, len);
    if ((long)rc < 0) return (int)(long)rc;
    return 0;
}

int ic_stat_b(const char *path, icda_stat_t *out) {
    uint64_t rc;
    if (!path || !out) return -U_EINVAL;
    rc = icda_stat(path, out);
    if ((long)rc < 0) return (int)(long)rc;
    return 0;
}

int ic_getcwd_b(char *buf, uint64_t cap) {
    uint64_t rc;
    if (!buf || cap == 0) return -U_EINVAL;
    rc = icda_getcwd(buf, cap);
    if ((long)rc < 0) return (int)(long)rc;
    return 0;
}

int ic_mkdir_b(const char *path) {
    uint64_t rc;
    if (!path) return -U_EINVAL;
    rc = icda_mkdir(path);
    if ((long)rc < 0) return (int)(long)rc;
    return 0;
}

int ic_create_b(const char *path) {
    uint64_t rc;
    if (!path) return -U_EINVAL;
    rc = icda_create(path);
    if ((long)rc < 0) return (int)(long)rc;
    return 0;
}

int ic_path_join(char *dst, uint64_t cap, const char *a, const char *b) {
    uint64_t alen, blen, need;
    uint64_t bstart = 0;
    int need_sep = 0;
    uint64_t pos;

    if (!dst || cap == 0) return -U_EINVAL;
    dst[0] = 0;

    alen = a ? ic_strlen(a) : 0;
    blen = b ? ic_strlen(b) : 0;

    if (alen == 0 && blen == 0) return 0;
    if (alen == 0) {
        need = blen;
        if (need + 1 > cap) { dst[0] = 0; return -U_ENOMEM; }
        ic_memcpy(dst, b, blen);
        dst[blen] = 0;
        return 0;
    }
    if (blen == 0) {
        need = alen;
        if (need + 1 > cap) { dst[0] = 0; return -U_ENOMEM; }
        ic_memcpy(dst, a, alen);
        dst[alen] = 0;
        return 0;
    }

    if (a[alen - 1] == '/' && b[0] == '/') {
        bstart = 1;
    } else if (a[alen - 1] != '/' && b[0] != '/') {
        need_sep = 1;
    }

    need = alen + (uint64_t)need_sep + (blen - bstart);
    if (need + 1 > cap) {
        dst[0] = 0;
        return -U_ENOMEM;
    }

    ic_memcpy(dst, a, alen);
    pos = alen;
    if (need_sep) {
        dst[pos++] = '/';
    }
    ic_memcpy(dst + pos, b + bstart, blen - bstart);
    pos += blen - bstart;
    dst[pos] = 0;
    return 0;
}

int ic_path_normalize(char *dst, uint64_t cap, const char *src) {
    uint64_t di = 0;
    uint64_t si = 0;
    int is_abs;

    if (!dst || cap == 0) return -U_ENOMEM;
    dst[0] = 0;
    if (!src || !*src) return 0;

    is_abs = (src[0] == '/');
    if (is_abs) {
        dst[0] = '/';
        di = 1;
        si = 1;
    }

    for (;;) {
        uint64_t comp_start, comp_len;

        while (src[si] == '/') si++;
        if (!src[si]) break;

        comp_start = si;
        while (src[si] && src[si] != '/') si++;
        comp_len = si - comp_start;

        if (comp_len == 1 && src[comp_start] == '.') {
            continue;
        }
        if (comp_len == 2 && src[comp_start] == '.' && src[comp_start + 1] == '.') {
            if (di > (uint64_t)(is_abs ? 1 : 0)) {
                di--;
                while (di > (uint64_t)(is_abs ? 1 : 0) && dst[di - 1] != '/') {
                    di--;
                }
            }
            continue;
        }

        if (di > 0 && !(di == 1 && dst[0] == '/')) {
            if (di >= cap) { dst[0] = 0; return -U_ENOMEM; }
            dst[di++] = '/';
        }
        if (di + comp_len >= cap) { dst[0] = 0; return -U_ENOMEM; }
        ic_memcpy(dst + di, src + comp_start, comp_len);
        di += comp_len;
    }

    dst[di] = 0;
    return 0;
}

int ic_list_dir_b(const char *path, char *buf, uint64_t cap, uint64_t *len_out) {
    uint64_t rc;
    if (!path || !buf || cap == 0) {
        if (len_out) *len_out = 0;
        return -U_EINVAL;
    }
    rc = icda_list_dir(path, buf, cap);
    if ((long)rc < 0) {
        if (len_out) *len_out = 0;
        return (int)(long)rc;
    }
    if (len_out) *len_out = rc;
    return 0;
}

void ic_dir_cursor_init(ic_dir_cursor_t *cur, const char *buf, uint64_t len) {
    if (!cur) return;
    cur->buf = buf;
    cur->len = buf ? len : 0;
    cur->pos = 0;
}

int ic_dir_next(ic_dir_cursor_t *cur, const char **name_out,
                uint64_t *name_len_out, int *is_dir_out) {
    uint64_t start, end, i;

    if (!cur || !cur->buf || cur->pos >= cur->len) {
        if (name_out) *name_out = (const char *)0;
        if (name_len_out) *name_len_out = 0;
        if (is_dir_out) *is_dir_out = 0;
        return 0;
    }

    start = cur->pos;
    i = start;
    while (i < cur->len && cur->buf[i] != '\n' && cur->buf[i] != '\0') {
        i++;
    }
    end = i;

    if (i < cur->len && cur->buf[i] == '\n') {
        cur->pos = i + 1;
    } else {
        cur->pos = i;
    }

    if (end <= start) {
        if (name_out) *name_out = (const char *)0;
        if (name_len_out) *name_len_out = 0;
        if (is_dir_out) *is_dir_out = 0;
        return 0;
    }

    if (name_out) *name_out = cur->buf + start;
    if (name_len_out) *name_len_out = end - start;
    if (is_dir_out) *is_dir_out = (cur->buf[end - 1] == '/');
    return 1;
}



uint64_t ic_spawn_b(const char *path) {
    if (!path || !*path) return (uint64_t)(-((long)U_EINVAL));
    return icda_spawn(path);
}

uint64_t ic_spawn_args_b(const char *path, const char *args) {
    if (!path || !*path) return (uint64_t)(-((long)U_EINVAL));
    if (!args) args = "";
    return icda_spawn_args(path, args);
}

int ic_wait_b(uint64_t pid) {
    return (int)icda_waitpid(pid);
}

void ic_sleep_ticks(uint64_t ticks) {
    icda_sleep(ticks);
}

uint64_t ic_ticks_b(void) {
    return icda_ticks();
}

void ic_yield_b(void) {
    icda_yield();
}

_Noreturn void ic_exit_b(uint64_t code) {
    icda_exit(code);
    for (;;) {}
}

int ic_shm_acquire(uint64_t size, ic_shm_t *out) {
    uint64_t handle, addr;

    if (!out || size == 0) return -U_EINVAL;
    out->handle = 0;
    out->addr   = 0;
    out->size   = 0;
    out->valid  = 0;

    handle = icda_shm_create(size);
    if ((long)handle < 0) return (int)(long)handle;

    addr = icda_shm_map(handle);
    if ((long)addr < 0) {
        icda_shm_close(handle);
        return (int)(long)addr;
    }

    out->handle = handle;
    out->addr   = addr;
    out->size   = size;
    out->valid  = 1;
    return 0;
}

void ic_shm_release(ic_shm_t *t) {
    if (!t || !t->valid) return;
    t->valid = 0;
    icda_shm_unmap(t->handle);
    icda_shm_close(t->handle);
    t->handle = 0;
    t->addr   = 0;
    t->size   = 0;
}

uint64_t ic_msg_open_b(const char *name) {
    if (!name || !*name) return (uint64_t)(-((long)U_EINVAL));
    return icda_msg_open(name);
}

int ic_msg_send_b(uint64_t handle, const void *msg) {
    if (!msg) return -U_EINVAL;
    return icda_msg_send(handle, msg);
}

int ic_msg_recv_b(uint64_t handle, void *out, int block) {
    if (!out) return -U_EINVAL;
    return icda_msg_recv(handle, out, block);
}

int ic_msg_poll_b(uint64_t handle) {
    return icda_msg_poll(handle);
}



static int ic_try_parse_ipv4(const char *host, uint32_t *ip_out) {
    const char *p = host;
    uint32_t octets[4];
    int o;
    uint32_t v;

    if (!host || !*host || !ip_out) return -1;

    for (o = 0; o < 4; o++) {
        v = 0;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') {
            v = v * 10U + (uint32_t)(*p - '0');
            if (v > 255U) return -1;
            p++;
        }
        octets[o] = v;
        if (o < 3) {
            if (*p != '.') return -1;
            p++;
        }
    }
    if (*p != 0) return -1;
    *ip_out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

int ic_url_split(const char *url, char *host_out, uint64_t host_cap,
                 uint16_t *port_out, char *path_out, uint64_t path_cap,
                 int *use_tls_out) {
    const char *host;
    uint64_t host_len = 0;
    uint64_t path_len = 0;
    uint16_t port = 80;
    int use_tls = 0;

    if (!url || !host_out || host_cap == 0 || !port_out ||
        !path_out || path_cap == 0 || !use_tls_out) {
        return -1;
    }
    host_out[0] = 0;
    path_out[0] = 0;

    if (ic_strprefix(url, "https://")) {
        host = url + 8;
        port = 443;
        use_tls = 1;
    } else if (ic_strprefix(url, "http://")) {
        host = url + 7;
    } else {
        return -1;
    }

    while (host[host_len] && host[host_len] != ':' && host[host_len] != '/') {
        if (host_len + 1 >= host_cap) return -1;
        host_out[host_len] = host[host_len];
        host_len++;
    }
    if (host_len == 0) return -1;
    host_out[host_len] = 0;
    host += host_len;

    if (*host == ':') {
        uint32_t port_value = 0;
        host++;
        if (*host < '0' || *host > '9') return -1;
        while (*host >= '0' && *host <= '9') {
            port_value = port_value * 10U + (uint32_t)(*host - '0');
            if (port_value > 65535U) return -1;
            host++;
        }
        if (port_value == 0) return -1;
        port = (uint16_t)port_value;
    }

    if (*host == 0) {
        ic_strcpy(path_out, "/", path_cap);
    } else {
        if (*host != '/') return -1;
        while (host[path_len] && path_len + 1 < path_cap) {
            path_out[path_len] = host[path_len];
            path_len++;
        }
        if (host[path_len] != 0) {
            host_out[0] = path_out[0] = 0;
            return -1;
        }
        path_out[path_len] = 0;
    }

    *port_out = port;
    *use_tls_out = use_tls;
    return 0;
}

int ic_dns_b(const char *host, uint32_t *ipv4_out) {
    long rc;
    if (!host || !*host || !ipv4_out) return -U_EINVAL;

    if (ic_try_parse_ipv4(host, ipv4_out) == 0) return 0;

    rc = (long)icda_dns_resolve(host, ipv4_out);
    return (int)rc;
}

int ic_http_fetch_to_file(const char *host, uint16_t port, int use_tls,
                          const char *path, const char *out_path,
                          uint64_t *bytes_out) {
    uint32_t ip = 0;
    long rc;

    if (!host || !*host || !path || !*path || !out_path) return -U_EINVAL;
    if (bytes_out) *bytes_out = 0;

    rc = (long)ic_dns_b(host, &ip);
    if (rc < 0) return (int)rc;

    if (use_tls) {
        rc = (long)icda_https_get_ipv4(ip, port, host, path, out_path, bytes_out);
    } else {
        rc = (long)icda_http_get_ipv4(ip, port, host, path, out_path, bytes_out);
    }
    return (int)rc;
}

int ic_http_fetch_mem(const char *url, char *buf, uint64_t cap,
                      uint64_t *len_out, const char *scratch_path) {
    char host[128];
    char path[256];
    uint16_t port = 80;
    int use_tls = 0;
    const char *sp;
    int rc;

    if (!url || !buf || cap == 0) {
        if (len_out) *len_out = 0;
        return -U_EINVAL;
    }
    if (len_out) *len_out = 0;

    sp = scratch_path ? scratch_path : "/tmp/.ic_fetch";

    rc = ic_url_split(url, host, sizeof(host), &port, path, sizeof(path), &use_tls);
    if (rc < 0) return rc;

    rc = ic_http_fetch_to_file(host, port, use_tls, path, sp, (uint64_t *)0);
    if (rc < 0) return rc;

    rc = ic_read_file_b(sp, buf, cap, len_out);
    return rc;
}



int ic_layout_row(ic_rect_t parent, int pad, int gap,
                  const int *widths, int count,
                  ic_rect_t *out, int out_cap) {
    int i, total_fixed = 0, flex_count = 0, flex_w, x, remaining;
    if (!out || out_cap <= 0) return -U_ENOMEM;
    if (!widths || count <= 0) return 0;
    if (count > out_cap) return -U_ENOMEM;

    for (i = 0; i < count; i++) {
        if (widths[i] < 0) flex_count++;
        else total_fixed += widths[i];
    }

    remaining = parent.w - 2 * pad - total_fixed - (count > 1 ? gap * (count - 1) : 0);
    if (flex_count > 0) {
        flex_w = remaining / flex_count;
        if (flex_w < 0) flex_w = 0;
    } else {
        flex_w = 0;
    }

    x = parent.x + pad;
    for (i = 0; i < count; i++) {
        int cw = widths[i] < 0 ? flex_w : widths[i];
        out[i].x = x;
        out[i].y = parent.y + pad;
        out[i].w = cw;
        out[i].h = parent.h - 2 * pad;
        x += cw;
        if (i + 1 < count) x += gap;
    }
    return 0;
}

int ic_layout_col(ic_rect_t parent, int pad, int gap,
                  const int *heights, int count,
                  ic_rect_t *out, int out_cap) {
    int i, total_fixed = 0, flex_count = 0, flex_h, y, remaining;
    if (!out || out_cap <= 0) return -U_ENOMEM;
    if (!heights || count <= 0) return 0;
    if (count > out_cap) return -U_ENOMEM;

    for (i = 0; i < count; i++) {
        if (heights[i] < 0) flex_count++;
        else total_fixed += heights[i];
    }

    remaining = parent.h - 2 * pad - total_fixed - (count > 1 ? gap * (count - 1) : 0);
    if (flex_count > 0) {
        flex_h = remaining / flex_count;
        if (flex_h < 0) flex_h = 0;
    } else {
        flex_h = 0;
    }

    y = parent.y + pad;
    for (i = 0; i < count; i++) {
        int ch = heights[i] < 0 ? flex_h : heights[i];
        out[i].x = parent.x + pad;
        out[i].y = y;
        out[i].w = parent.w - 2 * pad;
        out[i].h = ch;
        y += ch;
        if (i + 1 < count) y += gap;
    }
    return 0;
}

const char *ic_version_label(void) {
    static char label[40];
    if (!label[0]) {
        char rel[512];
        long n = (long)icda_read_file("/etc/icda-release.txt", rel, sizeof(rel) - 1);
        ic_strcpy(label, "Version " IC_VERSION_STRING, sizeof(label));
        if (n > 0) {
            rel[n] = 0;
            for (long i = 0; i + 8 < n; i++) {
                if ((i == 0 || rel[i - 1] == '\n') && ic_memcmp(rel + i, "version ", 8) == 0) {
                    long e = i + 8, k = 8;
                    while (e < n && rel[e] != '\n' && k + 1 < (long)sizeof(label)) label[k++] = rel[e++];
                    label[k] = 0;
                    break;
                }
            }
        }
    }
    return label;
}
