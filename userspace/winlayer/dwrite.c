/* DirectWrite (dwrite.dll), on FreeType.
 *
 * Programs that draw their own text (Chrome, through Skia) ask DirectWrite
 * for fonts, glyph outlines and glyph coverage.  The system font collection
 * is the font files in ICDA's /usr/share/fonts, the Linux root's
 * (/linux/usr/share/fonts), the prefix's C:\Windows\Fonts and the folders
 * in ICDA_WIN_FONTS, grouped into families as DirectWrite groups them (by
 * the font's WWS, typographic or plain family name).  Windows' own family
 * names (Segoe UI, Arial, Times New Roman, Consolas ...) find the closest
 * family there is.
 *
 * Names and metrics come from the fonts' own tables; FreeType reads the
 * glyphs: outlines for GetGlyphRunOutline, anti-aliased coverage for glyph
 * run analyses (no hinting, as DirectWrite's natural modes).  Fonts a
 * program brings (Skia's web fonts) come through its own loaders and
 * collections.  Not here: text layout (IDWriteTextFormat, IDWriteTextLayout),
 * the text analyzer, font sets (IDWriteFactory3 and later) and color glyphs.
 *
 * The objects are COM objects in C: a vtable pointer, a reference count,
 * then their own fields.  An interface that extends another (IDWriteFactory2
 * extends IDWriteFactory1, which extends IDWriteFactory) shares its vtable,
 * so QueryInterface hands back the same pointer.  One lock guards FreeType
 * and the shared tables; a program's own objects (loaders, streams, sinks)
 * are called without it. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_ADVANCES_H

typedef float FLOAT;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef int16_t INT16;
typedef uint64_t UINT64;

#define HR(x)                     ((HRESULT)(x))
#define S_OK_                     0
#define S_FALSE_                  1
#define E_NOTIMPL_                HR(0x80004001)
#define E_NOINTERFACE_            HR(0x80004002)
#define E_POINTER_                HR(0x80004003)
#define E_FAIL_                   HR(0x80004005)
#define E_OUTOFMEMORY_            HR(0x8007000E)
#define E_INVALIDARG_             HR(0x80070057)
#define E_NOT_SUFFICIENT_BUFFER_  HR(0x8007007A)
#define DWRITE_E_FILEFORMAT       HR(0x88985000)
#define DWRITE_E_NOFONT           HR(0x88985002)
#define DWRITE_E_FILENOTFOUND     HR(0x88985003)
#define DWRITE_E_ALREADYREGISTERED HR(0x88985006)
#define DWRITE_E_UNSUPPORTEDOP    HR(0x88985009)
#define DWRITE_E_NOCOLOR          HR(0x8898500C)

/* ---- DirectWrite's types ------------------------------------------------------------ */

typedef struct {
    UINT16 designUnitsPerEm, ascent, descent;
    INT16  lineGap;
    UINT16 capHeight, xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
} DWRITE_FONT_METRICS;

typedef struct {
    DWRITE_FONT_METRICS m;
    INT16 glyphBoxLeft, glyphBoxTop, glyphBoxRight, glyphBoxBottom;
    INT16 subscriptPositionX, subscriptPositionY, subscriptSizeX, subscriptSizeY;
    INT16 superscriptPositionX, superscriptPositionY, superscriptSizeX, superscriptSizeY;
    BOOL  hasTypographicMetrics;
} DWRITE_FONT_METRICS1;
_Static_assert(sizeof(DWRITE_FONT_METRICS) == 20 && sizeof(DWRITE_FONT_METRICS1) == 48, "DWRITE_FONT_METRICS");

typedef struct {
    INT32  leftSideBearing;
    UINT32 advanceWidth;
    INT32  rightSideBearing, topSideBearing;
    UINT32 advanceHeight;
    INT32  bottomSideBearing, verticalOriginY;
} DWRITE_GLYPH_METRICS;

typedef struct { FLOAT advanceOffset, ascenderOffset; } DWRITE_GLYPH_OFFSET;

typedef struct {
    void                      *fontFace;
    FLOAT                      fontEmSize;
    UINT32                     glyphCount;
    const UINT16              *glyphIndices;
    const FLOAT               *glyphAdvances;
    const DWRITE_GLYPH_OFFSET *glyphOffsets;
    BOOL                       isSideways;
    UINT32                     bidiLevel;
} DWRITE_GLYPH_RUN;
_Static_assert(sizeof(DWRITE_GLYPH_RUN) == 48, "DWRITE_GLYPH_RUN");

typedef struct { FLOAT m11, m12, m21, m22, dx, dy; } DWRITE_MATRIX;
typedef struct { UINT32 first, last; } DWRITE_UNICODE_RANGE;
typedef struct { INT16 slopeRise, slopeRun, offset; } DWRITE_CARET_METRICS;
typedef struct { FLOAT x, y; } D2D1_POINT_2F;
typedef struct { D2D1_POINT_2F point1, point2, point3; } D2D1_BEZIER_SEGMENT;

typedef struct {
    LONG  lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE  lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} DW_LOGFONTW;

enum { FACE_CFF = 0, FACE_TRUETYPE = 1, FACE_COLLECTION = 2 };
enum { FILE_UNKNOWN = 0, FILE_CFF = 1, FILE_TRUETYPE = 2, FILE_COLLECTION = 3 };
enum { SIM_BOLD = 1, SIM_OBLIQUE = 2 };
enum { STYLE_NORMAL = 0, STYLE_OBLIQUE = 1, STYLE_ITALIC = 2 };
enum { RM_DEFAULT = 0, RM_ALIASED = 1, RM_GDI_CLASSIC = 2, RM_GDI_NATURAL = 3, RM_NATURAL = 4,
       RM_NATURAL_SYMMETRIC = 5, RM_OUTLINE = 6 };
enum { TEX_ALIASED_1x1 = 0, TEX_CLEARTYPE_3x1 = 1 };
enum { AA_CLEARTYPE = 0, AA_GRAYSCALE = 1 };

/* ---- interface ids -------------------------------------------------------------------- */

#define IID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const GUID name = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
IID(IID_IUnknown_, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
IID(IID_Factory, 0xb859ee5a, 0xd838, 0x4b5b, 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48);
IID(IID_Factory1, 0x30572f99, 0xdac6, 0x41db, 0xa1, 0x6e, 0x04, 0x86, 0x30, 0x7e, 0x60, 0x6a);
IID(IID_Factory2, 0x0439fc60, 0xca44, 0x4994, 0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec);
IID(IID_FontCollection, 0xa84cee02, 0x3eea, 0x4eee, 0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc);
IID(IID_FontList, 0x1a0d8438, 0x1d97, 0x4ec1, 0xae, 0xf9, 0xa2, 0xfb, 0x86, 0xed, 0x6a, 0xcb);
IID(IID_FontFamily, 0xda20d8ef, 0x812a, 0x4c43, 0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xdd);
IID(IID_Font, 0xacd16696, 0x8c14, 0x4f5d, 0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x37);
IID(IID_Font1, 0xacd16696, 0x8c14, 0x4f5d, 0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x38);
IID(IID_Font2, 0x29748ed6, 0x8c9c, 0x4a6a, 0xbe, 0x0b, 0xd9, 0x12, 0xe8, 0x53, 0x89, 0x44);
IID(IID_FontFace, 0x5f49804d, 0x7024, 0x4d43, 0xbf, 0xa9, 0xd2, 0x59, 0x84, 0xf5, 0x38, 0x49);
IID(IID_FontFace1, 0xa71efdb4, 0x9fdb, 0x4838, 0xad, 0x90, 0xcf, 0xc3, 0xbe, 0x8c, 0x3d, 0xaf);
IID(IID_FontFace2, 0xd8b768ff, 0x64bc, 0x4e66, 0x98, 0x2b, 0xec, 0x8e, 0x87, 0xf6, 0x93, 0xf7);
IID(IID_LocalizedStrings, 0x08256209, 0x099a, 0x4b34, 0xb8, 0x6d, 0xc2, 0x2b, 0x11, 0x0e, 0x77, 0x71);
IID(IID_FontFile, 0x739d886a, 0xcef5, 0x47dc, 0x87, 0x69, 0x1a, 0x8b, 0x41, 0xbe, 0xbb, 0xb0);
IID(IID_FontFileLoader, 0x727cad4e, 0xd6af, 0x4c9e, 0x8a, 0x08, 0xd6, 0x95, 0xb1, 0x1c, 0xaa, 0x49);
IID(IID_LocalFontFileLoader, 0xb2d9f3ec, 0xc9fe, 0x4a11, 0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2);
IID(IID_FontFileStream, 0x6d4865fe, 0x0ab8, 0x4d91, 0x8f, 0x62, 0x5d, 0xd6, 0xbe, 0x34, 0xa3, 0xe0);
IID(IID_GlyphRunAnalysis, 0x7d97dbf7, 0xe085, 0x42d4, 0x81, 0xe3, 0x6a, 0x88, 0x3b, 0xde, 0xd1, 0x18);
IID(IID_RenderingParams, 0x2f0da53a, 0x2add, 0x47cd, 0x82, 0xee, 0xd9, 0xec, 0x34, 0x68, 0x8e, 0x75);
IID(IID_RenderingParams1, 0x94413cf4, 0xa6fc, 0x4248, 0x8b, 0x50, 0x66, 0x74, 0x34, 0x8f, 0xca, 0xd3);
IID(IID_RenderingParams2, 0xf9d711c3, 0x9777, 0x40ae, 0x87, 0xe8, 0x3e, 0x5a, 0xf9, 0xbf, 0x09, 0x48);
IID(IID_RenderingParams3, 0xb7924baa, 0x391b, 0x412a, 0x8c, 0x5c, 0xe4, 0x4c, 0xc2, 0xd8, 0x67, 0xdc);
IID(IID_FontFallback, 0xefa008f9, 0xf7a1, 0x48bf, 0xb0, 0x5c, 0xf2, 0x24, 0x71, 0x3c, 0xc0, 0xff);
IID(IID_GdiInterop, 0x1edd9491, 0x9853, 0x4299, 0x89, 0x8f, 0x64, 0x32, 0x98, 0x3b, 0x6f, 0x3a);
IID(IID_NumberSubstitution, 0x14885cc9, 0xbab0, 0x4f90, 0xb6, 0xed, 0x5c, 0x36, 0x6a, 0x2c, 0xd0, 0x3d);

static int iid_is(const GUID *a, const GUID *b) { return a && !memcmp(a, b, sizeof(GUID)); }

/* ---- COM plumbing ---------------------------------------------------------------------- */

/* every object of ours starts so */
typedef struct {
    void *const  *vt;
    volatile LONG refs;
} obj_t;

static ULONG WINAPI obj_AddRef(void *self) {
    return (ULONG)__atomic_add_fetch(&((obj_t *)self)->refs, 1, __ATOMIC_ACQ_REL);
}
static LONG obj_unref(void *self) { return __atomic_sub_fetch(&((obj_t *)self)->refs, 1, __ATOMIC_ACQ_REL); }

/* a program's object (or ours): its vtable */
#define VTBL(o) (*(void *const *const *)(o))
typedef ULONG (WINAPI *unk_fn)(void *);
static void com_addref(void *o) { if (o) ((unk_fn)VTBL(o)[1])(o); }
static void com_release(void *o) { if (o) ((unk_fn)VTBL(o)[2])(o); }

static HRESULT notimpl(const char *what, void **out) {
    if (out) *out = 0;
    if (wtrace) wlog("dwrite: %s is not implemented", what);
    return E_NOTIMPL_;
}

static pthread_mutex_t dw_mu;
static pthread_once_t dw_once = PTHREAD_ONCE_INIT;
static FT_Library ft_lib;

static void dw_init_once(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&dw_mu, &a);
    if (FT_Init_FreeType(&ft_lib)) {
        ft_lib = 0;
        wlog("dwrite: FreeType did not start");
    }
}
static void lock(void) { pthread_once(&dw_once, dw_init_once); pthread_mutex_lock(&dw_mu); }
static void unlock(void) { pthread_mutex_unlock(&dw_mu); }

/* ---- small helpers -------------------------------------------------------------------- */

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void wcopy(WCHAR *dst, size_t cap, const WCHAR *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void wfrom_ascii(WCHAR *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
}

static int wieq_ascii(const WCHAR *w, const char *a) {
    for (; *w && *a; w++, a++) {
        WCHAR c = *w >= 'A' && *w <= 'Z' ? (WCHAR)(*w + 32) : *w;
        char d = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (c != (WCHAR)(unsigned char)d) return 0;
    }
    return !*w && !*a;
}

static int wieq(const WCHAR *a, const WCHAR *b) {
    for (; *a && *b; a++, b++) {
        WCHAR c = *a >= 'A' && *a <= 'Z' ? (WCHAR)(*a + 32) : *a;
        WCHAR d = *b >= 'A' && *b <= 'Z' ? (WCHAR)(*b + 32) : *b;
        if (c != d) return 0;
    }
    return !*a && !*b;
}

/* ---- font file data --------------------------------------------------------------------- */

/* a font file's bytes, shared by the files, streams and faces that read it */
typedef struct fdata {
    struct fdata  *next;           /* the local files read, by path */
    volatile LONG  refs;
    char           upath[512];     /* a local file's Unix path, "" for a program's stream */
    const uint8_t *bytes;
    size_t         size;
    int            mapped;
    uint64_t       mtime;          /* FILETIME */
} fdata_t;

static fdata_t *local_datas;

static void fdata_unref(fdata_t *d) {
    if (!d || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (d->mapped) munmap((void *)d->bytes, d->size);
    else free((void *)d->bytes);
    free(d);
}

/* a local font file, read (mapped) once and kept while used */
static fdata_t *fdata_local(const char *upath) {
    fdata_t *d;
    struct stat st;
    int fd;
    void *p;
    lock();
    for (d = local_datas; d; d = d->next)
        if (!strcmp(d->upath, upath)) {
            __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
            unlock();
            return d;
        }
    unlock();
    fd = open(upath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || st.st_size < 12 || st.st_size > (1 << 30)) {
        close(fd);
        return 0;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return 0;
    }
    p = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) {
        d->mapped = 1;
    } else {
        size_t got = 0;
        p = malloc((size_t)st.st_size);
        while (p && got < (size_t)st.st_size) {
            ssize_t n = pread(fd, (char *)p + got, (size_t)st.st_size - got, (off_t)got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (!p || got != (size_t)st.st_size) {
            free(p);
            free(d);
            close(fd);
            return 0;
        }
    }
    close(fd);
    d->bytes = p;
    d->size = (size_t)st.st_size;
    d->refs = 2;                     /* the caller's and the list's */
    unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, (FILETIME *)&d->mtime);
    snprintf(d->upath, sizeof(d->upath), "%s", upath);
    lock();
    d->next = local_datas;
    local_datas = d;
    unlock();
    return d;
}

/* ---- reading a font's tables ------------------------------------------------------------- */

/* the bytes of a font (in memory), or a file read piecewise while the system
 * collection is listed (so listing reads a few kilobytes of each font) */
typedef struct {
    const uint8_t *mem;
    size_t         size;
    int            fd;
} src_t;

static int src_read(const src_t *s, uint64_t off, void *buf, size_t len) {
    if (s->mem) {
        if (off > s->size || len > s->size - off) return -1;
        memcpy(buf, s->mem + off, len);
        return 0;
    }
    return pread(s->fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

/* where face `index` of a font file starts; -1 if it is not a font */
static int64_t face_offset(const src_t *s, UINT32 index, UINT32 *faces, int *collection) {
    uint8_t h[16];
    if (src_read(s, 0, h, 12) != 0) return -1;
    *collection = !memcmp(h, "ttcf", 4);
    if (*collection) {
        uint8_t o[4];
        *faces = be32(h + 8);
        if (!*faces || index >= *faces || *faces > 4096) return -1;
        if (src_read(s, 12 + 4 * (uint64_t)index, o, 4) != 0) return -1;
        return be32(o);
    }
    *faces = 1;
    if (index != 0) return -1;
    if (be32(h) != 0x00010000 && memcmp(h, "OTTO", 4) && memcmp(h, "true", 4)) return -1;
    return 0;
}

/* a table of the face at `start`: its offset and length, 0 if it is not there */
static int find_table(const src_t *s, int64_t start, const char tag[4], uint32_t *off, uint32_t *len) {
    uint8_t h[12], rec[16];
    uint16_t n;
    if (start < 0 || src_read(s, (uint64_t)start, h, 12) != 0) return 0;
    n = be16(h + 4);
    for (uint16_t i = 0; i < n && i < 512; i++) {
        if (src_read(s, (uint64_t)start + 12 + 16 * (uint64_t)i, rec, 16) != 0) return 0;
        if (!memcmp(rec, tag, 4)) {
            *off = be32(rec + 8);
            *len = be32(rec + 12);
            return 1;
        }
    }
    return 0;
}

static uint8_t *load_table(const src_t *s, int64_t start, const char tag[4], uint32_t *len) {
    uint32_t off;
    uint8_t *b;
    if (!find_table(s, start, tag, &off, len) || *len > (16u << 20)) return 0;
    b = malloc(*len ? *len : 1);
    if (b && src_read(s, off, b, *len) != 0) {
        free(b);
        return 0;
    }
    return b;
}

/* ---- font descriptions ------------------------------------------------------------------ */

struct ffile;

/* one face of a font file: what DirectWrite says of it without its glyphs */
typedef struct fdesc {
    volatile LONG        refs;
    struct ffile        *file;              /* holds a reference */
    UINT32               index;
    UINT32               face_type;
    WCHAR                family[64], face[64], full[128], psname[64], win_family[64], win_face[64];
    UINT16               weight;
    uint8_t              stretch, style, symbol, mono, has_typo_family;
    uint8_t              panose[10];
    UINT32               glyphs;
    DWRITE_FONT_METRICS1 m;
    DWRITE_CARET_METRICS caret;
    /* the glyphs, when asked for */
    fdata_t             *data;
    FT_Face              ft;
    int                  ft_failed;
} fdesc_t;

/* a name from a font's name table, UTF-16: Windows' English one first */
static int name_get(const uint8_t *t, uint32_t len, int id, WCHAR *out, size_t cap) {
    int best = -1, score = 0;
    uint16_t count, so;
    out[0] = 0;
    if (!t || len < 6) return 0;
    count = be16(t + 2);
    so = be16(t + 4);
    for (int i = 0; i < count && 6 + 12 * (uint32_t)(i + 1) <= len; i++) {
        const uint8_t *r = t + 6 + 12 * i;
        int plat = be16(r), enc = be16(r + 2), lang = be16(r + 4), s = 0;
        if (be16(r + 6) != id) continue;
        if (plat == 3 && (enc == 1 || enc == 10)) s = lang == 0x409 ? 4 : 3;
        else if (plat == 0) s = 2;
        else if (plat == 1 && enc == 0) s = 1;
        if (s > score) score = s, best = i;
    }
    if (best < 0) return 0;
    {
        const uint8_t *r = t + 6 + 12 * best;
        uint32_t l = be16(r + 8), o = so + be16(r + 10);
        size_t n = 0;
        if (o > len || l > len - o) return 0;
        if (be16(r) == 1) {                              /* Mac Roman: as Latin-1 */
            for (uint32_t i = 0; i < l && n + 1 < cap; i++) out[n++] = t[o + i];
        } else {
            for (uint32_t i = 0; i + 1 < l && n + 1 < cap; i += 2) out[n++] = be16(t + o + i);
        }
        out[n] = 0;
        return n > 0;
    }
}

static int wcontains_ascii(const WCHAR *w, const char *a) {
    size_t la = strlen(a);
    for (; *w; w++) {
        size_t i = 0;
        for (; i < la && w[i]; i++) {
            WCHAR c = w[i] >= 'A' && w[i] <= 'Z' ? (WCHAR)(w[i] + 32) : w[i];
            if (c != (WCHAR)(unsigned char)a[i]) break;
        }
        if (i == la) return 1;
    }
    return 0;
}

/* fills d from face `index` of a font; 0 when it is a font DirectWrite would list */
static int describe(const src_t *s, UINT32 index, fdesc_t *d) {
    UINT32 faces;
    int coll;
    int64_t start = face_offset(s, index, &faces, &coll);
    uint8_t *head, *hhea, *os2, *post, *maxp, *name, *cmap;
    uint32_t lhead = 0, lhhea = 0, los2 = 0, lpost = 0, lmaxp = 0, lname = 0, lcmap = 0, off, len;
    DWRITE_FONT_METRICS1 *m = &d->m;
    int ok = 0;
    if (start < 0) return -1;
    head = load_table(s, start, "head", &lhead);
    hhea = load_table(s, start, "hhea", &lhhea);
    os2 = load_table(s, start, "OS/2", &los2);
    post = load_table(s, start, "post", &lpost);
    maxp = load_table(s, start, "maxp", &lmaxp);
    name = load_table(s, start, "name", &lname);
    cmap = load_table(s, start, "cmap", &lcmap);
    if (!head || lhead < 54 || !maxp || lmaxp < 6 || !cmap || lcmap < 4 || !hhea || lhhea < 36) goto out;
    if (!find_table(s, start, "glyf", &off, &len) && !find_table(s, start, "CFF ", &off, &len) &&
        !find_table(s, start, "CFF2", &off, &len))
        goto out;                                        /* bitmaps only */
    d->index = index;
    d->face_type = coll ? FACE_COLLECTION
                        : (find_table(s, start, "CFF ", &off, &len) || find_table(s, start, "CFF2", &off, &len))
                              ? FACE_CFF : FACE_TRUETYPE;
    d->glyphs = be16(maxp + 4);
    memset(m, 0, sizeof(*m));
    m->m.designUnitsPerEm = be16(head + 18);
    if (m->m.designUnitsPerEm < 16) goto out;
    m->glyphBoxLeft = (INT16)be16(head + 36);
    m->glyphBoxBottom = (INT16)be16(head + 38);
    m->glyphBoxRight = (INT16)be16(head + 40);
    m->glyphBoxTop = (INT16)be16(head + 42);
    m->m.ascent = be16(hhea + 4);
    m->m.descent = (UINT16)abs((INT16)be16(hhea + 6));
    d->caret.slopeRise = (INT16)be16(hhea + 18);
    d->caret.slopeRun = (INT16)be16(hhea + 20);
    d->caret.offset = (INT16)be16(hhea + 22);
    d->weight = 400;
    d->stretch = 5;
    d->style = (be16(head + 44) & 2) ? STYLE_ITALIC : STYLE_NORMAL;
    m->m.strikethroughPosition = (INT16)(m->m.designUnitsPerEm / 3);
    if (os2 && los2 >= 78) {
        uint16_t version = be16(os2), fs = be16(os2 + 62);
        int linegap = be16(hhea + 4) + abs((INT16)be16(hhea + 6)) + (INT16)be16(hhea + 8);
        uint16_t w = be16(os2 + 4), wd = be16(os2 + 6);
        m->m.ascent = be16(os2 + 74);
        m->m.descent = (UINT16)abs((INT16)be16(os2 + 76));
        linegap -= m->m.ascent + m->m.descent;
        m->m.lineGap = (INT16)(linegap > 0 ? linegap : 0);
        m->m.strikethroughThickness = be16(os2 + 26);
        m->m.strikethroughPosition = (INT16)be16(os2 + 28);
        m->subscriptSizeX = (INT16)be16(os2 + 10);
        m->subscriptSizeY = (INT16)be16(os2 + 12);
        m->subscriptPositionX = (INT16)be16(os2 + 14);
        m->subscriptPositionY = (INT16)-(INT16)be16(os2 + 16);
        m->superscriptSizeX = (INT16)be16(os2 + 18);
        m->superscriptSizeY = (INT16)be16(os2 + 20);
        m->superscriptPositionX = (INT16)be16(os2 + 22);
        m->superscriptPositionY = (INT16)be16(os2 + 24);
        if (version >= 2 && los2 >= 90) {
            m->m.xHeight = be16(os2 + 86);
            m->m.capHeight = be16(os2 + 88);
        }
        if (fs & 0x80) {                                 /* USE_TYPO_METRICS */
            INT16 td = (INT16)be16(os2 + 70);
            m->m.ascent = be16(os2 + 68);
            m->m.descent = (UINT16)(td < 0 ? -td : 0);
            m->m.lineGap = (INT16)be16(os2 + 72);
            m->hasTypographicMetrics = TRUE;
        }
        if (w >= 1 && w <= 9) w = (uint16_t)(w * 100);
        d->weight = (UINT16)(w < 1 ? 400 : w > 999 ? 999 : w);
        d->stretch = (uint8_t)(wd >= 1 && wd <= 9 ? wd : 5);
        d->style = (fs & 0x200) ? STYLE_OBLIQUE : (fs & 1) ? STYLE_ITALIC : STYLE_NORMAL;
        memcpy(d->panose, os2 + 32, 10);
    }
    if (post && lpost >= 16) {
        m->m.underlinePosition = (INT16)be16(post + 8);
        m->m.underlineThickness = be16(post + 10);
        d->mono = be32(post + 12) != 0;
    }
    if (!m->m.underlineThickness) m->m.underlineThickness = (UINT16)(m->m.designUnitsPerEm / 14);
    if (!m->m.strikethroughThickness) m->m.strikethroughThickness = m->m.underlineThickness;
    if (!m->m.xHeight) m->m.xHeight = (UINT16)(m->m.designUnitsPerEm / 2);
    if (!m->m.capHeight) m->m.capHeight = (UINT16)(m->m.designUnitsPerEm * 7 / 10);
    /* names: the WWS family (or the typographic, or the plain) groups faces */
    name_get(name, lname, 1, d->win_family, 64);
    name_get(name, lname, 2, d->win_face, 64);
    if (!name_get(name, lname, 21, d->family, 64) && !name_get(name, lname, 16, d->family, 64))
        wcopy(d->family, 64, d->win_family);
    else
        d->has_typo_family = 1;
    if (!name_get(name, lname, 22, d->face, 64) && !name_get(name, lname, 17, d->face, 64))
        wcopy(d->face, 64, d->win_face);
    name_get(name, lname, 4, d->full, 128);
    name_get(name, lname, 6, d->psname, 64);
    if (!d->family[0]) goto out;
    if (!d->face[0]) wfrom_ascii(d->face, 64, "Regular");
    if (!d->full[0]) wcopy(d->full, 128, d->family);
    if (d->style != STYLE_NORMAL && wcontains_ascii(d->face, "oblique")) d->style = STYLE_OBLIQUE;
    /* a symbol font: only a symbol character map */
    {
        int sym = 0, uni = 0;
        for (int i = 0; i < be16(cmap + 2) && 4 + 8 * (uint32_t)(i + 1) <= lcmap; i++) {
            int p = be16(cmap + 4 + 8 * i), e = be16(cmap + 6 + 8 * i);
            if (p == 3 && e == 0) sym = 1;
            if (p == 0 || (p == 3 && (e == 1 || e == 10))) uni = 1;
        }
        d->symbol = sym && !uni;
    }
    ok = 1;
out:
    free(head), free(hhea), free(os2), free(post), free(maxp), free(name), free(cmap);
    return ok ? 0 : -1;
}

/* ---- localized strings ------------------------------------------------------------------ */

typedef struct {
    obj_t  o;
    WCHAR *text;
} strs_t;

static void *const strs_vtbl[9];

static HRESULT WINAPI strs_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_LocalizedStrings)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI strs_Release(void *self) {
    strs_t *s = self;
    LONG r = obj_unref(self);
    if (!r) {
        free(s->text);
        free(s);
    }
    return (ULONG)r;
}
static UINT32 WINAPI strs_GetCount(void *self) { (void)self; return 1; }
static HRESULT WINAPI strs_FindLocaleName(void *self, const WCHAR *locale, UINT32 *index, BOOL *exists) {
    (void)self;
    *exists = locale && wieq_ascii(locale, "en-us");
    *index = *exists ? 0 : 0xFFFFFFFFu;
    return S_OK_;
}
static HRESULT WINAPI strs_GetLocaleNameLength(void *self, UINT32 i, UINT32 *len) {
    (void)self;
    if (i) return E_INVALIDARG_;
    *len = 5;
    return S_OK_;
}
static HRESULT WINAPI strs_GetLocaleName(void *self, UINT32 i, WCHAR *out, UINT32 size) {
    (void)self;
    if (i) return E_INVALIDARG_;
    if (size < 6) {
        if (size) out[0] = 0;
        return E_NOT_SUFFICIENT_BUFFER_;
    }
    wfrom_ascii(out, size, "en-us");
    return S_OK_;
}
static HRESULT WINAPI strs_GetStringLength(void *self, UINT32 i, UINT32 *len) {
    if (i) return E_INVALIDARG_;
    *len = (UINT32)w_len(((strs_t *)self)->text);
    return S_OK_;
}
static HRESULT WINAPI strs_GetString(void *self, UINT32 i, WCHAR *out, UINT32 size) {
    const WCHAR *t = ((strs_t *)self)->text;
    UINT32 n = (UINT32)w_len(t);
    if (i) return E_INVALIDARG_;
    if (size < n + 1) {
        if (size) out[0] = 0;
        return E_NOT_SUFFICIENT_BUFFER_;
    }
    memcpy(out, t, (n + 1) * sizeof(WCHAR));
    return S_OK_;
}
static void *const strs_vtbl[9] = {
    strs_QueryInterface, obj_AddRef, strs_Release, strs_GetCount, strs_FindLocaleName,
    strs_GetLocaleNameLength, strs_GetLocaleName, strs_GetStringLength, strs_GetString,
};

static HRESULT new_strings(const WCHAR *text, void **out) {
    strs_t *s = calloc(1, sizeof(*s));
    if (!s || !(s->text = w_dup(text))) {
        free(s);
        *out = 0;
        return E_OUTOFMEMORY_;
    }
    s->o.vt = strs_vtbl;
    s->o.refs = 1;
    *out = s;
    return S_OK_;
}

/* ---- font files and their loaders ------------------------------------------------------------ */

/* IDWriteFontFile: a loader and its key; the bytes once read */
typedef struct ffile {
    obj_t    o;
    void    *loader;            /* IDWriteFontFileLoader, referenced */
    uint8_t *key;
    UINT32   key_size;
    char     upath[512];        /* a local file: its Unix path */
    fdata_t *data;              /* read when first needed */
} ffile_t;

/* the local loader's key: the file's time and its Windows path */
typedef struct {
    FILETIME time;
    WCHAR    path[1];
} local_key_t;

static void *const ffile_vtbl[6];
static void *const local_loader_vtbl[7];
static void *const stream_vtbl[7];
static obj_t local_loader = { local_loader_vtbl, 1 };

/* IDWriteFontFileStream over bytes we hold */
typedef struct {
    obj_t    o;
    fdata_t *data;
} stream_t;

static HRESULT WINAPI stream_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontFileStream)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI stream_Release(void *self) {
    stream_t *s = self;
    LONG r = obj_unref(self);
    if (!r) {
        fdata_unref(s->data);
        free(s);
    }
    return (ULONG)r;
}
static HRESULT WINAPI stream_ReadFileFragment(void *self, const void **start, UINT64 off, UINT64 size, void **ctx) {
    fdata_t *d = ((stream_t *)self)->data;
    *ctx = 0;
    if (off > d->size || size > d->size - off) {
        *start = 0;
        return E_FAIL_;
    }
    *start = d->bytes + off;
    return S_OK_;
}
static void WINAPI stream_ReleaseFileFragment(void *self, void *ctx) { (void)self, (void)ctx; }
static HRESULT WINAPI stream_GetFileSize(void *self, UINT64 *size) {
    *size = ((stream_t *)self)->data->size;
    return S_OK_;
}
static HRESULT WINAPI stream_GetLastWriteTime(void *self, UINT64 *t) {
    *t = ((stream_t *)self)->data->mtime;
    return S_OK_;
}
static void *const stream_vtbl[7] = {
    stream_QueryInterface, obj_AddRef, stream_Release, stream_ReadFileFragment, stream_ReleaseFileFragment,
    stream_GetFileSize, stream_GetLastWriteTime,
};

/* the Unix path of a local key */
static int local_key_path(const void *key, UINT32 size, char *upath, size_t cap) {
    const local_key_t *k = key;
    char wp[1024];
    if (!key || size < sizeof(FILETIME) + 2) return -1;
    if (w_to_utf8(k->path, (int)((size - sizeof(FILETIME)) / 2), wp, sizeof(wp)) <= 0) return -1;
    wp[sizeof(wp) - 1] = 0;
    return path_to_unix(wp, upath, cap);
}

static HRESULT WINAPI loader_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontFileLoader) || iid_is(iid, &IID_LocalFontFileLoader)) {
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI static_AddRef(void *self) { (void)self; return 2; }
static ULONG WINAPI static_Release(void *self) { (void)self; return 1; }
static HRESULT WINAPI loader_CreateStreamFromKey(void *self, const void *key, UINT32 size, void **out) {
    char up[1024];
    stream_t *s;
    fdata_t *d;
    (void)self;
    *out = 0;
    if (local_key_path(key, size, up, sizeof(up)) != 0) return E_INVALIDARG_;
    if (!(d = fdata_local(up))) return DWRITE_E_FILENOTFOUND;
    if (!(s = calloc(1, sizeof(*s)))) {
        fdata_unref(d);
        return E_OUTOFMEMORY_;
    }
    s->o.vt = stream_vtbl;
    s->o.refs = 1;
    s->data = d;
    *out = s;
    return S_OK_;
}
static HRESULT WINAPI loader_GetFilePathLengthFromKey(void *self, const void *key, UINT32 size, UINT32 *len) {
    (void)self;
    if (!key || size < sizeof(FILETIME) + 2) return E_INVALIDARG_;
    *len = (UINT32)w_len(((const local_key_t *)key)->path);
    return S_OK_;
}
static HRESULT WINAPI loader_GetFilePathFromKey(void *self, const void *key, UINT32 size, WCHAR *out, UINT32 len) {
    const WCHAR *p;
    UINT32 n;
    (void)self;
    if (!key || size < sizeof(FILETIME) + 2) return E_INVALIDARG_;
    p = ((const local_key_t *)key)->path;
    n = (UINT32)w_len(p);
    if (len < n + 1) return E_NOT_SUFFICIENT_BUFFER_;
    memcpy(out, p, (n + 1) * sizeof(WCHAR));
    return S_OK_;
}
static HRESULT WINAPI loader_GetLastWriteTimeFromKey(void *self, const void *key, UINT32 size, FILETIME *t) {
    (void)self;
    if (!key || size < sizeof(FILETIME)) return E_INVALIDARG_;
    *t = ((const local_key_t *)key)->time;
    return S_OK_;
}
static void *const local_loader_vtbl[7] = {
    loader_QueryInterface, static_AddRef, static_Release, loader_CreateStreamFromKey,
    loader_GetFilePathLengthFromKey, loader_GetFilePathFromKey, loader_GetLastWriteTimeFromKey,
};

static HRESULT WINAPI ffile_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontFile)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI ffile_Release(void *self) {
    ffile_t *f = self;
    LONG r = obj_unref(self);
    if (!r) {
        com_release(f->loader);
        fdata_unref(f->data);
        free(f->key);
        free(f);
    }
    return (ULONG)r;
}
static HRESULT WINAPI ffile_GetReferenceKey(void *self, const void **key, UINT32 *size) {
    ffile_t *f = self;
    *key = f->key;
    *size = f->key_size;
    return S_OK_;
}
static HRESULT WINAPI ffile_GetLoader(void *self, void **loader) {
    ffile_t *f = self;
    com_addref(f->loader);
    *loader = f->loader;
    return S_OK_;
}

/* the file's bytes: a local file is read directly, another through its
 * loader's stream (a program's own code, called without the lock) */
static fdata_t *ffile_data(ffile_t *f) {
    fdata_t *d;
    typedef HRESULT (WINAPI *mkstream_fn)(void *, const void *, UINT32, void **);
    typedef HRESULT (WINAPI *read_fn)(void *, const void **, UINT64, UINT64, void **);
    typedef void (WINAPI *rel_frag_fn)(void *, void *);
    typedef HRESULT (WINAPI *size_fn)(void *, UINT64 *);
    lock();
    d = f->data;
    if (d) __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
    unlock();
    if (d) return d;
    if (f->upath[0]) {
        d = fdata_local(f->upath);
    } else {
        void *stream = 0, *ctx = 0;
        const void *frag = 0;
        UINT64 size = 0;
        if (((mkstream_fn)VTBL(f->loader)[3])(f->loader, f->key, f->key_size, &stream) < 0 || !stream) return 0;
        if (((size_fn)VTBL(stream)[5])(stream, &size) >= 0 && size >= 12 && size < (1ull << 30) &&
            ((read_fn)VTBL(stream)[3])(stream, &frag, 0, size, &ctx) >= 0 && frag) {
            d = calloc(1, sizeof(*d));
            if (d && (d->bytes = malloc((size_t)size))) {
                memcpy((void *)d->bytes, frag, (size_t)size);
                d->size = (size_t)size;
                d->refs = 1;
            } else {
                free(d);
                d = 0;
            }
            ((rel_frag_fn)VTBL(stream)[4])(stream, ctx);
        }
        com_release(stream);
    }
    if (!d) return 0;
    lock();
    if (!f->data) {
        f->data = d;
        __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
    } else {                                   /* another thread read it first */
        fdata_unref(d);
        d = f->data;
        __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
    }
    unlock();
    return d;
}

static HRESULT WINAPI ffile_Analyze(void *self, BOOL *supported, UINT32 *ftype, UINT32 *facetype, UINT32 *faces) {
    fdata_t *d = ffile_data(self);
    src_t s = { 0, 0, -1 };
    UINT32 n = 0;
    int coll = 0;
    fdesc_t probe;
    *supported = FALSE;
    *ftype = FILE_UNKNOWN;
    if (facetype) *facetype = 6;                   /* UNKNOWN */
    *faces = 0;
    if (!d) return DWRITE_E_FILENOTFOUND;
    s.mem = d->bytes, s.size = d->size;
    memset(&probe, 0, sizeof(probe));
    if (face_offset(&s, 0, &n, &coll) >= 0 && describe(&s, 0, &probe) == 0) {
        *supported = TRUE;
        *faces = n;
        *ftype = coll ? FILE_COLLECTION : probe.face_type == FACE_CFF ? FILE_CFF : FILE_TRUETYPE;
        if (facetype) *facetype = coll ? FACE_COLLECTION : probe.face_type;
    }
    fdata_unref(d);
    return S_OK_;
}
static void *const ffile_vtbl[6] = {
    ffile_QueryInterface, obj_AddRef, ffile_Release, ffile_GetReferenceKey, ffile_GetLoader, ffile_Analyze,
};

static ffile_t *as_ffile(void *p) { return p && ((obj_t *)p)->vt == ffile_vtbl ? p : 0; }

static ffile_t *new_ffile(void *loader, const void *key, UINT32 key_size, const char *upath) {
    ffile_t *f = calloc(1, sizeof(*f));
    if (!f || !(f->key = malloc(key_size ? key_size : 1))) {
        free(f);
        return 0;
    }
    f->o.vt = ffile_vtbl;
    f->o.refs = 1;
    memcpy(f->key, key, key_size);
    f->key_size = key_size;
    f->loader = loader;
    com_addref(loader);
    if (upath) snprintf(f->upath, sizeof(f->upath), "%s", upath);
    return f;
}

/* a local file's IDWriteFontFile */
static ffile_t *new_local_ffile(const char *upath, const char *winpath) {
    WCHAR wp[600];
    int n = utf8_to_w(winpath, -1, wp, 600);
    local_key_t *k;
    struct stat st;
    ffile_t *f;
    UINT32 size;
    if (n <= 0) return 0;
    size = (UINT32)(sizeof(FILETIME) + (size_t)n * sizeof(WCHAR));
    k = calloc(1, size);
    if (!k) return 0;
    if (stat(upath, &st) == 0) unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &k->time);
    memcpy(k->path, wp, (size_t)n * sizeof(WCHAR));
    f = new_ffile(&local_loader, k, size, upath);
    free(k);
    return f;
}

static int same_file(ffile_t *a, ffile_t *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->upath[0] || b->upath[0]) return !strcmp(a->upath, b->upath);
    return a->loader == b->loader && a->key_size == b->key_size && !memcmp(a->key, b->key, a->key_size);
}

/* ---- FreeType faces ------------------------------------------------------------------------ */

static void fdesc_unref(fdesc_t *d) {
    if (!d || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) return;
    lock();
    if (d->ft) FT_Done_Face(d->ft);
    unlock();
    fdata_unref(d->data);
    com_release(d->file);
    free(d);
}

/* the face's FreeType face (under the lock): 0 if it cannot be read */
static FT_Face ft_face(fdesc_t *d) {
    fdata_t *data;
    if (d->ft || d->ft_failed) return d->ft;
    unlock();
    data = ffile_data(d->file);
    lock();
    if (d->ft || d->ft_failed) {
        fdata_unref(data);
        return d->ft;
    }
    if (!data || !ft_lib || FT_New_Memory_Face(ft_lib, data->bytes, (FT_Long)data->size, (FT_Long)d->index, &d->ft)) {
        d->ft = 0;
        d->ft_failed = 1;
        fdata_unref(data);
        wlog("dwrite: a font could not be read");
        return 0;
    }
    d->data = data;
    if (d->symbol) {
        if (FT_Select_Charmap(d->ft, FT_ENCODING_MS_SYMBOL)) FT_Select_Charmap(d->ft, FT_ENCODING_UNICODE);
    } else {
        FT_Select_Charmap(d->ft, FT_ENCODING_UNICODE);
    }
    return d->ft;
}

static UINT32 glyph_of(fdesc_t *d, UINT32 cp) {
    FT_Face f = ft_face(d);
    UINT32 g;
    if (!f) return 0;
    g = FT_Get_Char_Index(f, cp);
    if (!g && d->symbol && cp < 0x100) g = FT_Get_Char_Index(f, 0xF000 + cp);
    return g;
}

static int has_char(fdesc_t *d, UINT32 cp) {
    int r;
    lock();
    r = glyph_of(d, cp) != 0;
    unlock();
    return r;
}

/* synthetic bold: FreeType's strength, a 24th of the em */
static FT_Pos bold_strength(fdesc_t *d) { return d->m.m.designUnitsPerEm / 24; }

/* a glyph's outline in font units (y up), simulations applied: a copy the
 * caller frees with FT_Outline_Done (under the lock) */
static int glyph_outline(fdesc_t *d, UINT32 sims, UINT16 gid, FT_Outline *out, FT_Pos *advance) {
    FT_Face f = ft_face(d);
    FT_Outline *o;
    if (!f || FT_Load_Glyph(f, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM))
        return -1;
    if (advance) *advance = f->glyph->metrics.horiAdvance + ((sims & SIM_BOLD) ? bold_strength(d) : 0);
    if (f->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    o = &f->glyph->outline;
    if (FT_Outline_New(ft_lib, (FT_UInt)o->n_points, o->n_contours, out)) return -1;
    FT_Outline_Copy(o, out);
    if (sims & SIM_BOLD) FT_Outline_EmboldenXY(out, bold_strength(d), bold_strength(d));
    if (sims & SIM_OBLIQUE) {
        FT_Matrix shear = { 0x10000, 0x5555, 0, 0x10000 };     /* x += y / 3 */
        FT_Outline_Transform(out, &shear);
    }
    return 0;
}

static int design_metrics(fdesc_t *d, UINT32 sims, UINT16 gid, DWRITE_GLYPH_METRICS *gm) {
    FT_Face f = ft_face(d);
    const DWRITE_FONT_METRICS *m = &d->m.m;
    FT_Glyph_Metrics *g;
    INT32 xmin, ymax, ymin, adv;
    memset(gm, 0, sizeof(*gm));
    gm->verticalOriginY = m->ascent;
    gm->advanceHeight = (UINT32)m->ascent + m->descent;
    if (!f || FT_Load_Glyph(f, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM))
        return -1;
    g = &f->glyph->metrics;
    adv = (INT32)g->horiAdvance + ((sims & SIM_BOLD) ? (INT32)bold_strength(d) : 0);
    xmin = (INT32)g->horiBearingX;
    ymax = (INT32)g->horiBearingY;
    ymin = (INT32)(g->horiBearingY - g->height);
    gm->advanceWidth = (UINT32)(adv > 0 ? adv : 0);
    gm->leftSideBearing = xmin;
    gm->rightSideBearing = adv - (xmin + (INT32)g->width + ((sims & SIM_BOLD) ? (INT32)bold_strength(d) : 0));
    gm->topSideBearing = gm->verticalOriginY - ymax;
    gm->bottomSideBearing = ymin - gm->verticalOriginY + (INT32)gm->advanceHeight;
    if (!g->width && !g->height) gm->rightSideBearing = adv - xmin, gm->topSideBearing = gm->bottomSideBearing = 0;
    return 0;
}

/* ---- font faces ------------------------------------------------------------------------------ */

typedef struct face {
    obj_t        o;
    fdesc_t     *d;
    UINT32       sims;
    struct face *next;           /* the faces alive, so one font is one face */
} face_t;

static face_t *live_faces;
static void *const face_vtbl[35];

static face_t *as_face(void *p) { return p && ((obj_t *)p)->vt == face_vtbl ? p : 0; }

static HRESULT WINAPI face_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontFace) || iid_is(iid, &IID_FontFace1) ||
        iid_is(iid, &IID_FontFace2)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI face_Release(void *self) {
    face_t *f = self;
    LONG r;
    lock();                                   /* not found in the live list while it goes */
    r = obj_unref(self);
    if (!r) {
        for (face_t **p = &live_faces; *p; p = &(*p)->next)
            if (*p == f) {
                *p = f->next;
                break;
            }
    }
    unlock();
    if (!r) {
        fdesc_unref(f->d);
        free(f);
    }
    return (ULONG)r;
}

/* the face of a font description: one object while it lives */
static face_t *get_face(fdesc_t *d, UINT32 sims) {
    face_t *f;
    lock();
    for (f = live_faces; f; f = f->next)
        if (f->sims == sims && (f->d == d || (same_file(f->d->file, d->file) && f->d->index == d->index))) {
            obj_AddRef(f);
            unlock();
            return f;
        }
    f = calloc(1, sizeof(*f));
    if (f) {
        f->o.vt = face_vtbl;
        f->o.refs = 1;
        f->d = d;
        __atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
        f->sims = sims;
        f->next = live_faces;
        live_faces = f;
    }
    unlock();
    return f;
}

static UINT32 WINAPI face_GetType(void *self) { return ((face_t *)self)->d->face_type; }
static HRESULT WINAPI face_GetFiles(void *self, UINT32 *n, void **files) {
    face_t *f = self;
    if (!n) return E_INVALIDARG_;
    if (files) {
        if (*n < 1) return E_INVALIDARG_;
        obj_AddRef(f->d->file);
        files[0] = f->d->file;
    }
    *n = 1;
    return S_OK_;
}
static UINT32 WINAPI face_GetIndex(void *self) { return ((face_t *)self)->d->index; }
static UINT32 WINAPI face_GetSimulations(void *self) { return ((face_t *)self)->sims; }
static BOOL WINAPI face_IsSymbolFont(void *self) { return ((face_t *)self)->d->symbol; }
static void WINAPI face_GetMetrics(void *self, DWRITE_FONT_METRICS *m) { *m = ((face_t *)self)->d->m.m; }
static UINT16 WINAPI face_GetGlyphCount(void *self) { return (UINT16)((face_t *)self)->d->glyphs; }
static HRESULT WINAPI face_GetDesignGlyphMetrics(void *self, const UINT16 *g, UINT32 n, DWRITE_GLYPH_METRICS *gm,
                                                 BOOL sideways) {
    face_t *f = self;
    (void)sideways;
    if (!g || !gm) return E_INVALIDARG_;
    lock();
    for (UINT32 i = 0; i < n; i++) design_metrics(f->d, f->sims, g[i], &gm[i]);
    unlock();
    return S_OK_;
}
static HRESULT WINAPI face_GetGlyphIndices(void *self, const UINT32 *cps, UINT32 n, UINT16 *out) {
    face_t *f = self;
    if (!cps || !out) return E_INVALIDARG_;
    lock();
    for (UINT32 i = 0; i < n; i++) out[i] = (UINT16)glyph_of(f->d, cps[i]);
    unlock();
    return S_OK_;
}
static HRESULT WINAPI face_TryGetFontTable(void *self, UINT32 tag, const void **data, UINT32 *size, void **ctx,
                                           BOOL *exists) {
    face_t *f = self;
    fdata_t *d;
    src_t s = { 0, 0, -1 };
    UINT32 faces, off, len;
    int coll;
    char t[4];
    *data = 0, *size = 0, *ctx = 0, *exists = FALSE;
    lock();
    ft_face(f->d);                             /* reads the file */
    d = f->d->data;
    unlock();
    if (!d) return S_OK_;
    s.mem = d->bytes, s.size = d->size;
    memcpy(t, &tag, 4);                        /* DWRITE_MAKE_OPENTYPE_TAG: the first letter lowest */
    if (find_table(&s, face_offset(&s, f->d->index, &faces, &coll), t, &off, &len) && off <= d->size &&
        len <= d->size - off) {
        *data = d->bytes + off;
        *size = len;
        *exists = TRUE;
    }
    return S_OK_;
}
static void WINAPI face_ReleaseFontTable(void *self, void *ctx) { (void)self, (void)ctx; }

/* ---- outlines to a geometry sink ---- */

typedef void (WINAPI *sink_fill_fn)(void *, UINT32);
typedef void (WINAPI *sink_begin_fn)(void *, D2D1_POINT_2F, UINT32);
typedef void (WINAPI *sink_lines_fn)(void *, const D2D1_POINT_2F *, UINT32);
typedef void (WINAPI *sink_beziers_fn)(void *, const D2D1_BEZIER_SEGMENT *, UINT32);
typedef void (WINAPI *sink_end_fn)(void *, UINT32);

typedef struct {
    void         *sink;
    double        scale, ox, oy;       /* font units to the run's coordinates (y down) */
    int           open;
    D2D1_POINT_2F last;
    D2D1_POINT_2F lines[64];
    int           nlines;
} decomp_t;

static D2D1_POINT_2F pt_of(decomp_t *c, const FT_Vector *v) {
    D2D1_POINT_2F p = { (FLOAT)(c->ox + v->x * c->scale), (FLOAT)(c->oy - v->y * c->scale) };
    return p;
}
static void flush_lines(decomp_t *c) {
    if (c->nlines) ((sink_lines_fn)VTBL(c->sink)[6])(c->sink, c->lines, (UINT32)c->nlines);
    c->nlines = 0;
}
static void end_figure(decomp_t *c) {
    if (!c->open) return;
    flush_lines(c);
    ((sink_end_fn)VTBL(c->sink)[8])(c->sink, 1);       /* D2D1_FIGURE_END_CLOSED */
    c->open = 0;
}
static int dc_move(const FT_Vector *to, void *u) {
    decomp_t *c = u;
    end_figure(c);
    c->last = pt_of(c, to);
    ((sink_begin_fn)VTBL(c->sink)[5])(c->sink, c->last, 0);     /* D2D1_FIGURE_BEGIN_FILLED */
    c->open = 1;
    return 0;
}
static int dc_line(const FT_Vector *to, void *u) {
    decomp_t *c = u;
    c->last = pt_of(c, to);
    if (c->nlines == 64) flush_lines(c);
    c->lines[c->nlines++] = c->last;
    return 0;
}
static int dc_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
    decomp_t *c = u;
    D2D1_BEZIER_SEGMENT b = { pt_of(c, c1), pt_of(c, c2), pt_of(c, to) };
    flush_lines(c);
    ((sink_beziers_fn)VTBL(c->sink)[7])(c->sink, &b, 1);
    c->last = b.point3;
    return 0;
}
static int dc_conic(const FT_Vector *ctl, const FT_Vector *to, void *u) {
    decomp_t *c = u;
    D2D1_POINT_2F q = pt_of(c, ctl), p1 = pt_of(c, to), p0 = c->last;
    D2D1_BEZIER_SEGMENT b;
    b.point1.x = p0.x + 2.0f / 3.0f * (q.x - p0.x);
    b.point1.y = p0.y + 2.0f / 3.0f * (q.y - p0.y);
    b.point2.x = p1.x + 2.0f / 3.0f * (q.x - p1.x);
    b.point2.y = p1.y + 2.0f / 3.0f * (q.y - p1.y);
    b.point3 = p1;
    flush_lines(c);
    ((sink_beziers_fn)VTBL(c->sink)[7])(c->sink, &b, 1);
    c->last = p1;
    return 0;
}

static HRESULT WINAPI face_GetGlyphRunOutline(void *self, FLOAT em, const UINT16 *g, const FLOAT *adv,
                                              const DWRITE_GLYPH_OFFSET *offs, UINT32 n, BOOL sideways, BOOL rtl,
                                              void *sink) {
    static const FT_Outline_Funcs funcs = { dc_move, dc_line, dc_conic, dc_cubic, 0, 0 };
    face_t *f = self;
    double scale = em / f->d->m.m.designUnitsPerEm, pen = 0;
    (void)sideways;
    if (!sink || (n && !g)) return E_INVALIDARG_;
    ((sink_fill_fn)VTBL(sink)[3])(sink, 1);                  /* D2D1_FILL_MODE_WINDING */
    for (UINT32 i = 0; i < n; i++) {
        FT_Outline o;
        FT_Pos a = 0;
        double advance, x, y;
        decomp_t c;
        lock();
        if (glyph_outline(f->d, f->sims, g[i], &o, &a) != 0) memset(&o, 0, sizeof(o)), a = 0;
        unlock();
        advance = adv ? adv[i] : a * scale;
        if (rtl) pen -= advance;
        x = pen + (offs ? (rtl ? -offs[i].advanceOffset : offs[i].advanceOffset) : 0);
        y = offs ? -offs[i].ascenderOffset : 0;
        if (!rtl) pen += advance;
        memset(&c, 0, sizeof(c));
        c.sink = sink, c.scale = scale, c.ox = x, c.oy = y;
        if (o.n_points) {
            FT_Outline_Decompose(&o, &funcs, &c);
            end_figure(&c);
        }
        lock();
        if (o.n_points || o.n_contours) FT_Outline_Done(ft_lib, &o);
        unlock();
    }
    return S_OK_;
}

static HRESULT WINAPI face_GetRecommendedRenderingMode(void *self, FLOAT em, FLOAT ppdip, UINT32 measuring,
                                                       void *params, UINT32 *mode) {
    (void)self, (void)measuring, (void)params;
    *mode = em * ppdip >= 100.0f ? RM_OUTLINE : RM_NATURAL_SYMMETRIC;
    return S_OK_;
}
static HRESULT WINAPI face_GetGdiCompatibleMetrics(void *self, FLOAT em, FLOAT ppdip, const DWRITE_MATRIX *m,
                                                   DWRITE_FONT_METRICS *out) {
    (void)em, (void)ppdip, (void)m;
    *out = ((face_t *)self)->d->m.m;
    return S_OK_;
}
static HRESULT WINAPI face_GetGdiCompatibleGlyphMetrics(void *self, FLOAT em, FLOAT ppdip, const DWRITE_MATRIX *m,
                                                        BOOL natural, const UINT16 *g, UINT32 n,
                                                        DWRITE_GLYPH_METRICS *gm, BOOL sideways) {
    (void)em, (void)ppdip, (void)m, (void)natural;
    return face_GetDesignGlyphMetrics(self, g, n, gm, sideways);
}
/* IDWriteFontFace1 */
static void WINAPI face_GetMetrics1(void *self, DWRITE_FONT_METRICS1 *m) { *m = ((face_t *)self)->d->m; }
static HRESULT WINAPI face_GetGdiCompatibleMetrics1(void *self, FLOAT em, FLOAT ppdip, const DWRITE_MATRIX *m,
                                                    DWRITE_FONT_METRICS1 *out) {
    (void)em, (void)ppdip, (void)m;
    *out = ((face_t *)self)->d->m;
    return S_OK_;
}
static void WINAPI face_GetCaretMetrics(void *self, DWRITE_CARET_METRICS *c) {
    face_t *f = self;
    *c = f->d->caret;
    if (!c->slopeRise && !c->slopeRun) c->slopeRise = 1;
    if (f->sims & SIM_OBLIQUE) c->slopeRise = 3, c->slopeRun = 1;
}
static HRESULT unicode_ranges(fdesc_t *d, UINT32 max, DWRITE_UNICODE_RANGE *out, UINT32 *count) {
    FT_Face f;
    FT_ULong cp;
    FT_UInt gi;
    UINT32 n = 0;
    DWRITE_UNICODE_RANGE cur = { 0, 0 };
    int have = 0;
    lock();
    f = ft_face(d);
    if (f) {
        for (cp = FT_Get_First_Char(f, &gi); gi; cp = FT_Get_Next_Char(f, cp, &gi)) {
            if (have && cp == cur.last + 1) {
                cur.last = (UINT32)cp;
                continue;
            }
            if (have) {
                if (n < max && out) out[n] = cur;
                n++;
            }
            cur.first = cur.last = (UINT32)cp;
            have = 1;
        }
        if (have) {
            if (n < max && out) out[n] = cur;
            n++;
        }
    }
    unlock();
    *count = n;
    return n > max ? E_NOT_SUFFICIENT_BUFFER_ : S_OK_;
}
static HRESULT WINAPI face_GetUnicodeRanges(void *self, UINT32 max, DWRITE_UNICODE_RANGE *out, UINT32 *count) {
    return unicode_ranges(((face_t *)self)->d, max, out, count);
}
static BOOL WINAPI face_IsMonospacedFont(void *self) { return ((face_t *)self)->d->mono; }
static HRESULT WINAPI face_GetDesignGlyphAdvances(void *self, UINT32 n, const UINT16 *g, INT32 *adv, BOOL sideways) {
    face_t *f = self;
    FT_Face ft;
    if (!g || !adv) return E_INVALIDARG_;
    lock();
    ft = ft_face(f->d);
    for (UINT32 i = 0; i < n; i++) {
        FT_Fixed a = 0;
        if (sideways) a = (FT_Fixed)f->d->m.m.ascent + f->d->m.m.descent;
        else if (ft) FT_Get_Advance(ft, g[i], FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, &a);
        adv[i] = (INT32)a + (!sideways && (f->sims & SIM_BOLD) ? (INT32)bold_strength(f->d) : 0);
    }
    unlock();
    return S_OK_;
}
static HRESULT WINAPI face_GetGdiCompatibleGlyphAdvances(void *self, FLOAT em, FLOAT ppdip, const DWRITE_MATRIX *m,
                                                         BOOL natural, BOOL sideways, UINT32 n, const UINT16 *g,
                                                         INT32 *adv) {
    (void)em, (void)ppdip, (void)m, (void)natural;
    return face_GetDesignGlyphAdvances(self, n, g, adv, sideways);
}
static HRESULT WINAPI face_GetKerningPairAdjustments(void *self, UINT32 n, const UINT16 *g, INT32 *adj) {
    face_t *f = self;
    FT_Face ft;
    if (!n) return S_OK_;
    if (!g || !adj) return E_INVALIDARG_;
    memset(adj, 0, n * sizeof(*adj));
    lock();
    ft = ft_face(f->d);
    if (ft && FT_HAS_KERNING(ft))
        for (UINT32 i = 0; i + 1 < n; i++) {
            FT_Vector k;
            if (!FT_Get_Kerning(ft, g[i], g[i + 1], FT_KERNING_UNSCALED, &k)) adj[i] = (INT32)k.x;
        }
    unlock();
    return S_OK_;
}
static BOOL WINAPI face_HasKerningPairs(void *self) {
    face_t *f = self;
    FT_Face ft;
    BOOL r;
    lock();
    ft = ft_face(f->d);
    r = ft && FT_HAS_KERNING(ft);
    unlock();
    return r;
}
static HRESULT WINAPI face_GetRecommendedRenderingMode1(void *self, FLOAT em, FLOAT dx, FLOAT dy,
                                                        const DWRITE_MATRIX *m, BOOL sideways, UINT32 threshold,
                                                        UINT32 measuring, UINT32 *mode) {
    (void)self, (void)dx, (void)m, (void)sideways, (void)threshold, (void)measuring;
    *mode = em * dy / 96.0f >= 100.0f ? RM_OUTLINE : RM_NATURAL_SYMMETRIC;
    return S_OK_;
}
static HRESULT WINAPI face_GetVerticalGlyphVariants(void *self, UINT32 n, const UINT16 *in, UINT16 *out) {
    (void)self;
    if (n && (!in || !out)) return E_INVALIDARG_;
    memcpy(out, in, n * sizeof(UINT16));
    return S_OK_;
}
static BOOL WINAPI face_HasVerticalGlyphVariants(void *self) { (void)self; return FALSE; }
/* IDWriteFontFace2 */
static BOOL WINAPI face_IsColorFont(void *self) { (void)self; return FALSE; }
static UINT32 WINAPI face_GetColorPaletteCount(void *self) { (void)self; return 0; }
static UINT32 WINAPI face_GetPaletteEntryCount(void *self) { (void)self; return 0; }
static HRESULT WINAPI face_GetPaletteEntries(void *self, UINT32 p, UINT32 first, UINT32 n, void *out) {
    (void)self, (void)p, (void)first, (void)n, (void)out;
    return DWRITE_E_NOCOLOR;
}
static HRESULT WINAPI face_GetRecommendedRenderingMode2(void *self, FLOAT em, FLOAT dx, FLOAT dy,
                                                        const DWRITE_MATRIX *m, BOOL sideways, UINT32 threshold,
                                                        UINT32 measuring, void *params, UINT32 *mode,
                                                        UINT32 *gridfit) {
    (void)params;
    face_GetRecommendedRenderingMode1(self, em, dx, dy, m, sideways, threshold, measuring, mode);
    *gridfit = 1;                                         /* DWRITE_GRID_FIT_MODE_DISABLED */
    return S_OK_;
}
static void *const face_vtbl[35] = {
    face_QueryInterface, obj_AddRef, face_Release,
    face_GetType, face_GetFiles, face_GetIndex, face_GetSimulations, face_IsSymbolFont, face_GetMetrics,
    face_GetGlyphCount, face_GetDesignGlyphMetrics, face_GetGlyphIndices, face_TryGetFontTable,
    face_ReleaseFontTable, face_GetGlyphRunOutline, face_GetRecommendedRenderingMode,
    face_GetGdiCompatibleMetrics, face_GetGdiCompatibleGlyphMetrics,
    /* IDWriteFontFace1 */
    face_GetMetrics1, face_GetGdiCompatibleMetrics1, face_GetCaretMetrics, face_GetUnicodeRanges,
    face_IsMonospacedFont, face_GetDesignGlyphAdvances, face_GetGdiCompatibleGlyphAdvances,
    face_GetKerningPairAdjustments, face_HasKerningPairs, face_GetRecommendedRenderingMode1,
    face_GetVerticalGlyphVariants, face_HasVerticalGlyphVariants,
    /* IDWriteFontFace2 */
    face_IsColorFont, face_GetColorPaletteCount, face_GetPaletteEntryCount, face_GetPaletteEntries,
    face_GetRecommendedRenderingMode2,
};
_Static_assert(sizeof(face_vtbl) / sizeof(face_vtbl[0]) == 35, "IDWriteFontFace2 has 35 methods");

/* ---- collections, families, fonts ---------------------------------------------------------------- */

typedef struct {
    WCHAR     name[64];
    fdesc_t **fonts;
    int       n, cap;
} famdata_t;

typedef struct {
    obj_t      o;
    famdata_t *fams;
    int        nfam, cap;
    int        system;
} coll_t;

typedef struct {
    obj_t   o;
    coll_t *c;
    int     fam;
} family_t;

typedef struct {
    obj_t    o;
    coll_t  *c;
    int      fam;
    fdesc_t *d;
    UINT32   sims;
} font_t;

typedef struct {
    obj_t    o;
    coll_t  *c;
    int      fam;
    int      n;
    int     *idx;
    UINT32   sims;              /* of the first (the best match) */
} fontlist_t;

static void *const coll_vtbl[7];
static void *const family_vtbl[9];
static void *const font_vtbl[19];
static void *const fontlist_vtbl[6];

static coll_t *as_coll(void *p) { return p && ((obj_t *)p)->vt == coll_vtbl ? p : 0; }
static font_t *as_font(void *p) { return p && ((obj_t *)p)->vt == font_vtbl ? p : 0; }

static coll_t *new_coll(void) {
    coll_t *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->o.vt = coll_vtbl;
    c->o.refs = 1;
    return c;
}

/* adds a face, taking the reference d holds; same family and full name as
 * one there (the same font in two folders): the first stays */
static void coll_add(coll_t *c, fdesc_t *d) {
    famdata_t *f = 0;
    for (int i = 0; i < c->nfam; i++)
        if (wieq(c->fams[i].name, d->family)) {
            f = &c->fams[i];
            break;
        }
    if (!f) {
        if (c->nfam == c->cap) {
            int cap = c->cap ? c->cap * 2 : 16;
            famdata_t *n = realloc(c->fams, (size_t)cap * sizeof(*n));
            if (!n) goto drop;
            c->fams = n, c->cap = cap;
        }
        f = &c->fams[c->nfam++];
        memset(f, 0, sizeof(*f));
        wcopy(f->name, 64, d->family);
    }
    for (int i = 0; i < f->n; i++)
        if (wieq(f->fonts[i]->full, d->full) && f->fonts[i]->weight == d->weight && f->fonts[i]->style == d->style &&
            f->fonts[i]->stretch == d->stretch)
            goto drop;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 4;
        fdesc_t **n = realloc(f->fonts, (size_t)cap * sizeof(*n));
        if (!n) goto drop;
        f->fonts = n, f->cap = cap;
    }
    f->fonts[f->n++] = d;
    return;
drop:
    fdesc_unref(d);
}

/* every face of a font file, added to c */
static int coll_add_file(coll_t *c, ffile_t *file, const src_t *s) {
    UINT32 faces = 0;
    int coll, added = 0;
    if (face_offset(s, 0, &faces, &coll) < 0) return 0;
    for (UINT32 i = 0; i < faces; i++) {
        fdesc_t *d = calloc(1, sizeof(*d));
        if (!d) break;
        if (describe(s, i, d) != 0) {
            free(d);
            continue;
        }
        d->refs = 1;
        d->file = file;
        obj_AddRef(file);
        coll_add(c, d);
        added++;
    }
    return added;
}

/* the best of a family's fonts for a weight, stretch and style (CSS's order);
 * *sims: what DirectWrite would simulate */
static int weight_rank(int want, int w) {
    if (w == want) return 0;
    if (want == 400 || want == 500) {
        int other = want == 400 ? 500 : 400;
        if (w == other) return 1;
        if (w < 400) return 2 + (400 - w);
        return 1000 + w;
    }
    if (want < 400) return w <= want ? want - w : 1000 + (w - want);
    return w >= want ? w - want : 1000 + (want - w);
}
static int style_rank(int want, int s) {
    static const int order[3][3] = { { 0, 1, 2 }, { 2, 0, 1 }, { 2, 1, 0 } };   /* [want][have] */
    return order[want > 2 ? 0 : want][s > 2 ? 0 : s];
}
static long match_score(const fdesc_t *d, int weight, int stretch, int style) {
    int sd = d->stretch - stretch;
    long st = sd == 0 ? 0 : (stretch <= 5 ? (sd < 0 ? -sd : 10 + sd) : (sd > 0 ? sd : 10 - sd));
    return st * 10000000L + style_rank(style, d->style) * 100000L + weight_rank(weight, d->weight);
}
static int best_match(const famdata_t *f, int weight, int stretch, int style, UINT32 *sims) {
    int best = -1;
    long bs = 0;
    for (int i = 0; i < f->n; i++) {
        long s = match_score(f->fonts[i], weight, stretch, style);
        if (best < 0 || s < bs) best = i, bs = s;
    }
    if (sims) {
        *sims = 0;
        if (best >= 0) {
            const fdesc_t *d = f->fonts[best];
            if (weight >= 600 && d->weight < 600 && weight - d->weight >= 150) *sims |= SIM_BOLD;
            if (style != STYLE_NORMAL && d->style == STYLE_NORMAL) *sims |= SIM_OBLIQUE;
        }
    }
    return best;
}

/* Windows' family names: the closest family there is */
static const char *const sans_names[] = {
    "segoe ui", "segoe ui variable", "segoe ui variable display", "segoe ui variable text", "segoe ui variable small",
    "segoe ui semibold", "segoe ui semilight", "segoe ui light", "segoe ui black", "segoe ui historic",
    "system-ui", "-apple-system", "blinkmacsystemfont", 0
};
static const char *const sans_to[] = { "Inter", "DejaVu Sans", "Liberation Sans", "Noto Sans", 0 };
static const char *const arial_names[] = {
    "arial", "helvetica", "helvetica neue", "tahoma", "verdana", "microsoft sans serif", "ms sans serif",
    "ms shell dlg", "ms shell dlg 2", "calibri", "candara", "corbel", "trebuchet ms", "arial unicode ms",
    "lucida sans unicode", "lucida grande", "franklin gothic medium", "sans-serif", "sans", "roboto", "open sans",
    "noto sans", "liberation sans", "dejavu sans", "inter", "arimo", 0
};
static const char *const arial_to[] = { "Liberation Sans", "Arimo", "DejaVu Sans", "Noto Sans", "Inter", 0 };
static const char *const serif_names[] = {
    "times new roman", "times", "georgia", "cambria", "palatino linotype", "palatino", "book antiqua", "garamond",
    "constantia", "sylfaen", "serif", "liberation serif", "dejavu serif", "noto serif", "tinos", 0
};
static const char *const serif_to[] = { "Liberation Serif", "Tinos", "DejaVu Serif", "Noto Serif", 0 };
static const char *const mono_names[] = {
    "consolas", "courier new", "courier", "lucida console", "cascadia mono", "cascadia code", "segoe ui mono",
    "monospace", "menlo", "monaco", "sf mono", "liberation mono", "dejavu sans mono", "jetbrains mono", "cousine",
    "noto sans mono", 0
};
static const char *const mono_to[] = { "Liberation Mono", "Cousine", "DejaVu Sans Mono", "JetBrains Mono",
                                       "Noto Sans Mono", 0 };
static const char *const emoji_names[] = { "segoe ui emoji", "segoe ui symbol", "apple color emoji",
                                           "noto color emoji", "segoe mdl2 assets", "segoe fluent icons", 0 };
static const char *const emoji_to[] = { "Noto Color Emoji", "DejaVu Sans", "Noto Sans Symbols", 0 };

static int find_family_exact(const coll_t *c, const WCHAR *name) {
    for (int i = 0; i < c->nfam; i++)
        if (wieq(c->fams[i].name, name)) return i;
    /* a face's plain family name ("DejaVu Sans Condensed", "Inter Medium") */
    for (int i = 0; i < c->nfam; i++)
        for (int j = 0; j < c->fams[i].n; j++)
            if (wieq(c->fams[i].fonts[j]->win_family, name)) return i;
    return -1;
}
static int find_family_ascii(const coll_t *c, const char *name) {
    WCHAR w[64];
    wfrom_ascii(w, 64, name);
    return find_family_exact(c, w);
}
static int find_family(const coll_t *c, const WCHAR *name) {
    static const char *const *const groups[][2] = {
        { sans_names, sans_to }, { arial_names, arial_to }, { serif_names, serif_to }, { mono_names, mono_to },
        { emoji_names, emoji_to },
    };
    int i = find_family_exact(c, name);
    if (i >= 0 || !c->system) return i;
    for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
        for (int k = 0; groups[g][0][k]; k++)
            if (wieq_ascii(name, groups[g][0][k])) {
                for (int t = 0; groups[g][1][t]; t++)
                    if ((i = find_family_ascii(c, groups[g][1][t])) >= 0) return i;
                /* none of those: any sans */
                for (int t = 0; sans_to[t]; t++)
                    if ((i = find_family_ascii(c, sans_to[t])) >= 0) return i;
                return c->nfam ? 0 : -1;
            }
    return -1;
}

/* ---- the system collection ---- */

static coll_t *sys_coll;
static coll_t *empty_coll;

static void scan_dir(coll_t *c, const char *dir, int depth) {
    DIR *dp = opendir(dir);
    struct dirent *e;
    if (!dp) return;
    while ((e = readdir(dp))) {
        char p[1024], wp[1024];
        const char *ext;
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan_dir(c, p, depth + 1);
            continue;
        }
        ext = strrchr(e->d_name, '.');
        if (!ext || (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf") && strcasecmp(ext, ".ttc") &&
                     strcasecmp(ext, ".otc")))
            continue;
        {
            src_t s = { 0, 0, open(p, O_RDONLY | O_CLOEXEC) };
            ffile_t *f;
            if (s.fd < 0) continue;
            if (path_to_win(p, wp, sizeof(wp)) != 0) snprintf(wp, sizeof(wp), "Z:%s", p);
            f = new_local_ffile(p, wp);
            if (f) {
                coll_add_file(c, f, &s);
                ffile_Release(f);                 /* the faces hold it */
            }
            close(s.fd);
        }
    }
    closedir(dp);
}

/* sorted by name, as Windows lists them */
static int fam_cmp(const void *a, const void *b) {
    const WCHAR *x = ((const famdata_t *)a)->name, *y = ((const famdata_t *)b)->name;
    for (; *x && *y; x++, y++) {
        WCHAR p = *x >= 'A' && *x <= 'Z' ? (WCHAR)(*x + 32) : *x, q = *y >= 'A' && *y <= 'Z' ? (WCHAR)(*y + 32) : *y;
        if (p != q) return p < q ? -1 : 1;
    }
    return *x ? 1 : *y ? -1 : 0;
}

static coll_t *system_collection(void) {
    lock();
    if (!sys_coll) {
        coll_t *c = new_coll();
        const char *env = getenv("ICDA_WIN_FONTS");
        char winfonts[1024];
        if (c) {
            c->system = 1;
            if (env && *env) {
                char *copy = strdup(env), *save = 0;
                for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(0, ":", &save)) scan_dir(c, d, 0);
                free(copy);
            }
            snprintf(winfonts, sizeof(winfonts), "%s/Windows/Fonts", path_prefix());
            scan_dir(c, winfonts, 0);
            scan_dir(c, "/usr/share/fonts", 0);
            scan_dir(c, "/linux/usr/share/fonts", 0);
            scan_dir(c, "/usr/local/share/fonts", 0);
            qsort(c->fams, (size_t)c->nfam, sizeof(*c->fams), fam_cmp);
            if (wtrace) {
                int faces = 0;
                for (int i = 0; i < c->nfam; i++) faces += c->fams[i].n;
                wlog("dwrite: %d font families (%d faces)", c->nfam, faces);
            }
            if (!c->nfam) wlog("dwrite: no fonts found (in /usr/share/fonts, /linux/usr/share/fonts, C:\\Windows\\Fonts)");
            sys_coll = c;
        }
    }
    unlock();
    return sys_coll;
}

/* ---- IDWriteFontCollection ---- */

static HRESULT WINAPI coll_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontCollection)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI coll_Release(void *self) {
    coll_t *c = self;
    LONG r = obj_unref(self);
    if (!r && !c->system && c != empty_coll) {
        for (int i = 0; i < c->nfam; i++) {
            for (int j = 0; j < c->fams[i].n; j++) fdesc_unref(c->fams[i].fonts[j]);
            free(c->fams[i].fonts);
        }
        free(c->fams);
        free(c);
    }
    return (ULONG)(r > 0 ? r : 0);
}
static UINT32 WINAPI coll_GetFontFamilyCount(void *self) { return (UINT32)((coll_t *)self)->nfam; }

static family_t *new_family(coll_t *c, int fam) {
    family_t *f = calloc(1, sizeof(*f));
    if (!f) return 0;
    f->o.vt = family_vtbl;
    f->o.refs = 1;
    f->c = c;
    f->fam = fam;
    obj_AddRef(c);
    return f;
}
static font_t *new_font(coll_t *c, int fam, fdesc_t *d, UINT32 sims) {
    font_t *f = calloc(1, sizeof(*f));
    if (!f) return 0;
    f->o.vt = font_vtbl;
    f->o.refs = 1;
    f->c = c;
    f->fam = fam;
    f->d = d;
    f->sims = sims;
    obj_AddRef(c);
    return f;
}

static HRESULT WINAPI coll_GetFontFamily(void *self, UINT32 i, void **out) {
    coll_t *c = self;
    *out = 0;
    if (i >= (UINT32)c->nfam) return E_INVALIDARG_;
    return (*out = new_family(c, (int)i)) ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI coll_FindFamilyName(void *self, const WCHAR *name, UINT32 *index, BOOL *exists) {
    int i = name ? find_family(self, name) : -1;
    if (wtrace >= 2) {
        char n[128] = "";
        if (name) w_to_utf8(name, -1, n, sizeof(n));
        wlog("dwrite: family \"%s\": %s", n, i >= 0 ? "found" : "not there");
    }
    *exists = i >= 0;
    *index = i >= 0 ? (UINT32)i : 0xFFFFFFFFu;
    return S_OK_;
}
static HRESULT WINAPI coll_GetFontFromFontFace(void *self, void *face, void **out) {
    coll_t *c = self;
    face_t *f = as_face(face);
    *out = 0;
    if (!f) return E_INVALIDARG_;
    for (int i = 0; i < c->nfam; i++)
        for (int j = 0; j < c->fams[i].n; j++) {
            fdesc_t *d = c->fams[i].fonts[j];
            if (d == f->d || (d->index == f->d->index && same_file(d->file, f->d->file)))
                return (*out = new_font(c, i, d, f->sims)) ? S_OK_ : E_OUTOFMEMORY_;
        }
    return DWRITE_E_NOFONT;
}
static void *const coll_vtbl[7] = {
    coll_QueryInterface, obj_AddRef, coll_Release, coll_GetFontFamilyCount, coll_GetFontFamily,
    coll_FindFamilyName, coll_GetFontFromFontFace,
};

/* ---- IDWriteFontFamily (and IDWriteFontList) ---- */

static HRESULT WINAPI family_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontList) || iid_is(iid, &IID_FontFamily)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI family_Release(void *self) {
    family_t *f = self;
    LONG r = obj_unref(self);
    if (!r) {
        coll_Release(f->c);
        free(f);
    }
    return (ULONG)r;
}
static HRESULT WINAPI family_GetFontCollection(void *self, void **out) {
    family_t *f = self;
    obj_AddRef(f->c);
    *out = f->c;
    return S_OK_;
}
static UINT32 WINAPI family_GetFontCount(void *self) {
    family_t *f = self;
    return (UINT32)f->c->fams[f->fam].n;
}
static HRESULT WINAPI family_GetFont(void *self, UINT32 i, void **out) {
    family_t *f = self;
    famdata_t *fd = &f->c->fams[f->fam];
    *out = 0;
    if (i >= (UINT32)fd->n) return E_INVALIDARG_;
    return (*out = new_font(f->c, f->fam, fd->fonts[i], 0)) ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI family_GetFamilyNames(void *self, void **out) {
    family_t *f = self;
    return new_strings(f->c->fams[f->fam].name, out);
}
static HRESULT WINAPI family_GetFirstMatchingFont(void *self, UINT32 weight, UINT32 stretch, UINT32 style,
                                                  void **out) {
    family_t *f = self;
    UINT32 sims;
    int i = best_match(&f->c->fams[f->fam], (int)weight, (int)stretch, (int)style, &sims);
    *out = 0;
    if (i < 0) return DWRITE_E_NOFONT;
    return (*out = new_font(f->c, f->fam, f->c->fams[f->fam].fonts[i], sims)) ? S_OK_ : E_OUTOFMEMORY_;
}

static const famdata_t *sort_fam;
static int sort_w, sort_st, sort_sty;
static int list_cmp(const void *a, const void *b) {
    long x = match_score(sort_fam->fonts[*(const int *)a], sort_w, sort_st, sort_sty);
    long y = match_score(sort_fam->fonts[*(const int *)b], sort_w, sort_st, sort_sty);
    return x < y ? -1 : x > y;
}
static HRESULT WINAPI family_GetMatchingFonts(void *self, UINT32 weight, UINT32 stretch, UINT32 style, void **out) {
    family_t *f = self;
    famdata_t *fd = &f->c->fams[f->fam];
    fontlist_t *l = calloc(1, sizeof(*l));
    *out = 0;
    if (!l || !(l->idx = malloc(sizeof(int) * (size_t)(fd->n ? fd->n : 1)))) {
        free(l);
        return E_OUTOFMEMORY_;
    }
    l->o.vt = fontlist_vtbl;
    l->o.refs = 1;
    l->c = f->c;
    obj_AddRef(f->c);
    l->fam = f->fam;
    l->n = fd->n;
    for (int i = 0; i < fd->n; i++) l->idx[i] = i;
    lock();
    sort_fam = fd, sort_w = (int)weight, sort_st = (int)stretch, sort_sty = (int)style;
    qsort(l->idx, (size_t)l->n, sizeof(int), list_cmp);
    unlock();
    best_match(fd, (int)weight, (int)stretch, (int)style, &l->sims);
    *out = l;
    return S_OK_;
}
static void *const family_vtbl[9] = {
    family_QueryInterface, obj_AddRef, family_Release, family_GetFontCollection, family_GetFontCount,
    family_GetFont, family_GetFamilyNames, family_GetFirstMatchingFont, family_GetMatchingFonts,
};

static HRESULT WINAPI fontlist_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontList)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI fontlist_Release(void *self) {
    fontlist_t *l = self;
    LONG r = obj_unref(self);
    if (!r) {
        coll_Release(l->c);
        free(l->idx);
        free(l);
    }
    return (ULONG)r;
}
static UINT32 WINAPI fontlist_GetFontCount(void *self) { return (UINT32)((fontlist_t *)self)->n; }
static HRESULT WINAPI fontlist_GetFont(void *self, UINT32 i, void **out) {
    fontlist_t *l = self;
    *out = 0;
    if (i >= (UINT32)l->n) return E_INVALIDARG_;
    return (*out = new_font(l->c, l->fam, l->c->fams[l->fam].fonts[l->idx[i]], i ? 0 : l->sims)) ? S_OK_
                                                                                                 : E_OUTOFMEMORY_;
}
static void *const fontlist_vtbl[6] = {
    fontlist_QueryInterface, obj_AddRef, fontlist_Release, family_GetFontCollection, fontlist_GetFontCount,
    fontlist_GetFont,
};

/* ---- IDWriteFont2 ---- */

static HRESULT WINAPI font_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_Font) || iid_is(iid, &IID_Font1) || iid_is(iid, &IID_Font2)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI font_Release(void *self) {
    font_t *f = self;
    LONG r = obj_unref(self);
    if (!r) {
        coll_Release(f->c);
        free(f);
    }
    return (ULONG)r;
}
static HRESULT WINAPI font_GetFontFamily(void *self, void **out) {
    font_t *f = self;
    return (*out = new_family(f->c, f->fam)) ? S_OK_ : E_OUTOFMEMORY_;
}
static UINT32 WINAPI font_GetWeight(void *self) {
    font_t *f = self;
    return (f->sims & SIM_BOLD) && f->d->weight < 700 ? 700 : f->d->weight;
}
static UINT32 WINAPI font_GetStretch(void *self) { return ((font_t *)self)->d->stretch; }
static UINT32 WINAPI font_GetStyle(void *self) {
    font_t *f = self;
    return (f->sims & SIM_OBLIQUE) && f->d->style == STYLE_NORMAL ? STYLE_OBLIQUE : f->d->style;
}
static BOOL WINAPI font_IsSymbolFont(void *self) { return ((font_t *)self)->d->symbol; }
static HRESULT WINAPI font_GetFaceNames(void *self, void **out) {
    font_t *f = self;
    WCHAR name[96];
    wcopy(name, 96, f->d->face);
    if (f->sims & SIM_BOLD) {
        static const WCHAR bold[] = { ' ', 'B', 'o', 'l', 'd', 0 };
        size_t n = w_len(name);
        if (n + 6 < 96) memcpy(name + n, bold, sizeof(bold));
    }
    return new_strings(name, out);
}
static HRESULT WINAPI font_GetInformationalStrings(void *self, UINT32 id, void **out, BOOL *exists) {
    font_t *f = self;
    const WCHAR *s = 0;
    switch (id) {
    case 11: s = f->d->win_family; break;                 /* WIN32_FAMILY_NAMES */
    case 12: s = f->d->win_face; break;                   /* WIN32_SUBFAMILY_NAMES */
    case 13: s = f->d->has_typo_family ? f->d->family : 0; break;   /* TYPOGRAPHIC_FAMILY_NAMES */
    case 14: s = f->d->has_typo_family ? f->d->face : 0; break;
    case 16: s = f->d->full; break;                       /* FULL_NAME */
    case 17: s = f->d->psname; break;                     /* POSTSCRIPT_NAME */
    case 19: s = f->d->family; break;                     /* WEIGHT_STRETCH_STYLE_FAMILY_NAME */
    default: break;
    }
    *out = 0;
    *exists = s && s[0];
    if (!*exists) return S_OK_;
    return new_strings(s, out);
}
static UINT32 WINAPI font_GetSimulations(void *self) { return ((font_t *)self)->sims; }
static void WINAPI font_GetMetrics(void *self, DWRITE_FONT_METRICS *m) { *m = ((font_t *)self)->d->m.m; }
static HRESULT WINAPI font_HasCharacter(void *self, UINT32 cp, BOOL *exists) {
    *exists = has_char(((font_t *)self)->d, cp);
    return S_OK_;
}
static HRESULT WINAPI font_CreateFontFace(void *self, void **out) {
    font_t *f = self;
    return (*out = get_face(f->d, f->sims)) ? S_OK_ : E_OUTOFMEMORY_;
}
static void WINAPI font_GetMetrics1(void *self, DWRITE_FONT_METRICS1 *m) { *m = ((font_t *)self)->d->m; }
static void WINAPI font_GetPanose(void *self, void *p) { memcpy(p, ((font_t *)self)->d->panose, 10); }
static HRESULT WINAPI font_GetUnicodeRanges(void *self, UINT32 max, DWRITE_UNICODE_RANGE *r, UINT32 *n) {
    return unicode_ranges(((font_t *)self)->d, max, r, n);
}
static BOOL WINAPI font_IsMonospacedFont(void *self) { return ((font_t *)self)->d->mono; }
static BOOL WINAPI font_IsColorFont(void *self) { (void)self; return FALSE; }
static void *const font_vtbl[19] = {
    font_QueryInterface, obj_AddRef, font_Release, font_GetFontFamily, font_GetWeight, font_GetStretch,
    font_GetStyle, font_IsSymbolFont, font_GetFaceNames, font_GetInformationalStrings, font_GetSimulations,
    font_GetMetrics, font_HasCharacter, font_CreateFontFace,
    font_GetMetrics1, font_GetPanose, font_GetUnicodeRanges, font_IsMonospacedFont,
    font_IsColorFont,
};
_Static_assert(sizeof(font_vtbl) / sizeof(font_vtbl[0]) == 19, "IDWriteFont2 has 19 methods");

/* ---- glyph run analysis: coverage of a glyph run --------------------------------------------- */

typedef struct {
    obj_t    o;
    RECT     bounds;            /* empty when nothing is drawn */
    UINT32   texture;           /* the texture type it gives */
    uint8_t *alpha;             /* the bounds' coverage, a byte a pixel */
} analysis_t;

static HRESULT WINAPI analysis_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_GlyphRunAnalysis)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI analysis_Release(void *self) {
    analysis_t *a = self;
    LONG r = obj_unref(self);
    if (!r) {
        free(a->alpha);
        free(a);
    }
    return (ULONG)r;
}
static HRESULT WINAPI analysis_GetAlphaTextureBounds(void *self, UINT32 type, RECT *r) {
    analysis_t *a = self;
    if (type > TEX_CLEARTYPE_3x1) return E_INVALIDARG_;
    if (type != a->texture) memset(r, 0, sizeof(*r));
    else *r = a->bounds;
    return S_OK_;
}
static HRESULT WINAPI analysis_CreateAlphaTexture(void *self, UINT32 type, const RECT *r, BYTE *out, UINT32 size) {
    analysis_t *a = self;
    LONG w, h, bw = a->bounds.right - a->bounds.left;
    int k = type == TEX_CLEARTYPE_3x1 ? 3 : 1;
    if (type > TEX_CLEARTYPE_3x1 || !r || !out) return E_INVALIDARG_;
    w = r->right - r->left, h = r->bottom - r->top;
    if (w <= 0 || h <= 0) return S_OK_;
    if ((uint64_t)w * (uint64_t)h * (uint64_t)k > size) return E_NOT_SUFFICIENT_BUFFER_;
    if (type != a->texture) return DWRITE_E_UNSUPPORTEDOP;
    for (LONG y = 0; y < h; y++) {
        LONG sy = r->top + y;
        BYTE *row = out + (size_t)y * (size_t)w * (size_t)k;
        if (sy < a->bounds.top || sy >= a->bounds.bottom || !a->alpha) {
            memset(row, 0, (size_t)w * (size_t)k);
            continue;
        }
        for (LONG x = 0; x < w; x++) {
            LONG sx = r->left + x;
            BYTE v = 0;
            if (sx >= a->bounds.left && sx < a->bounds.right)
                v = a->alpha[(size_t)(sy - a->bounds.top) * (size_t)bw + (size_t)(sx - a->bounds.left)];
            if (k == 3) row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = v;
            else row[x] = v;
        }
    }
    return S_OK_;
}
typedef FLOAT (WINAPI *float_fn)(void *);
static HRESULT WINAPI analysis_GetAlphaBlendParams(void *self, void *params, FLOAT *gamma, FLOAT *contrast,
                                                  FLOAT *cleartype) {
    (void)self;
    if (params) {
        *gamma = ((float_fn)VTBL(params)[3])(params);
        *contrast = ((float_fn)VTBL(params)[4])(params);
        *cleartype = ((float_fn)VTBL(params)[5])(params);
    } else {
        *gamma = 1.8f, *contrast = 0.5f, *cleartype = 0.0f;
    }
    return S_OK_;
}
static void *const analysis_vtbl[] = {
    analysis_QueryInterface, obj_AddRef, analysis_Release, analysis_GetAlphaTextureBounds,
    analysis_CreateAlphaTexture, analysis_GetAlphaBlendParams,
};

/* the run's glyphs as one outline in device pixels, then FreeType's
 * anti-aliased scan converter; the bounds are the pixels it covered */
static HRESULT make_analysis(const DWRITE_GLYPH_RUN *run, FLOAT ppdip, const DWRITE_MATRIX *m, UINT32 mode,
                             UINT32 antialias, FLOAT bx, FLOAT by, void **out) {
    face_t *f = run ? as_face(run->fontFace) : 0;
    analysis_t *a;
    DWRITE_MATRIX t = { 1, 0, 0, 1, 0, 0 };
    double scale, pen = 0, minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    double *px = 0, *py = 0;
    char *tags = 0;
    short *ends = 0;
    int np = 0, nc = 0, capp = 0, capc = 0, rtl;
    *out = 0;
    if (!f || !run->glyphCount || !run->glyphIndices) return E_INVALIDARG_;
    if (mode == RM_OUTLINE || mode > 7) return E_INVALIDARG_;
    if (m) t = *m;
    if (ppdip <= 0) ppdip = 1;
    a = calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY_;
    a->o.vt = analysis_vtbl;
    a->o.refs = 1;
    a->texture = (mode == RM_ALIASED || antialias == AA_GRAYSCALE) ? TEX_ALIASED_1x1 : TEX_CLEARTYPE_3x1;
    scale = run->fontEmSize / f->d->m.m.designUnitsPerEm;
    rtl = run->bidiLevel & 1;
    lock();
    for (UINT32 i = 0; i < run->glyphCount; i++) {
        FT_Outline o;
        FT_Pos adv = 0;
        double advance, ox, oy;
        if (glyph_outline(f->d, f->sims, run->glyphIndices[i], &o, &adv) != 0) memset(&o, 0, sizeof(o)), adv = 0;
        advance = run->glyphAdvances ? run->glyphAdvances[i] : adv * scale;
        if (rtl) pen -= advance;
        ox = pen + (run->glyphOffsets ? (rtl ? -1 : 1) * run->glyphOffsets[i].advanceOffset : 0);
        oy = run->glyphOffsets ? -run->glyphOffsets[i].ascenderOffset : 0;
        if (!rtl) pen += advance;
        if (np + o.n_points > capp) {
            int cap = (np + o.n_points) * 2 + 64;
            double *nx = realloc(px, sizeof(double) * (size_t)cap), *ny;
            if (nx) px = nx;
            ny = realloc(py, sizeof(double) * (size_t)cap);
            if (ny) py = ny;
            char *nt = realloc(tags, (size_t)cap);
            if (nt) tags = nt;
            if (!nx || !ny || !nt) {
                if (o.n_points || o.n_contours) FT_Outline_Done(ft_lib, &o);
                break;
            }
            capp = cap;
        }
        if (nc + o.n_contours > capc) {
            int cap = (nc + o.n_contours) * 2 + 16;
            short *ne = realloc(ends, sizeof(short) * (size_t)cap);
            if (!ne) {
                if (o.n_points || o.n_contours) FT_Outline_Done(ft_lib, &o);
                break;
            }
            ends = ne, capc = cap;
        }
        for (int k = 0; k < o.n_points; k++) {
            /* DIPs (y down), then the transform, in pixels */
            double x = bx + ox + o.points[k].x * scale, y = by + oy - o.points[k].y * scale;
            double dxp = (x * t.m11 + y * t.m21) * ppdip + t.dx * ppdip;
            double dyp = (x * t.m12 + y * t.m22) * ppdip + t.dy * ppdip;
            px[np + k] = dxp, py[np + k] = dyp;
            tags[np + k] = (char)(o.tags[k] & 3);
            if (dxp < minx) minx = dxp;
            if (dxp > maxx) maxx = dxp;
            if (dyp < miny) miny = dyp;
            if (dyp > maxy) maxy = dyp;
        }
        for (int k = 0; k < o.n_contours; k++) ends[nc + k] = (short)(np + o.contours[k]);
        np += o.n_points, nc += o.n_contours;
        if (o.n_points || o.n_contours) FT_Outline_Done(ft_lib, &o);
    }
    if (np > 0 && np < 32767 && maxx - minx < 8192 && maxy - miny < 8192) {
        LONG left = (LONG)floor(minx), top = (LONG)floor(miny), right = (LONG)ceil(maxx), bottom = (LONG)ceil(maxy);
        LONG w = right - left + 1, h = bottom - top + 1;
        uint8_t *buf = calloc((size_t)w * (size_t)h, 1);
        FT_Outline ol;
        if (buf && !FT_Outline_New(ft_lib, (FT_UInt)np, nc, &ol)) {
            FT_Bitmap bm;
            int mono = mode == RM_ALIASED;
            uint8_t *mbuf = 0;
            LONG pitch = mono ? (w + 7) / 8 : w;
            for (int k = 0; k < np; k++) {
                ol.points[k].x = (FT_Pos)lround((px[k] - left) * 64.0);
                ol.points[k].y = (FT_Pos)lround(((double)(top + h) - py[k]) * 64.0);
                ol.tags[k] = tags[k];
            }
            for (int k = 0; k < nc; k++) ol.contours[k] = ends[k];
            ol.flags = FT_OUTLINE_NONE;
            memset(&bm, 0, sizeof(bm));
            bm.rows = (unsigned)h, bm.width = (unsigned)w, bm.pitch = (int)pitch;
            bm.pixel_mode = mono ? FT_PIXEL_MODE_MONO : FT_PIXEL_MODE_GRAY;
            bm.num_grays = mono ? 2 : 256;
            if (mono) mbuf = calloc((size_t)pitch * (size_t)h, 1);
            bm.buffer = mono ? mbuf : buf;
            if (bm.buffer && !FT_Outline_Get_Bitmap(ft_lib, &ol, &bm)) {
                LONG x0 = w, y0 = h, x1 = -1, y1 = -1;
                if (mono)
                    for (LONG y = 0; y < h; y++)
                        for (LONG x = 0; x < w; x++)
                            buf[y * w + x] = (mbuf[y * pitch + x / 8] & (0x80 >> (x & 7))) ? 255 : 0;
                for (LONG y = 0; y < h; y++)
                    for (LONG x = 0; x < w; x++)
                        if (buf[y * w + x]) {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                if (x1 >= 0) {
                    LONG cw = x1 - x0 + 1, ch = y1 - y0 + 1;
                    a->alpha = malloc((size_t)cw * (size_t)ch);
                    if (a->alpha) {
                        for (LONG y = 0; y < ch; y++) memcpy(a->alpha + y * cw, buf + (y0 + y) * w + x0, (size_t)cw);
                        a->bounds.left = left + x0, a->bounds.top = top + y0;
                        a->bounds.right = a->bounds.left + cw, a->bounds.bottom = a->bounds.top + ch;
                    }
                }
            }
            free(mbuf);
            FT_Outline_Done(ft_lib, &ol);
        }
        free(buf);
    }
    unlock();
    free(px), free(py), free(tags), free(ends);
    *out = a;
    return S_OK_;
}

/* ---- rendering parameters ------------------------------------------------------------------------ */

typedef struct {
    obj_t  o;
    FLOAT  gamma, contrast, gray_contrast, cleartype;
    UINT32 geometry, mode, gridfit;
} params_t;

static HRESULT WINAPI params_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_RenderingParams) || iid_is(iid, &IID_RenderingParams1) ||
        iid_is(iid, &IID_RenderingParams2) || iid_is(iid, &IID_RenderingParams3)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI params_Release(void *self) {
    LONG r = obj_unref(self);
    if (!r) free(self);
    return (ULONG)r;
}
static FLOAT WINAPI params_GetGamma(void *self) { return ((params_t *)self)->gamma; }
static FLOAT WINAPI params_GetEnhancedContrast(void *self) { return ((params_t *)self)->contrast; }
static FLOAT WINAPI params_GetClearTypeLevel(void *self) { return ((params_t *)self)->cleartype; }
static UINT32 WINAPI params_GetPixelGeometry(void *self) { return ((params_t *)self)->geometry; }
static UINT32 WINAPI params_GetRenderingMode(void *self) {
    UINT32 m = ((params_t *)self)->mode;
    return m > RM_OUTLINE ? RM_NATURAL_SYMMETRIC : m;
}
static FLOAT WINAPI params_GetGrayscaleEnhancedContrast(void *self) { return ((params_t *)self)->gray_contrast; }
static UINT32 WINAPI params_GetGridFitMode(void *self) { return ((params_t *)self)->gridfit; }
static UINT32 WINAPI params_GetRenderingMode1(void *self) { return ((params_t *)self)->mode; }
static void *const params_vtbl[] = {
    params_QueryInterface, obj_AddRef, params_Release, params_GetGamma, params_GetEnhancedContrast,
    params_GetClearTypeLevel, params_GetPixelGeometry, params_GetRenderingMode,
    params_GetGrayscaleEnhancedContrast, params_GetGridFitMode, params_GetRenderingMode1,
};

static HRESULT new_params(FLOAT gamma, FLOAT contrast, FLOAT gray, FLOAT cleartype, UINT32 geometry, UINT32 mode,
                          UINT32 gridfit, void **out) {
    params_t *p = calloc(1, sizeof(*p));
    *out = 0;
    if (!p) return E_OUTOFMEMORY_;
    if (gamma <= 0 || gamma > 256 || contrast < 0 || gray < 0 || cleartype < 0 || cleartype > 1 || geometry > 2) {
        free(p);
        return E_INVALIDARG_;
    }
    p->o.vt = params_vtbl;
    p->o.refs = 1;
    p->gamma = gamma, p->contrast = contrast, p->gray_contrast = gray, p->cleartype = cleartype;
    p->geometry = geometry, p->mode = mode, p->gridfit = gridfit;
    *out = p;
    return S_OK_;
}

/* ---- font fallback ------------------------------------------------------------------------------- */

typedef HRESULT (WINAPI *text_at_fn)(void *, UINT32, const WCHAR **, UINT32 *);

static UINT32 next_cp(const WCHAR *t, UINT32 n, UINT32 i, UINT32 *units) {
    if (i + 1 < n && t[i] >= 0xD800 && t[i] < 0xDC00 && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
        *units = 2;
        return 0x10000 + ((UINT32)(t[i] - 0xD800) << 10) + (t[i + 1] - 0xDC00);
    }
    *units = 1;
    return t[i];
}

/* families tried after the asked one, in order */
static const char *const fallback_order[] = {
    "Inter", "DejaVu Sans", "Liberation Sans", "Noto Sans", "DejaVu Serif", "Noto Sans Symbols",
    "Noto Sans Symbols 2", "Noto Color Emoji", "Noto Sans CJK SC", "Noto Sans CJK JP", "Noto Sans Arabic",
    "Noto Sans Hebrew", "Noto Sans Devanagari", "Noto Sans Thai", "DejaVu Sans Mono", "JetBrains Mono", 0
};

static HRESULT WINAPI fallback_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_FontFallback)) {
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static HRESULT WINAPI fallback_MapCharacters(void *self, void *source, UINT32 pos, UINT32 len, void *base,
                                             const WCHAR *base_family, UINT32 weight, UINT32 style, UINT32 stretch,
                                             UINT32 *mapped_len, void **mapped, FLOAT *scale) {
    WCHAR text[256];
    UINT32 n = 0, units, cp;
    coll_t *c = as_coll(base);
    fdesc_t *pick = 0;
    int pick_fam = -1;
    UINT32 pick_sims = 0;
    (void)self;
    *mapped_len = 0;
    *mapped = 0;
    *scale = 1.0f;
    if (!source || !len) return E_INVALIDARG_;
    while (n < len && n < 256) {                  /* the text, from the program's source */
        const WCHAR *t = 0;
        UINT32 tl = 0;
        if (((text_at_fn)VTBL(source)[3])(source, pos + n, &t, &tl) < 0 || !t || !tl) break;
        for (UINT32 i = 0; i < tl && n < len && n < 256; i++) text[n++] = t[i];
    }
    if (!n) return E_INVALIDARG_;
    if (!c) c = system_collection();
    if (!c) return E_FAIL_;
    cp = next_cp(text, n, 0, &units);
    /* the asked family, then the system's in fallback order, then any */
    {
        int fams[64], nf = 0;
        int i = base_family && base_family[0] ? find_family(c, base_family) : -1;
        if (i >= 0) fams[nf++] = i;
        if (c != sys_coll) c = system_collection();
        for (int k = 0; fallback_order[k] && nf < 48; k++)
            if ((i = find_family_ascii(c, fallback_order[k])) >= 0) fams[nf++] = i;
        for (int k = 0; k < nf && !pick; k++) {
            famdata_t *fd = &c->fams[fams[k]];
            UINT32 sims;
            int b = best_match(fd, (int)weight, (int)stretch, (int)style, &sims);
            if (b >= 0 && (cp == ' ' || has_char(fd->fonts[b], cp))) pick = fd->fonts[b], pick_fam = fams[k], pick_sims = sims;
        }
        for (int k = 0; k < c->nfam && !pick; k++) {
            famdata_t *fd = &c->fams[k];
            UINT32 sims;
            int b = best_match(fd, (int)weight, (int)stretch, (int)style, &sims);
            if (b >= 0 && has_char(fd->fonts[b], cp)) pick = fd->fonts[b], pick_fam = k, pick_sims = sims;
        }
    }
    if (!pick) {                                     /* no font has it: no font for this character */
        *mapped_len = units;
        return S_OK_;
    }
    {
        UINT32 i = units;
        while (i < n) {
            UINT32 u, ch = next_cp(text, n, i, &u);
            if (ch != ' ' && !has_char(pick, ch)) break;
            i += u;
        }
        *mapped_len = i;
    }
    *mapped = new_font(c, pick_fam, pick, pick_sims);
    return *mapped ? S_OK_ : E_OUTOFMEMORY_;
}
static void *const fallback_vtbl[] = { fallback_QueryInterface, static_AddRef, static_Release, fallback_MapCharacters };
static obj_t the_fallback = { fallback_vtbl, 1 };

/* ---- number substitution (only kept) ---------------------------------------------------------------- */

static HRESULT WINAPI numsub_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_NumberSubstitution)) {
        obj_AddRef(self);
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static ULONG WINAPI numsub_Release(void *self) {
    LONG r = obj_unref(self);
    if (!r) free(self);
    return (ULONG)r;
}
static void *const numsub_vtbl[] = { numsub_QueryInterface, obj_AddRef, numsub_Release };

/* ---- GDI interop -------------------------------------------------------------------------------------- */

static void to_logfont(const fdesc_t *d, UINT32 sims, DW_LOGFONTW *lf) {
    memset(lf, 0, sizeof(*lf));
    lf->lfWeight = (sims & SIM_BOLD) && d->weight < 700 ? 700 : d->weight;
    lf->lfItalic = d->style != STYLE_NORMAL || (sims & SIM_OBLIQUE);
    lf->lfCharSet = 1;                                  /* DEFAULT_CHARSET */
    lf->lfOutPrecision = 4;                             /* OUT_TT_PRECIS */
    lf->lfQuality = 0;
    lf->lfPitchAndFamily = d->mono ? 1 : 2;
    wcopy(lf->lfFaceName, 32, d->win_family[0] ? d->win_family : d->family);
}

static HRESULT WINAPI gdi_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_GdiInterop)) {
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static HRESULT WINAPI gdi_CreateFontFromLOGFONT(void *self, const DW_LOGFONTW *lf, void **out) {
    coll_t *c = system_collection();
    int fam;
    UINT32 sims;
    int b;
    (void)self;
    *out = 0;
    if (!lf) return E_INVALIDARG_;
    if (!c || (fam = find_family(c, lf->lfFaceName)) < 0) return DWRITE_E_NOFONT;
    b = best_match(&c->fams[fam], lf->lfWeight ? (int)lf->lfWeight : 400, 5, lf->lfItalic ? STYLE_ITALIC : STYLE_NORMAL,
                   &sims);
    if (b < 0) return DWRITE_E_NOFONT;
    return (*out = new_font(c, fam, c->fams[fam].fonts[b], sims)) ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI gdi_ConvertFontToLOGFONT(void *self, void *font, DW_LOGFONTW *lf, BOOL *system) {
    font_t *f = as_font(font);
    (void)self;
    if (!f || !lf) return E_INVALIDARG_;
    to_logfont(f->d, f->sims, lf);
    if (system) *system = f->c->system;
    return S_OK_;
}
static HRESULT WINAPI gdi_ConvertFontFaceToLOGFONT(void *self, void *face, DW_LOGFONTW *lf) {
    face_t *f = as_face(face);
    (void)self;
    if (!f || !lf) return E_INVALIDARG_;
    to_logfont(f->d, f->sims, lf);
    return S_OK_;
}
static HRESULT WINAPI gdi_CreateFontFaceFromHdc(void *self, HDC hdc, void **out) {
    (void)self, (void)hdc;
    return notimpl("IDWriteGdiInterop::CreateFontFaceFromHdc", out);
}
static HRESULT WINAPI gdi_CreateBitmapRenderTarget(void *self, HDC hdc, UINT32 w, UINT32 h, void **out) {
    (void)self, (void)hdc, (void)w, (void)h;
    return notimpl("IDWriteGdiInterop::CreateBitmapRenderTarget", out);
}
static void *const gdi_vtbl[] = {
    gdi_QueryInterface, static_AddRef, static_Release, gdi_CreateFontFromLOGFONT, gdi_ConvertFontToLOGFONT,
    gdi_ConvertFontFaceToLOGFONT, gdi_CreateFontFaceFromHdc, gdi_CreateBitmapRenderTarget,
};
static obj_t the_gdi = { gdi_vtbl, 1 };

/* ---- the factory ----------------------------------------------------------------------------------- */

#define MAX_LOADERS 256
static void *file_loaders[MAX_LOADERS], *coll_loaders[MAX_LOADERS];
static int nfile_loaders, ncoll_loaders;

static int loader_index(void **tab, int n, void *l) {
    for (int i = 0; i < n; i++)
        if (tab[i] == l) return i;
    return -1;
}
static HRESULT loader_add(void **tab, int *n, void *l) {
    HRESULT r = S_OK_;
    if (!l) return E_INVALIDARG_;
    lock();
    if (loader_index(tab, *n, l) >= 0) r = DWRITE_E_ALREADYREGISTERED;
    else if (*n == MAX_LOADERS) r = E_FAIL_;
    else tab[(*n)++] = l;
    unlock();
    if (r == S_OK_) com_addref(l);
    return r;
}
static HRESULT loader_remove(void **tab, int *n, void *l) {
    int i;
    lock();
    i = loader_index(tab, *n, l);
    if (i >= 0) tab[i] = tab[--*n];
    unlock();
    if (i < 0) return E_INVALIDARG_;
    com_release(l);
    return S_OK_;
}

static void *const factory_vtbl[31];
static obj_t the_factory = { factory_vtbl, 1 };

static HRESULT WINAPI factory_QueryInterface(void *self, const GUID *iid, void **out) {
    if (iid_is(iid, &IID_IUnknown_) || iid_is(iid, &IID_Factory) || iid_is(iid, &IID_Factory1) ||
        iid_is(iid, &IID_Factory2)) {
        *out = self;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}
static HRESULT WINAPI factory_GetSystemFontCollection(void *self, void **out, BOOL check) {
    coll_t *c = system_collection();
    (void)self, (void)check;
    *out = c;
    if (!c) return E_OUTOFMEMORY_;
    obj_AddRef(c);
    return S_OK_;
}
typedef HRESULT (WINAPI *enum_from_key_fn)(void *, void *, const void *, UINT32, void **);
typedef HRESULT (WINAPI *move_next_fn)(void *, BOOL *);
typedef HRESULT (WINAPI *current_file_fn)(void *, void **);
static HRESULT WINAPI factory_CreateCustomFontCollection(void *self, void *loader, const void *key, UINT32 size,
                                                         void **out) {
    void *en = 0;
    coll_t *c;
    BOOL more = FALSE;
    int known;
    *out = 0;
    if (!loader) return E_INVALIDARG_;
    lock();
    known = loader_index(coll_loaders, ncoll_loaders, loader) >= 0;
    unlock();
    if (!known) return E_INVALIDARG_;
    if (((enum_from_key_fn)VTBL(loader)[3])(loader, self, key, size, &en) < 0 || !en) return E_FAIL_;
    c = new_coll();
    if (!c) {
        com_release(en);
        return E_OUTOFMEMORY_;
    }
    while (((move_next_fn)VTBL(en)[3])(en, &more) >= 0 && more) {
        void *file = 0;
        ffile_t *f;
        fdata_t *d;
        if (((current_file_fn)VTBL(en)[4])(en, &file) < 0 || !file) continue;
        if ((f = as_ffile(file)) && (d = ffile_data(f))) {
            src_t s = { d->bytes, d->size, -1 };
            coll_add_file(c, f, &s);
            fdata_unref(d);
        }
        com_release(file);
    }
    com_release(en);
    *out = c;
    return S_OK_;
}
static HRESULT WINAPI factory_RegisterFontCollectionLoader(void *self, void *l) {
    (void)self;
    return loader_add(coll_loaders, &ncoll_loaders, l);
}
static HRESULT WINAPI factory_UnregisterFontCollectionLoader(void *self, void *l) {
    (void)self;
    return loader_remove(coll_loaders, &ncoll_loaders, l);
}
static HRESULT WINAPI factory_CreateFontFileReference(void *self, const WCHAR *path, const FILETIME *t, void **out) {
    char wp[1024], up[1024];
    ffile_t *f;
    struct stat st;
    (void)self, (void)t;
    *out = 0;
    if (!path || w_to_utf8(path, -1, wp, sizeof(wp)) <= 0 || path_to_unix(wp, up, sizeof(up)) != 0)
        return E_INVALIDARG_;
    if (stat(up, &st) != 0) return DWRITE_E_FILENOTFOUND;
    f = new_local_ffile(up, wp);
    *out = f;
    return f ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI factory_CreateCustomFontFileReference(void *self, const void *key, UINT32 size, void *loader,
                                                            void **out) {
    int known;
    (void)self;
    *out = 0;
    if (!loader || (!key && size)) return E_INVALIDARG_;
    lock();
    known = loader == &local_loader || loader_index(file_loaders, nfile_loaders, loader) >= 0;
    unlock();
    if (!known) return E_INVALIDARG_;
    if (loader == &local_loader) {
        char up[1024];
        ffile_t *f;
        if (local_key_path(key, size, up, sizeof(up)) != 0) return E_INVALIDARG_;
        f = new_ffile(loader, key, size, up);
        *out = f;
        return f ? S_OK_ : E_OUTOFMEMORY_;
    }
    *out = new_ffile(loader, key, size, 0);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI factory_CreateFontFace(void *self, UINT32 type, UINT32 nfiles, void *const *files, UINT32 index,
                                             UINT32 sims, void **out) {
    ffile_t *f = nfiles == 1 && files ? as_ffile(files[0]) : 0;
    fdata_t *data;
    fdesc_t *d;
    face_t *face;
    src_t s = { 0, 0, -1 };
    (void)self, (void)type;
    *out = 0;
    if (!f || sims > 3) return E_INVALIDARG_;
    if (!(data = ffile_data(f))) return DWRITE_E_FILENOTFOUND;
    d = calloc(1, sizeof(*d));
    if (!d) {
        fdata_unref(data);
        return E_OUTOFMEMORY_;
    }
    s.mem = data->bytes, s.size = data->size;
    if (describe(&s, index, d) != 0) {
        free(d);
        fdata_unref(data);
        return DWRITE_E_FILEFORMAT;
    }
    fdata_unref(data);
    d->refs = 1;
    d->file = f;
    obj_AddRef(f);
    face = get_face(d, sims);
    fdesc_unref(d);                           /* the face holds it (or an equal face was found) */
    *out = face;
    return face ? S_OK_ : E_OUTOFMEMORY_;
}
static HRESULT WINAPI factory_CreateRenderingParams(void *self, void **out) {
    (void)self;
    return new_params(1.8f, 0.5f, 1.0f, 0.0f, 0, RM_DEFAULT, 0, out);
}
static HRESULT WINAPI factory_CreateMonitorRenderingParams(void *self, HMONITOR mon, void **out) {
    (void)mon;
    return factory_CreateRenderingParams(self, out);
}
static HRESULT WINAPI factory_CreateCustomRenderingParams(void *self, FLOAT gamma, FLOAT contrast, FLOAT cleartype,
                                                          UINT32 geometry, UINT32 mode, void **out) {
    (void)self;
    return new_params(gamma, contrast, 1.0f, cleartype, geometry, mode, 0, out);
}
static HRESULT WINAPI factory_RegisterFontFileLoader(void *self, void *l) {
    (void)self;
    if (l == &local_loader) return DWRITE_E_ALREADYREGISTERED;
    return loader_add(file_loaders, &nfile_loaders, l);
}
static HRESULT WINAPI factory_UnregisterFontFileLoader(void *self, void *l) {
    (void)self;
    return loader_remove(file_loaders, &nfile_loaders, l);
}
static HRESULT WINAPI factory_CreateTextFormat(void *self, const WCHAR *family, void *coll, UINT32 weight,
                                               UINT32 style, UINT32 stretch, FLOAT size, const WCHAR *locale,
                                               void **out) {
    (void)self, (void)family, (void)coll, (void)weight, (void)style, (void)stretch, (void)size, (void)locale;
    return notimpl("IDWriteFactory::CreateTextFormat", out);
}
static HRESULT WINAPI factory_CreateTypography(void *self, void **out) {
    (void)self;
    return notimpl("IDWriteFactory::CreateTypography", out);
}
static HRESULT WINAPI factory_GetGdiInterop(void *self, void **out) {
    (void)self;
    *out = &the_gdi;
    return S_OK_;
}
static HRESULT WINAPI factory_CreateTextLayout(void *self, const WCHAR *s, UINT32 n, void *fmt, FLOAT w, FLOAT h,
                                               void **out) {
    (void)self, (void)s, (void)n, (void)fmt, (void)w, (void)h;
    return notimpl("IDWriteFactory::CreateTextLayout", out);
}
static HRESULT WINAPI factory_CreateGdiCompatibleTextLayout(void *self, const WCHAR *s, UINT32 n, void *fmt, FLOAT w,
                                                            FLOAT h, FLOAT ppdip, const DWRITE_MATRIX *m,
                                                            BOOL natural, void **out) {
    (void)self, (void)s, (void)n, (void)fmt, (void)w, (void)h, (void)ppdip, (void)m, (void)natural;
    return notimpl("IDWriteFactory::CreateGdiCompatibleTextLayout", out);
}
static HRESULT WINAPI factory_CreateEllipsisTrimmingSign(void *self, void *fmt, void **out) {
    (void)self, (void)fmt;
    return notimpl("IDWriteFactory::CreateEllipsisTrimmingSign", out);
}
static HRESULT WINAPI factory_CreateTextAnalyzer(void *self, void **out) {
    (void)self;
    return notimpl("IDWriteFactory::CreateTextAnalyzer", out);
}
static HRESULT WINAPI factory_CreateNumberSubstitution(void *self, UINT32 method, const WCHAR *locale, BOOL ignore,
                                                       void **out) {
    obj_t *o = calloc(1, sizeof(*o));
    (void)self, (void)method, (void)locale, (void)ignore;
    *out = o;
    if (!o) return E_OUTOFMEMORY_;
    o->vt = numsub_vtbl;
    o->refs = 1;
    return S_OK_;
}
static HRESULT WINAPI factory_CreateGlyphRunAnalysis(void *self, const DWRITE_GLYPH_RUN *run, FLOAT ppdip,
                                                     const DWRITE_MATRIX *m, UINT32 mode, UINT32 measuring, FLOAT x,
                                                     FLOAT y, void **out) {
    (void)self, (void)measuring;
    return make_analysis(run, ppdip, m, mode, AA_CLEARTYPE, x, y, out);
}
static HRESULT WINAPI factory_GetEudcFontCollection(void *self, void **out, BOOL check) {
    (void)self, (void)check;
    lock();
    if (!empty_coll) empty_coll = new_coll();
    unlock();
    *out = empty_coll;
    if (!empty_coll) return E_OUTOFMEMORY_;
    obj_AddRef(empty_coll);
    return S_OK_;
}
static HRESULT WINAPI factory_CreateCustomRenderingParams1(void *self, FLOAT gamma, FLOAT contrast, FLOAT gray,
                                                           FLOAT cleartype, UINT32 geometry, UINT32 mode, void **out) {
    (void)self;
    return new_params(gamma, contrast, gray, cleartype, geometry, mode, 0, out);
}
static HRESULT WINAPI factory_GetSystemFontFallback(void *self, void **out) {
    (void)self;
    *out = &the_fallback;
    return S_OK_;
}
static HRESULT WINAPI factory_CreateFontFallbackBuilder(void *self, void **out) {
    (void)self;
    return notimpl("IDWriteFactory2::CreateFontFallbackBuilder", out);
}
static HRESULT WINAPI factory_TranslateColorGlyphRun(void *self, FLOAT x, FLOAT y, const DWRITE_GLYPH_RUN *run,
                                                     const void *desc, UINT32 measuring, const DWRITE_MATRIX *m,
                                                     UINT32 palette, void **out) {
    (void)self, (void)x, (void)y, (void)run, (void)desc, (void)measuring, (void)m, (void)palette;
    *out = 0;
    return DWRITE_E_NOCOLOR;
}
static HRESULT WINAPI factory_CreateCustomRenderingParams2(void *self, FLOAT gamma, FLOAT contrast, FLOAT gray,
                                                           FLOAT cleartype, UINT32 geometry, UINT32 mode,
                                                           UINT32 gridfit, void **out) {
    (void)self;
    return new_params(gamma, contrast, gray, cleartype, geometry, mode, gridfit, out);
}
static HRESULT WINAPI factory_CreateGlyphRunAnalysis2(void *self, const DWRITE_GLYPH_RUN *run, const DWRITE_MATRIX *m,
                                                      UINT32 mode, UINT32 measuring, UINT32 gridfit, UINT32 antialias,
                                                      FLOAT x, FLOAT y, void **out) {
    (void)self, (void)measuring, (void)gridfit;
    return make_analysis(run, 1.0f, m, mode, antialias, x, y, out);
}
static void *const factory_vtbl[31] = {
    factory_QueryInterface, static_AddRef, static_Release,
    factory_GetSystemFontCollection, factory_CreateCustomFontCollection, factory_RegisterFontCollectionLoader,
    factory_UnregisterFontCollectionLoader, factory_CreateFontFileReference, factory_CreateCustomFontFileReference,
    factory_CreateFontFace, factory_CreateRenderingParams, factory_CreateMonitorRenderingParams,
    factory_CreateCustomRenderingParams, factory_RegisterFontFileLoader, factory_UnregisterFontFileLoader,
    factory_CreateTextFormat, factory_CreateTypography, factory_GetGdiInterop, factory_CreateTextLayout,
    factory_CreateGdiCompatibleTextLayout, factory_CreateEllipsisTrimmingSign, factory_CreateTextAnalyzer,
    factory_CreateNumberSubstitution, factory_CreateGlyphRunAnalysis,
    /* IDWriteFactory1 */
    factory_GetEudcFontCollection, factory_CreateCustomRenderingParams1,
    /* IDWriteFactory2 */
    factory_GetSystemFontFallback, factory_CreateFontFallbackBuilder, factory_TranslateColorGlyphRun,
    factory_CreateCustomRenderingParams2, factory_CreateGlyphRunAnalysis2,
};
_Static_assert(sizeof(factory_vtbl) / sizeof(factory_vtbl[0]) == 31, "IDWriteFactory2 has 31 methods");

static HRESULT WINAPI DWriteCreateFactory(UINT32 type, const GUID *iid, void **out) {
    (void)type;
    if (!out) return E_POINTER_;
    return factory_QueryInterface(&the_factory, iid, out);
}

static const wexport_t dwrite_exports[] = {
    WEXP(DWriteCreateFactory),
};

const wdll_t dll_dwrite = { "dwrite.dll", dwrite_exports, sizeof(dwrite_exports) / sizeof(dwrite_exports[0]), 0, 0, 0, 0 };
