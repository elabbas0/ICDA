#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "smath.h"
#define STBTT_ifloor(x) ((int)sm_floor(x))
#define STBTT_iceil(x) ((int)sm_ceil(x))
#define STBTT_sqrt(x) sm_sqrt(x)
#define STBTT_pow(x, y) sm_pow(x, y)
#define STBTT_fmod(x, y) sm_fmod(x, y)
#define STBTT_cos(x) sm_cos(x)
#define STBTT_acos(x) sm_acos(x)
#define STBTT_fabs(x) sm_fabs(x)
#include "third_party/stb_truetype.h"

static const char *const face_files[FACE_COUNT] = {
    "/usr/share/fonts/Inter-Regular.ttf",
    "/usr/share/fonts/Inter-SemiBold.ttf",
    "/usr/share/fonts/JetBrainsMono-Regular.ttf"
};

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    int            ok;
    float          ascent, descent, gap;   /* per em */
    float          upem_scale;             /* stbtt scale for 1 px em */
    /* advance width cache for the first 0x3000 code points, per em (0 = unknown) */
    float         *adv;
} face_t;

static face_t faces[FACE_COUNT];
static int fonts_ready;

static unsigned char *read_file(const char *path, long *size_out) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long size;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)size);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = 0;
    }
    fclose(f);
    if (size_out) *size_out = size;
    return buf;
}

int font_init(void) {
    int loaded = 0;
    if (fonts_ready) return 0;
    for (int i = 0; i < FACE_COUNT; i++) {
        face_t *fc = &faces[i];
        int asc, desc, gap;
        fc->data = read_file(face_files[i], 0);
        if (!fc->data || !stbtt_InitFont(&fc->info, fc->data, stbtt_GetFontOffsetForIndex(fc->data, 0))) continue;
        stbtt_GetFontVMetrics(&fc->info, &asc, &desc, &gap);
        fc->upem_scale = stbtt_ScaleForMappingEmToPixels(&fc->info, 1.0f);
        fc->ascent = asc * fc->upem_scale;
        fc->descent = desc * fc->upem_scale;
        fc->gap = gap * fc->upem_scale;
        fc->adv = (float *)calloc(0x3000, sizeof(float));
        fc->ok = 1;
        loaded++;
    }
    fonts_ready = 1;
    return loaded ? 0 : -1;
}

static int contains(const char *s, const char *word) {
    return s && strstr(s, word) != 0;
}

font_t font_pick(const char *family, int weight, int italic, float size) {
    font_t f;
    int mono = contains(family, "mono") || contains(family, "courier") || contains(family, "consol") ||
               contains(family, "menlo") || contains(family, "code");
    font_init();
    f.face = mono ? FACE_MONO : weight >= 600 ? FACE_SANS_BOLD : FACE_SANS;
    if (!faces[f.face].ok) f.face = FACE_SANS;
    f.size = size;
    f.italic = italic;
    if (faces[f.face].ok) {
        f.ascent = faces[f.face].ascent * size;
        f.descent = faces[f.face].descent * size;
        f.line_gap = faces[f.face].gap * size;
    } else {
        f.ascent = size * 0.8f;
        f.descent = -size * 0.2f;
        f.line_gap = 0;
    }
    return f;
}

uint32_t utf8_next(const char **sp, const char *end) {
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t cp;
    int extra;
    if (s[0] < 0x80) {
        *sp += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else {
        *sp += 1;
        return 0xFFFD;
    }
    if ((const char *)s + extra >= end) {
        *sp = end;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *sp += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *sp += extra + 1;
    return cp;
}

float font_advance(const font_t *f, uint32_t cp) {
    face_t *fc = &faces[f->face];
    float per_em;
    if (!fc->ok) return f->size * 0.5f;
    if (cp < 0x3000 && fc->adv[cp] != 0) return fc->adv[cp] * f->size;
    {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&fc->info, (int)cp, &adv, &lsb);
        per_em = adv * fc->upem_scale;
        if (per_em <= 0 && cp != 0x200B && cp != 0xAD) per_em = 0.5f;
    }
    if (cp < 0x3000) fc->adv[cp] = per_em > 0 ? per_em : 1e-6f;
    return per_em * f->size;
}

float font_text_width(const font_t *f, const char *s, size_t n) {
    const char *end = s + n;
    float w = 0;
    while (s < end) w += font_advance(f, utf8_next(&s, end));
    return w;
}

/* ---- glyph cache -------------------------------------------------------- */

#define GCACHE 4096

typedef struct {
    uint32_t cp;
    uint16_t px;        /* device size * 4 */
    uint8_t  face, italic, used;
    glyph_t  g;
} gentry_t;

static gentry_t gcache[GCACHE];
static int gcache_count;

static void gcache_clear(void) {
    for (int i = 0; i < GCACHE; i++) {
        if (gcache[i].used) free(gcache[i].g.alpha);
        gcache[i].used = 0;
    }
    gcache_count = 0;
}

const glyph_t *font_glyph(const font_t *f, uint32_t cp, float scale) {
    face_t *fc = &faces[f->face];
    float px = f->size * scale;
    uint16_t key_px = (uint16_t)(px * 4 + 0.5f);
    uint32_t h = (cp * 2654435761U) ^ ((uint32_t)key_px << 16) ^ ((uint32_t)f->face << 3) ^ (uint32_t)f->italic;
    gentry_t *e;
    if (!fc->ok || px < 1 || px > 600) return 0;
    for (int probe = 0; probe < 16; probe++) {
        e = &gcache[(h + (uint32_t)probe) % GCACHE];
        if (!e->used) break;
        if (e->cp == cp && e->px == key_px && e->face == f->face && e->italic == f->italic) return &e->g;
    }
    if (!e->used || gcache_count > GCACHE * 3 / 4) {
        if (gcache_count > GCACHE * 3 / 4) {
            gcache_clear();
            e = &gcache[h % GCACHE];
        }
    } else {
        /* probe chain full: overwrite the home slot */
        e = &gcache[h % GCACHE];
        free(e->g.alpha);
        e->used = 0;
        gcache_count--;
    }
    {
        float s = stbtt_ScaleForMappingEmToPixels(&fc->info, px);
        int x0, y0, x1, y1, w, hgt;
        unsigned char *bmp;
        stbtt_GetCodepointBitmapBox(&fc->info, (int)cp, s, s, &x0, &y0, &x1, &y1);
        w = x1 - x0;
        hgt = y1 - y0;
        memset(&e->g, 0, sizeof(e->g));
        if (w > 0 && hgt > 0 && w < 1024 && hgt < 1024) {
            int shear = f->italic ? (int)(hgt * 0.2f) + 1 : 0;
            bmp = (unsigned char *)calloc((size_t)(w + shear) * (size_t)hgt, 1);
            if (bmp) {
                if (shear) {
                    unsigned char *tmp = (unsigned char *)malloc((size_t)w * (size_t)hgt);
                    if (tmp) {
                        stbtt_MakeCodepointBitmap(&fc->info, tmp, w, hgt, w, s, s, (int)cp);
                        for (int y = 0; y < hgt; y++) {
                            int dx = (int)((hgt - 1 - y) * 0.2f);
                            memcpy(bmp + (size_t)y * (size_t)(w + shear) + dx, tmp + (size_t)y * (size_t)w, (size_t)w);
                        }
                        free(tmp);
                    }
                } else {
                    stbtt_MakeCodepointBitmap(&fc->info, bmp, w, hgt, w, s, s, (int)cp);
                }
                e->g.alpha = bmp;
                e->g.w = w + shear;
                e->g.h = hgt;
                e->g.xoff = x0;
                e->g.yoff = y0;
            }
        }
    }
    e->cp = cp;
    e->px = key_px;
    e->face = (uint8_t)f->face;
    e->italic = (uint8_t)f->italic;
    e->used = 1;
    gcache_count++;
    return &e->g;
}
