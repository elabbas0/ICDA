/* gdi32: device contexts, bitmaps and DIB sections, copying pixels to a
 * window, pixel formats for OpenGL, and enough of fonts and drawing for
 * what games do with GDI (a splash, a software frame, a message box).
 *
 * A window's DC draws into the window's surface (for the main window, the
 * ICDA window itself); a memory DC draws into its selected bitmap.  Pixels
 * are 32-bit BGRA everywhere; bitmaps of other depths are converted when
 * they are copied. */
#define _GNU_SOURCE
#include "wingui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_MAGIC  0x44430001
#define OBJ_MAGIC 0x47440001

enum { OBJ_PEN = 1, OBJ_BRUSH = 2, OBJ_DC = 3, OBJ_PAL = 5, OBJ_FONT = 6, OBJ_BITMAP = 7, OBJ_REGION = 8, OBJ_MEMDC = 10 };

typedef struct {
    int      magic;
    int      type;
    uint32_t color;           /* brushes and pens: 0x00BBGGRR (COLORREF) */
    int      w, h, bpp;       /* bitmaps */
    int      stride;          /* bytes a row */
    int      top_down;
    uint8_t *bits;
    int      own_bits;
    uint32_t palette[256];
    RECT     rgn;             /* regions: their bounds */
    int      font_h;
    int      stock;
} gobj_t;

static gobj_t *obj_get(void *h, int type) {
    gobj_t *o = h;
    if (!o || (uintptr_t)o < 0x10000 || o->magic != OBJ_MAGIC) return 0;
    if (type && o->type != type) return 0;
    return o;
}

static gobj_t *obj_new(int type) {
    gobj_t *o = calloc(1, sizeof(*o));
    o->magic = OBJ_MAGIC;
    o->type = type;
    return o;
}

static gobj_t stock[20];

__attribute__((constructor)) static void stock_init(void) {
    static const uint32_t colors[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000, 0 };
    for (int i = 0; i < 20; i++) {
        stock[i].magic = OBJ_MAGIC;
        stock[i].stock = 1;
        stock[i].type = i <= 5 ? OBJ_BRUSH : i <= 8 ? OBJ_PEN : i <= 17 ? OBJ_FONT : OBJ_PAL;
        stock[i].color = i <= 5 ? colors[i] : i == 6 ? 0xFFFFFF : 0;
        stock[i].font_h = 16;
    }
}

void *WINAPI GetStockObject(int i) { return i >= 0 && i < 20 ? &stock[i] : 0; }

/* ---- device contexts ---------------------------------------------------------------------- */

static wdc_t screen_dc;

wdc_t *dc_get(HDC h) {
    wdc_t *d = h;
    if (!d || (uintptr_t)d < 0x10000 || d->magic != DC_MAGIC) return 0;
    return d;
}

static wdc_t *dc_new(HWND hwnd) {
    wdc_t *d = calloc(1, sizeof(*d));
    d->magic = DC_MAGIC;
    d->hwnd = hwnd;
    d->text_color = 0;
    d->bk_color = 0xFFFFFF;
    d->bk_mode = 2;          /* OPAQUE */
    d->brush = &stock[0];
    d->pen = &stock[7];
    d->font = &stock[13];
    return d;
}

HDC dc_for_window(wwnd_t *w) {
    if (!w->dc) w->dc = dc_new(w->h);
    return w->dc;
}

wwnd_t *dc_window(HDC h) {
    wdc_t *d = dc_get(h);
    return d ? wnd_get(d->hwnd) : 0;
}

HDC WINAPI GetDC(HWND h) {
    wwnd_t *w;
    if (!h) {
        if (!screen_dc.magic) {
            wdc_t *d = dc_new(0);
            screen_dc = *d;
            free(d);
            screen_dc.is_display = 1;
        }
        return &screen_dc;
    }
    w = wnd_get(h);
    if (!w) return 0;
    return dc_for_window(w);
}

HDC WINAPI GetDCEx(HWND h, HRGN r, DWORD f) {
    (void)r, (void)f;
    return GetDC(h);
}
HDC WINAPI GetWindowDC(HWND h) { return GetDC(h); }
int WINAPI ReleaseDC(HWND h, HDC dc) {
    (void)h, (void)dc;
    return 1;
}
HWND WINAPI WindowFromDC(HDC dc) {
    wdc_t *d = dc_get(dc);
    return d ? d->hwnd : 0;
}

HDC WINAPI CreateCompatibleDC(HDC dc) {
    wdc_t *d = dc_new(0);
    (void)dc;
    d->bitmap = 0;
    return d;
}

HDC WINAPI CreateDCW(const WCHAR *drv, const WCHAR *dev, const WCHAR *out, const void *dm) {
    wdc_t *d = dc_new(0);
    (void)drv, (void)dev, (void)out, (void)dm;
    d->is_display = 1;
    return d;
}

HDC WINAPI CreateDCA(const char *drv, const char *dev, const char *out, const void *dm) {
    (void)drv, (void)dev, (void)out;
    return CreateDCW(0, 0, 0, dm);
}

BOOL WINAPI DeleteDC(HDC dc) {
    wdc_t *d = dc_get(dc);
    if (!d || d == &screen_dc) return FALSE;
    /* a window's own DC stays */
    if (d->hwnd && wnd_get(d->hwnd) && wnd_get(d->hwnd)->dc == d) return TRUE;
    d->magic = 0;
    free(d);
    return TRUE;
}

int WINAPI SaveDC(HDC dc) {
    (void)dc;
    return 1;
}
BOOL WINAPI RestoreDC(HDC dc, int n) {
    (void)dc, (void)n;
    return TRUE;
}

/* what a DC draws into: its pixels, size and stride (in pixels); 0 if
 * nothing 32-bit is there */
static uint32_t *dc_target(wdc_t *d, int *w, int *h, int *stride, wwnd_t **win) {
    *win = 0;
    if (!d) return 0;
    if (d->hwnd) {
        wwnd_t *wn = wnd_get(d->hwnd);
        if (!wn) return 0;
        wnd_surface(wn);
        *w = wn->sw, *h = wn->sh, *stride = wn->sw;
        *win = wn;
        return wn->surface;
    }
    if (d->bitmap) {
        gobj_t *b = d->bitmap;
        if (b->bpp != 32 || !b->top_down) return 0;
        *w = b->w, *h = b->h, *stride = b->stride / 4;
        return (uint32_t *)b->bits;
    }
    return 0;
}

static uint32_t bgra_of(uint32_t colorref) {
    return 0xFF000000u | ((colorref & 0xFF) << 16) | (colorref & 0xFF00) | ((colorref >> 16) & 0xFF);
}

/* ---- objects ---------------------------------------------------------------------------------- */

void *WINAPI SelectObject(HDC dc, void *h) {
    wdc_t *d = dc_get(dc);
    gobj_t *o = obj_get(h, 0);
    void *old = 0;
    if (!d || !o) return 0;
    switch (o->type) {
    case OBJ_BITMAP: old = d->bitmap; d->bitmap = o; break;
    case OBJ_BRUSH: old = d->brush; d->brush = o; break;
    case OBJ_PEN: old = d->pen; d->pen = o; break;
    case OBJ_FONT: old = d->font; d->font = o; break;
    case OBJ_REGION: return (void *)1;     /* SIMPLEREGION */
    default: break;
    }
    return old ? old : &stock[0];
}

BOOL WINAPI DeleteObject(void *h) {
    gobj_t *o = obj_get(h, 0);
    if (!o) return FALSE;
    if (o->stock) return TRUE;
    if (o->own_bits) free(o->bits);
    o->magic = 0;
    free(o);
    return TRUE;
}

DWORD WINAPI GetObjectType(void *h) {
    gobj_t *o = obj_get(h, 0);
    wdc_t *d = dc_get(h);
    if (d) return d->hwnd || d->is_display ? OBJ_DC : OBJ_MEMDC;
    return o ? (DWORD)o->type : 0;
}

void *WINAPI GetCurrentObject(HDC dc, UINT type) {
    wdc_t *d = dc_get(dc);
    if (!d) return 0;
    switch (type) {
    case OBJ_BITMAP: return d->bitmap;
    case OBJ_BRUSH: return d->brush;
    case OBJ_PEN: return d->pen;
    case OBJ_FONT: return d->font;
    default: return 0;
    }
}

typedef struct {
    LONG  bmType, bmWidth, bmHeight, bmWidthBytes;
    WORD  bmPlanes, bmBitsPixel;
    void *bmBits;
} BITMAP;

int WINAPI GetObjectW(void *h, int n, void *out) {
    gobj_t *o = obj_get(h, 0);
    if (!o) return 0;
    if (o->type == OBJ_BITMAP) {
        BITMAP b;
        if (!out) return sizeof(b);
        memset(&b, 0, sizeof(b));
        b.bmWidth = o->w;
        b.bmHeight = o->h;
        b.bmWidthBytes = o->stride;
        b.bmPlanes = 1;
        b.bmBitsPixel = (WORD)o->bpp;
        b.bmBits = o->bits;
        memcpy(out, &b, (size_t)(n < (int)sizeof(b) ? n : (int)sizeof(b)));
        return n < (int)sizeof(b) ? n : (int)sizeof(b);
    }
    if (o->type == OBJ_BRUSH && out && n >= 4) {
        memset(out, 0, (size_t)n);
        ((uint32_t *)out)[1] = o->color;
        return n < 16 ? n : 16;
    }
    if (out && n > 0) memset(out, 0, (size_t)n);
    return n;
}

int WINAPI GetObjectA(void *h, int n, void *out) { return GetObjectW(h, n, out); }

void *WINAPI CreateSolidBrush(uint32_t c) {
    gobj_t *o = obj_new(OBJ_BRUSH);
    o->color = c;
    return o;
}
void *WINAPI CreateBrushIndirect(const uint32_t *lb) { return CreateSolidBrush(lb[1]); }
void *WINAPI CreatePatternBrush(void *bm) {
    (void)bm;
    return CreateSolidBrush(0);
}
void *WINAPI CreateHatchBrush(int s, uint32_t c) {
    (void)s;
    return CreateSolidBrush(c);
}
void *WINAPI CreatePen(int style, int w, uint32_t c) {
    gobj_t *o = obj_new(OBJ_PEN);
    (void)style, (void)w;
    o->color = c;
    return o;
}
void *WINAPI CreatePalette(const void *lp) {
    (void)lp;
    return obj_new(OBJ_PAL);
}
void *WINAPI SelectPalette(HDC dc, void *p, BOOL bg) {
    (void)dc, (void)bg;
    return p;
}
UINT WINAPI RealizePalette(HDC dc) {
    (void)dc;
    return 0;
}
UINT WINAPI GetSystemPaletteEntries(HDC dc, UINT s, UINT n, void *e) {
    (void)dc, (void)s, (void)n, (void)e;
    return 0;
}

/* ---- regions ---------------------------------------------------------------------------------- */

void *WINAPI CreateRectRgn(int l, int t, int r, int b) {
    gobj_t *o = obj_new(OBJ_REGION);
    o->rgn.left = l, o->rgn.top = t, o->rgn.right = r, o->rgn.bottom = b;
    return o;
}
void *WINAPI CreateRectRgnIndirect(const RECT *r) { return CreateRectRgn(r->left, r->top, r->right, r->bottom); }
int WINAPI CombineRgn(void *d, void *a, void *b, int mode) {
    gobj_t *od = obj_get(d, OBJ_REGION), *oa = obj_get(a, OBJ_REGION), *ob = obj_get(b, OBJ_REGION);
    (void)mode;
    if (!od || !oa) return 0;
    od->rgn = oa->rgn;
    if (ob && mode == 2) {      /* RGN_OR: the bounds of both */
        if (ob->rgn.left < od->rgn.left) od->rgn.left = ob->rgn.left;
        if (ob->rgn.top < od->rgn.top) od->rgn.top = ob->rgn.top;
        if (ob->rgn.right > od->rgn.right) od->rgn.right = ob->rgn.right;
        if (ob->rgn.bottom > od->rgn.bottom) od->rgn.bottom = ob->rgn.bottom;
    }
    return 2;                   /* SIMPLEREGION */
}
int WINAPI GetRgnBox(void *r, RECT *out) {
    gobj_t *o = obj_get(r, OBJ_REGION);
    if (!o) return 0;
    *out = o->rgn;
    return 2;
}
BOOL WINAPI SetRectRgn(void *r, int l, int t, int ri, int b) {
    gobj_t *o = obj_get(r, OBJ_REGION);
    if (!o) return FALSE;
    o->rgn.left = l, o->rgn.top = t, o->rgn.right = ri, o->rgn.bottom = b;
    return TRUE;
}
int WINAPI SelectClipRgn(HDC dc, void *r) {
    (void)dc, (void)r;
    return 2;
}
int WINAPI GetClipBox(HDC dc, RECT *r) {
    wdc_t *d = dc_get(dc);
    int w = 0, h = 0, s;
    wwnd_t *win;
    r->left = r->top = 0;
    if (dc_target(d, &w, &h, &s, &win)) r->right = w, r->bottom = h;
    else r->right = r->bottom = 1;
    return 2;
}
int WINAPI IntersectClipRect(HDC dc, int l, int t, int r, int b) {
    (void)dc, (void)l, (void)t, (void)r, (void)b;
    return 2;
}

/* ---- bitmaps ---------------------------------------------------------------------------------- */

typedef struct {
    DWORD biSize;
    LONG  biWidth, biHeight;
    WORD  biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG  biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

static gobj_t *bitmap_new(int w, int h, int bpp, int top_down, void *bits) {
    gobj_t *o = obj_new(OBJ_BITMAP);
    o->w = w < 1 ? 1 : w;
    o->h = h < 1 ? 1 : h;
    o->bpp = bpp;
    o->stride = ((o->w * bpp + 31) / 32) * 4;
    o->top_down = top_down;
    if (bits) {
        o->bits = bits;
    } else {
        o->bits = calloc((size_t)o->stride, (size_t)o->h);
        o->own_bits = 1;
    }
    return o;
}

void *WINAPI CreateDIBSection(HDC dc, const void *bmi, UINT usage, void **bits, HANDLE section, DWORD off) {
    const BITMAPINFOHEADER *h = bmi;
    gobj_t *o;
    (void)dc, (void)usage, (void)section, (void)off;
    o = bitmap_new(h->biWidth, h->biHeight < 0 ? -h->biHeight : h->biHeight, h->biBitCount ? h->biBitCount : 32, h->biHeight < 0, 0);
    if (h->biBitCount <= 8) {
        const uint32_t *pal = (const uint32_t *)((const uint8_t *)bmi + h->biSize);
        int n = h->biClrUsed ? (int)h->biClrUsed : 1 << h->biBitCount;
        for (int i = 0; i < n && i < 256; i++) o->palette[i] = pal[i];
    }
    if (bits) *bits = o->bits;
    return o;
}

void *WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) {
    gobj_t *o = bitmap_new(w, h, (int)(bpp ? bpp : 1), 1, 0);
    (void)planes;
    if (bits) {
        /* CreateBitmap rows are 16-bit aligned */
        int src_stride = ((w * (int)bpp + 15) / 16) * 2;
        for (int y = 0; y < o->h; y++)
            memcpy(o->bits + (size_t)y * (size_t)o->stride, (const uint8_t *)bits + (size_t)y * (size_t)src_stride,
                   (size_t)(src_stride < o->stride ? src_stride : o->stride));
    }
    return o;
}

void *WINAPI CreateCompatibleBitmap(HDC dc, int w, int h) {
    (void)dc;
    return bitmap_new(w, h, 32, 1, 0);
}

void *WINAPI CreateDIBitmap(HDC dc, const BITMAPINFOHEADER *h, DWORD init, const void *bits, const void *bmi, UINT usage) {
    gobj_t *o = bitmap_new(h->biWidth, h->biHeight < 0 ? -h->biHeight : h->biHeight, 32, 1, 0);
    (void)dc, (void)init, (void)bits, (void)bmi, (void)usage;
    return o;
}

/* one pixel of a bitmap row as BGRA */
static uint32_t px_at(const gobj_t *b, const uint8_t *row, int x) {
    switch (b->bpp) {
    case 32: return ((const uint32_t *)row)[x] | 0xFF000000u;
    case 24: return 0xFF000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
    case 16: {
        uint16_t v = ((const uint16_t *)row)[x];
        return 0xFF000000u | (uint32_t)((v >> 10) & 31) << 19 | (uint32_t)((v >> 5) & 31) << 11 | (uint32_t)(v & 31) << 3;
    }
    case 8: return b->palette[row[x]] | 0xFF000000u;
    case 4: return b->palette[(row[x / 2] >> ((1 - (x & 1)) * 4)) & 15] | 0xFF000000u;
    case 1: return (row[x / 8] >> (7 - (x & 7)) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    default: return 0xFF000000u;
    }
}

static const uint8_t *bm_row(const gobj_t *b, int y) {
    return b->bits + (size_t)(b->top_down ? y : b->h - 1 - y) * (size_t)b->stride;
}

/* copies pixels into a DC's target and shows them when it is the main window */
static void blit_into(wdc_t *dst, int dx, int dy, int w, int h, const gobj_t *src, int sx, int sy, int sw, int sh) {
    int tw, th, ts;
    wwnd_t *win;
    uint32_t *t = dc_target(dst, &tw, &th, &ts, &win);
    if (!t || !src || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        int ty = dy + y, syy = sy + (sh == h ? y : (int)((int64_t)y * sh / h));
        const uint8_t *row;
        if (ty < 0 || ty >= th || syy < 0 || syy >= src->h) continue;
        row = bm_row(src, syy);
        for (int x = 0; x < w; x++) {
            int tx = dx + x, sxx = sx + (sw == w ? x : (int)((int64_t)x * sw / w));
            if (tx < 0 || tx >= tw || sxx < 0 || sxx >= src->w) continue;
            t[(size_t)ty * (size_t)ts + (size_t)tx] = px_at(src, row, sxx);
        }
    }
    if (win) wnd_present(win);
}

BOOL WINAPI BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    wdc_t *d = dc_get(dst), *s = dc_get(src);
    (void)rop;
    if (!d) return FALSE;
    if (s && s->bitmap) blit_into(d, x, y, w, h, s->bitmap, sx, sy, w, h);
    return TRUE;
}

BOOL WINAPI StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    wdc_t *d = dc_get(dst), *s = dc_get(src);
    (void)rop;
    if (!d) return FALSE;
    if (s && s->bitmap) blit_into(d, x, y, w, h, s->bitmap, sx, sy, sw, sh);
    return TRUE;
}

BOOL WINAPI PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop) {
    wdc_t *d = dc_get(dc);
    int tw, th, ts;
    wwnd_t *win;
    uint32_t *t = dc_target(d, &tw, &th, &ts, &win), c;
    if (!t) return TRUE;
    c = rop == 0x42 ? 0xFF000000u : rop == 0xFF0062 ? 0xFFFFFFFFu : bgra_of(((gobj_t *)d->brush)->color);
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < th; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < tw; xx++) t[(size_t)yy * (size_t)ts + (size_t)xx] = c;
    if (win) wnd_present(win);
    return TRUE;
}

int WINAPI StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits, const void *bmi,
                         UINT usage, DWORD rop) {
    const BITMAPINFOHEADER *bh = bmi;
    gobj_t tmp;
    (void)usage, (void)rop;
    memset(&tmp, 0, sizeof(tmp));
    tmp.w = bh->biWidth;
    tmp.h = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    tmp.bpp = bh->biBitCount;
    tmp.stride = ((tmp.w * tmp.bpp + 31) / 32) * 4;
    tmp.top_down = bh->biHeight < 0;
    tmp.bits = (uint8_t *)bits;
    if (bh->biBitCount <= 8) {
        const uint32_t *pal = (const uint32_t *)((const uint8_t *)bmi + bh->biSize);
        int n = bh->biClrUsed ? (int)bh->biClrUsed : 1 << bh->biBitCount;
        for (int i = 0; i < n && i < 256; i++) tmp.palette[i] = pal[i];
    }
    /* source rows count from the bottom for bottom-up DIBs: sy is from the bottom */
    if (!tmp.top_down) sy = tmp.h - sy - sh;
    blit_into(dc_get(dc), x, y, w, h, &tmp, sx, sy, sw, sh);
    return h;
}

int WINAPI SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines, const void *bits,
                             const void *bmi, UINT usage) {
    (void)start, (void)lines;
    return StretchDIBits(dc, x, y, (int)w, (int)h, sx, sy, (int)w, (int)h, bits, bmi, usage, 0xCC0020);
}

int WINAPI GetDIBits(HDC dc, void *bm, UINT start, UINT n, void *bits, void *bmi, UINT usage) {
    gobj_t *b = obj_get(bm, OBJ_BITMAP);
    BITMAPINFOHEADER *h = bmi;
    (void)dc, (void)usage;
    if (!b) return 0;
    if (!bits) {
        h->biWidth = b->w;
        h->biHeight = b->h;
        h->biPlanes = 1;
        h->biBitCount = (WORD)b->bpp;
        h->biCompression = 0;
        h->biSizeImage = (DWORD)(b->stride * b->h);
        return 1;
    }
    {
        /* as 32-bit rows, in the direction asked */
        int top_down = h->biHeight < 0, rows = 0;
        for (UINT i = 0; i < n && (int)(start + i) < b->h; i++) {
            int y = (int)(start + i);
            const uint8_t *row = bm_row(b, top_down ? y : b->h - 1 - y);
            uint32_t *out = (uint32_t *)bits + (size_t)i * (size_t)b->w;
            for (int x = 0; x < b->w; x++) out[x] = px_at(b, row, x);
            rows++;
        }
        return rows;
    }
}

int WINAPI SetDIBits(HDC dc, void *bm, UINT start, UINT n, const void *bits, const void *bmi, UINT usage) {
    gobj_t *b = obj_get(bm, OBJ_BITMAP);
    const BITMAPINFOHEADER *h = bmi;
    (void)dc, (void)usage;
    if (!b || b->bpp != 32 || h->biBitCount != 32) return 0;
    for (UINT i = 0; i < n && (int)(start + i) < b->h; i++)
        memcpy(b->bits + (size_t)(start + i) * (size_t)b->stride, (const uint8_t *)bits + (size_t)i * (size_t)b->w * 4, (size_t)b->w * 4);
    return (int)n;
}

uint32_t WINAPI GetPixel(HDC dc, int x, int y) {
    wdc_t *d = dc_get(dc);
    int w, h, s;
    wwnd_t *win;
    uint32_t *t = dc_target(d, &w, &h, &s, &win), p;
    if (!t || x < 0 || y < 0 || x >= w || y >= h) return 0xFFFFFFFF;
    p = t[(size_t)y * (size_t)s + (size_t)x];
    return ((p >> 16) & 0xFF) | (p & 0xFF00) | ((p & 0xFF) << 16);
}

uint32_t WINAPI SetPixel(HDC dc, int x, int y, uint32_t c) {
    wdc_t *d = dc_get(dc);
    int w, h, s;
    wwnd_t *win;
    uint32_t *t = dc_target(d, &w, &h, &s, &win);
    if (t && x >= 0 && y >= 0 && x < w && y < h) t[(size_t)y * (size_t)s + (size_t)x] = bgra_of(c);
    return c;
}

/* ---- drawing ------------------------------------------------------------------------------------ */

static void fill(wdc_t *d, const RECT *r, uint32_t c) {
    int w, h, s;
    wwnd_t *win;
    uint32_t *t = dc_target(d, &w, &h, &s, &win);
    if (!t) return;
    for (int y = r->top < 0 ? 0 : r->top; y < r->bottom && y < h; y++)
        for (int x = r->left < 0 ? 0 : r->left; x < r->right && x < w; x++) t[(size_t)y * (size_t)s + (size_t)x] = c;
}

int WINAPI FillRect(HDC dc, const RECT *r, HBRUSH b) {
    gobj_t *o = obj_get(b, OBJ_BRUSH);
    uint32_t c;
    extern DWORD WINAPI GetSysColor(int i);
    if (o) c = o->color;
    else if ((uintptr_t)b >= 0xB0000 && (uintptr_t)b < 0xB0100) c = GetSysColor((int)((uintptr_t)b & 0xFF));
    else if ((uintptr_t)b > 0 && (uintptr_t)b <= 31) c = GetSysColor((int)(uintptr_t)b - 1);   /* COLOR_x + 1 */
    else c = 0xFFFFFF;
    fill(dc_get(dc), r, bgra_of(c));
    return 1;
}

int WINAPI FrameRect(HDC dc, const RECT *r, HBRUSH b) {
    RECT e;
    e = *r, e.bottom = e.top + 1;
    FillRect(dc, &e, b);
    e = *r, e.top = e.bottom - 1;
    FillRect(dc, &e, b);
    e = *r, e.right = e.left + 1;
    FillRect(dc, &e, b);
    e = *r, e.left = e.right - 1;
    FillRect(dc, &e, b);
    return 1;
}

BOOL WINAPI Rectangle(HDC dc, int l, int t, int r, int b) {
    wdc_t *d = dc_get(dc);
    RECT rc = { l, t, r, b };
    if (!d) return FALSE;
    fill(d, &rc, bgra_of(((gobj_t *)d->brush)->color));
    return TRUE;
}

BOOL WINAPI DrawEdge(HDC dc, RECT *r, UINT e, UINT f) {
    (void)dc, (void)r, (void)e, (void)f;
    return TRUE;
}
BOOL WINAPI DrawFocusRect(HDC dc, const RECT *r) {
    (void)dc, (void)r;
    return TRUE;
}
BOOL WINAPI InvertRect(HDC dc, const RECT *r) {
    (void)dc, (void)r;
    return TRUE;
}
BOOL WINAPI MoveToEx(HDC dc, int x, int y, POINT *old) {
    (void)dc, (void)x, (void)y;
    if (old) old->x = old->y = 0;
    return TRUE;
}
BOOL WINAPI LineTo(HDC dc, int x, int y) {
    (void)dc, (void)x, (void)y;
    return TRUE;
}
BOOL WINAPI Ellipse(HDC dc, int l, int t, int r, int b) { return Rectangle(dc, l, t, r, b); }
BOOL WINAPI Polygon(HDC dc, const POINT *p, int n) {
    (void)dc, (void)p, (void)n;
    return TRUE;
}
BOOL WINAPI Polyline(HDC dc, const POINT *p, int n) {
    (void)dc, (void)p, (void)n;
    return TRUE;
}
BOOL WINAPI RoundRect(HDC dc, int l, int t, int r, int b, int w, int h) {
    (void)w, (void)h;
    return Rectangle(dc, l, t, r, b);
}
BOOL WINAPI GdiFlush(void) { return TRUE; }

/* ---- text: an 8x16 cell per character, no glyphs drawn ----------------------------------------- */

void *WINAPI CreateFontIndirectW(const void *lf) {
    gobj_t *o = obj_new(OBJ_FONT);
    LONG h = lf ? *(const LONG *)lf : 16;
    o->font_h = h < 0 ? -h : h ? h : 16;
    return o;
}
void *WINAPI CreateFontIndirectA(const void *lf) { return CreateFontIndirectW(lf); }
void *WINAPI CreateFontW(int h, int w, int e, int o, int wt, DWORD it, DWORD u, DWORD s, DWORD cs, DWORD op, DWORD cp, DWORD q,
                         DWORD pf, const WCHAR *face) {
    LONG lf[1] = { h };
    (void)w, (void)e, (void)o, (void)wt, (void)it, (void)u, (void)s, (void)cs, (void)op, (void)cp, (void)q, (void)pf, (void)face;
    return CreateFontIndirectW(lf);
}
void *WINAPI CreateFontA(int h, int w, int e, int o, int wt, DWORD it, DWORD u, DWORD s, DWORD cs, DWORD op, DWORD cp, DWORD q,
                         DWORD pf, const char *face) {
    (void)face;
    return CreateFontW(h, w, e, o, wt, it, u, s, cs, op, cp, q, pf, 0);
}

static int font_h(HDC dc) {
    wdc_t *d = dc_get(dc);
    gobj_t *f = d ? obj_get(d->font, OBJ_FONT) : 0;
    return f && f->font_h > 0 ? f->font_h : 16;
}

BOOL WINAPI GetTextMetricsW(HDC dc, void *tm) {
    LONG *t = tm;
    int h = font_h(dc);
    memset(tm, 0, 60);
    t[0] = h;                 /* tmHeight */
    t[1] = h * 4 / 5;         /* tmAscent */
    t[2] = h - h * 4 / 5;     /* tmDescent */
    t[5] = h / 2;             /* tmAveCharWidth */
    t[6] = h;                 /* tmMaxCharWidth */
    t[7] = 400;               /* tmWeight */
    ((BYTE *)tm)[56] = 0x31;  /* fixed pitch, TrueType */
    return TRUE;
}
BOOL WINAPI GetTextMetricsA(HDC dc, void *tm) { return GetTextMetricsW(dc, tm); }

BOOL WINAPI GetTextExtentPoint32W(HDC dc, const WCHAR *s, int n, SIZE *sz) {
    int h = font_h(dc);
    (void)s;
    sz->cx = n * (h / 2);
    sz->cy = h;
    return TRUE;
}
BOOL WINAPI GetTextExtentPoint32A(HDC dc, const char *s, int n, SIZE *sz) {
    (void)s;
    return GetTextExtentPoint32W(dc, 0, n, sz);
}
BOOL WINAPI GetTextExtentExPointW(HDC dc, const WCHAR *s, int n, int max, int *fit, int *dx, SIZE *sz) {
    int cw = font_h(dc) / 2;
    GetTextExtentPoint32W(dc, s, n, sz);
    if (fit) *fit = cw ? (max / cw < n ? max / cw : n) : n;
    if (dx)
        for (int i = 0; i < n; i++) dx[i] = (i + 1) * cw;
    return TRUE;
}
BOOL WINAPI ExtTextOutW(HDC dc, int x, int y, UINT o, const RECT *r, const WCHAR *s, UINT n, const int *dx) {
    (void)dc, (void)x, (void)y, (void)o, (void)r, (void)s, (void)n, (void)dx;
    return TRUE;
}
BOOL WINAPI ExtTextOutA(HDC dc, int x, int y, UINT o, const RECT *r, const char *s, UINT n, const int *dx) {
    (void)s;
    return ExtTextOutW(dc, x, y, o, r, 0, n, dx);
}
BOOL WINAPI TextOutW(HDC dc, int x, int y, const WCHAR *s, int n) {
    (void)dc, (void)x, (void)y, (void)s, (void)n;
    return TRUE;
}
BOOL WINAPI TextOutA(HDC dc, int x, int y, const char *s, int n) {
    (void)dc, (void)x, (void)y, (void)s, (void)n;
    return TRUE;
}
int WINAPI DrawTextW(HDC dc, const WCHAR *s, int n, RECT *r, UINT f) {
    int h = font_h(dc);
    if (n < 0) n = (int)w_len(s);
    if (f & 0x400) {          /* DT_CALCRECT */
        r->right = r->left + n * (h / 2);
        r->bottom = r->top + h;
    }
    return h;
}
int WINAPI DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT f) {
    int h = font_h(dc);
    if (n < 0) n = (int)strlen(s);
    if (f & 0x400) {
        r->right = r->left + n * (h / 2);
        r->bottom = r->top + h;
    }
    return h;
}
uint32_t WINAPI SetTextColor(HDC dc, uint32_t c) {
    wdc_t *d = dc_get(dc);
    uint32_t o;
    if (!d) return 0xFFFFFFFF;
    o = d->text_color;
    d->text_color = c;
    return o;
}
uint32_t WINAPI GetTextColor(HDC dc) {
    wdc_t *d = dc_get(dc);
    return d ? d->text_color : 0;
}
uint32_t WINAPI SetBkColor(HDC dc, uint32_t c) {
    wdc_t *d = dc_get(dc);
    uint32_t o;
    if (!d) return 0xFFFFFFFF;
    o = d->bk_color;
    d->bk_color = c;
    return o;
}
int WINAPI SetBkMode(HDC dc, int m) {
    wdc_t *d = dc_get(dc);
    int o;
    if (!d) return 0;
    o = d->bk_mode;
    d->bk_mode = m;
    return o;
}
UINT WINAPI SetTextAlign(HDC dc, UINT a) {
    (void)dc, (void)a;
    return 0;
}
int WINAPI SetMapMode(HDC dc, int m) {
    (void)dc, (void)m;
    return 1;
}
int WINAPI SetStretchBltMode(HDC dc, int m) {
    (void)dc, (void)m;
    return 3;
}
int WINAPI SetGraphicsMode(HDC dc, int m) {
    (void)dc, (void)m;
    return 1;
}
int WINAPI AddFontResourceExW(const WCHAR *n, DWORD f, void *r) {
    (void)n, (void)f, (void)r;
    return 1;
}
int WINAPI EnumFontFamiliesExW(HDC dc, void *lf, void *fn, LPARAM lp, DWORD f) {
    (void)dc, (void)lf, (void)fn, (void)lp, (void)f;
    return 1;
}
DWORD WINAPI GetFontData(HDC dc, DWORD t, DWORD o, void *b, DWORD n) {
    (void)dc, (void)t, (void)o, (void)b, (void)n;
    return 0xFFFFFFFF;    /* GDI_ERROR */
}

/* ---- the device ---------------------------------------------------------------------------------- */

int WINAPI GetDeviceCaps(HDC dc, int i) {
    int sw, sh;
    (void)dc;
    bk_screen(&sw, &sh);
    switch (i) {
    case 2: return 0x400;          /* DRIVERVERSION */
    case 4: return sw * 254 / 960; /* HORZSIZE, mm at 96 dpi */
    case 6: return sh * 254 / 960;
    case 8: case 118: return sw;   /* HORZRES, DESKTOPHORZRES */
    case 10: case 117: return sh;  /* VERTRES, DESKTOPVERTRES */
    case 12: return 32;            /* BITSPIXEL */
    case 14: return 1;             /* PLANES */
    case 24: return -1;            /* NUMCOLORS */
    case 38: return 0x7E99;        /* RASTERCAPS: a capable raster device */
    case 88: case 90: return 96;   /* LOGPIXELSX/Y */
    case 104: return 0;            /* SIZEPALETTE */
    case 108: return 24;           /* COLORRES */
    case 116: return 60;           /* VREFRESH */
    case 119: return 1;            /* BLTALIGNMENT */
    case 121: return 0x7;          /* SHADEBLENDCAPS */
    case 122: return 0;            /* COLORMGMTCAPS */
    default: return 0;
    }
}

BOOL WINAPI GetDeviceGammaRamp(HDC dc, void *ramp) {
    WORD *r = ramp;
    (void)dc;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) r[c * 256 + i] = (WORD)(i * 257);
    return TRUE;
}
BOOL WINAPI SetDeviceGammaRamp(HDC dc, void *ramp) {
    (void)dc, (void)ramp;
    return TRUE;
}
BOOL WINAPI GetICMProfileW(HDC dc, DWORD *n, WCHAR *name) {
    (void)dc, (void)n, (void)name;
    return FALSE;
}
BOOL WINAPI GetICMProfileA(HDC dc, DWORD *n, char *name) {
    (void)dc, (void)n, (void)name;
    return FALSE;
}

/* ---- pixel formats (for OpenGL) ------------------------------------------------------------- */

typedef struct {
    WORD  nSize, nVersion;
    DWORD dwFlags;
    BYTE  iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits,
        cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits,
        cStencilBits, cAuxBuffers, iLayerType, bReserved;
    DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR;

/* one format: 32-bit colour, 24-bit depth, 8-bit stencil, double-buffered */
int WINAPI DescribePixelFormat(HDC dc, int i, UINT n, PIXELFORMATDESCRIPTOR *p) {
    (void)dc;
    if (i < 1 || i > 1) return 1;
    if (p && n >= sizeof(*p)) {
        memset(p, 0, sizeof(*p));
        p->nSize = sizeof(*p);
        p->nVersion = 1;
        p->dwFlags = 0x4 | 0x20 | 0x1 | 0x8000;     /* DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLEBUFFER, SUPPORT_COMPOSITION */
        p->iPixelType = 0;                          /* RGBA */
        p->cColorBits = 32;
        p->cRedBits = 8, p->cRedShift = 16;
        p->cGreenBits = 8, p->cGreenShift = 8;
        p->cBlueBits = 8, p->cBlueShift = 0;
        p->cAlphaBits = 8, p->cAlphaShift = 24;
        p->cDepthBits = 24;
        p->cStencilBits = 8;
    }
    return 1;
}

int WINAPI ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *p) {
    (void)dc, (void)p;
    return 1;
}

BOOL WINAPI SetPixelFormat(HDC dc, int fmt, const PIXELFORMATDESCRIPTOR *p) {
    wdc_t *d = dc_get(dc);
    wwnd_t *w = dc_window(dc);
    (void)p;
    if (!d) return FALSE;
    if (w) {
        if (w->pixel_format) {      /* only once per window, as on Windows */
            wset_last_error(2000);  /* ERROR_INVALID_PIXEL_FORMAT */
            return w->pixel_format == fmt;
        }
        w->pixel_format = fmt;
    }
    d->pixel_format = fmt;
    return TRUE;
}

int WINAPI GetPixelFormat(HDC dc) {
    wdc_t *d = dc_get(dc);
    wwnd_t *w = dc_window(dc);
    if (w) return w->pixel_format;
    return d ? d->pixel_format : 0;
}

BOOL WINAPI SwapBuffers(HDC dc) { return gl_swap_buffers(dc); }

/* ---- the export table --------------------------------------------------------------------- */

#define X(f) WEXP(f)
static const wexport_t g32[] = {
    X(GetStockObject), X(CreateCompatibleDC), X(CreateDCW), X(CreateDCA), X(DeleteDC), X(SaveDC), X(RestoreDC),
    X(WindowFromDC), X(SelectObject), X(DeleteObject), X(GetObjectType), X(GetCurrentObject), X(GetObjectW),
    X(GetObjectA), X(CreateSolidBrush), X(CreateBrushIndirect), X(CreatePatternBrush), X(CreateHatchBrush),
    X(CreatePen), X(CreatePalette), X(SelectPalette), X(RealizePalette), X(GetSystemPaletteEntries), X(CreateRectRgn),
    X(CreateRectRgnIndirect), X(CombineRgn), X(GetRgnBox), X(SetRectRgn), X(SelectClipRgn), X(GetClipBox),
    X(IntersectClipRect), X(CreateDIBSection), X(CreateBitmap), X(CreateCompatibleBitmap), X(CreateDIBitmap),
    X(BitBlt), X(StretchBlt), X(PatBlt), X(StretchDIBits), X(SetDIBitsToDevice), X(GetDIBits), X(SetDIBits),
    X(GetPixel), X(SetPixel), X(Rectangle), X(MoveToEx), X(LineTo), X(Ellipse), X(Polygon), X(Polyline), X(RoundRect),
    X(GdiFlush), X(CreateFontIndirectW), X(CreateFontIndirectA), X(CreateFontW), X(CreateFontA), X(GetTextMetricsW),
    X(GetTextMetricsA), X(GetTextExtentPoint32W), X(GetTextExtentPoint32A), X(GetTextExtentExPointW), X(ExtTextOutW),
    X(ExtTextOutA), X(TextOutW), X(TextOutA), X(SetTextColor), X(GetTextColor), X(SetBkColor), X(SetBkMode),
    X(SetTextAlign), X(SetMapMode), X(SetStretchBltMode), X(SetGraphicsMode), X(AddFontResourceExW),
    X(EnumFontFamiliesExW), X(GetFontData), X(GetDeviceCaps), X(GetDeviceGammaRamp), X(SetDeviceGammaRamp),
    X(GetICMProfileW), X(GetICMProfileA), X(DescribePixelFormat), X(ChoosePixelFormat), X(SetPixelFormat),
    X(GetPixelFormat), X(SwapBuffers), WEXPN("GetTextExtentPointW", GetTextExtentPoint32W),
    WEXPN("GetTextExtentPointA", GetTextExtentPoint32A),
};

const wdll_t dll_gdi32 = { "gdi32.dll", g32, sizeof(g32) / sizeof(g32[0]) };
