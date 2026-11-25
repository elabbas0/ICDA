/* DirectWrite under icda-win: the system's fonts by Windows' family names,
 * a face's metrics, glyphs and tables, glyph outlines and coverage, font
 * fallback, and a font a program loads from memory through its own loaders
 * (as Chrome's Skia does with web fonts).
 *   t_dwrite.exe <a .ttf file>     (for the font loaded from memory)
 * Prints what it found; a line starting FAIL says what was wrong. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DWRITE_E_ALREADYREGISTERED
#define DWRITE_E_ALREADYREGISTERED ((HRESULT)0x88985006)
#endif

static int fails;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            printf("FAIL line %d: ", __LINE__);               \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
            fails++;                                          \
        }                                                     \
    } while (0)

static void family_of(IDWriteFont *font, char *out, int cap) {
    IDWriteFontFamily *fam = 0;
    IDWriteLocalizedStrings *names = 0;
    WCHAR w[128] = { 0 };
    out[0] = 0;
    if (FAILED(IDWriteFont_GetFontFamily(font, &fam))) return;
    if (SUCCEEDED(IDWriteFontFamily_GetFamilyNames(fam, &names))) {
        IDWriteLocalizedStrings_GetString(names, 0, w, 128);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, 0, 0);
        IDWriteLocalizedStrings_Release(names);
    }
    IDWriteFontFamily_Release(fam);
}

/* ---- a geometry sink that measures what it is given ---- */

typedef struct {
    ID2D1SimplifiedGeometrySink iface;
    int figures, segments, open, unbalanced;
    float minx, maxx, miny, maxy;
} sink_t;

static void grow(sink_t *s, D2D1_POINT_2F p) {
    if (p.x < s->minx) s->minx = p.x;
    if (p.x > s->maxx) s->maxx = p.x;
    if (p.y < s->miny) s->miny = p.y;
    if (p.y > s->maxy) s->maxy = p.y;
}
static HRESULT STDMETHODCALLTYPE sink_qi(IUnknown *u, REFIID iid, void **out) { (void)iid; *out = u; return S_OK; }
static ULONG STDMETHODCALLTYPE sink_ref(IUnknown *u) { (void)u; return 1; }
static void STDMETHODCALLTYPE sink_fill(ID2D1SimplifiedGeometrySink *t, D2D1_FILL_MODE m) { (void)t, (void)m; }
static void STDMETHODCALLTYPE sink_flags(ID2D1SimplifiedGeometrySink *t, D2D1_PATH_SEGMENT f) { (void)t, (void)f; }
static void STDMETHODCALLTYPE sink_begin(ID2D1SimplifiedGeometrySink *t, D2D1_POINT_2F p, D2D1_FIGURE_BEGIN b) {
    sink_t *s = (sink_t *)t;
    (void)b;
    if (s->open) s->unbalanced++;
    s->figures++, s->open = 1;
    grow(s, p);
}
static void STDMETHODCALLTYPE sink_lines(ID2D1SimplifiedGeometrySink *t, const D2D1_POINT_2F *p, UINT n) {
    sink_t *s = (sink_t *)t;
    for (UINT i = 0; i < n; i++) grow(s, p[i]);
    s->segments += (int)n;
}
static void STDMETHODCALLTYPE sink_beziers(ID2D1SimplifiedGeometrySink *t, const D2D1_BEZIER_SEGMENT *b, UINT n) {
    sink_t *s = (sink_t *)t;
    for (UINT i = 0; i < n; i++) grow(s, b[i].point3);
    s->segments += (int)n;
}
static void STDMETHODCALLTYPE sink_end(ID2D1SimplifiedGeometrySink *t, D2D1_FIGURE_END e) {
    sink_t *s = (sink_t *)t;
    (void)e;
    if (!s->open) s->unbalanced++;
    s->open = 0;
}
static HRESULT STDMETHODCALLTYPE sink_close(ID2D1SimplifiedGeometrySink *t) { (void)t; return S_OK; }
static ID2D1SimplifiedGeometrySinkVtbl sink_vtbl = {
    { sink_qi, sink_ref, sink_ref }, sink_fill, sink_flags, sink_begin, sink_lines, sink_beziers, sink_end, sink_close,
};

/* ---- text for font fallback ---- */

typedef struct {
    IDWriteTextAnalysisSource iface;
    const WCHAR *text;
    UINT32 len;
} text_t;

static HRESULT STDMETHODCALLTYPE src_qi(IDWriteTextAnalysisSource *t, REFIID iid, void **out) { (void)iid; *out = t; return S_OK; }
static ULONG STDMETHODCALLTYPE src_ref(IDWriteTextAnalysisSource *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE src_at(IDWriteTextAnalysisSource *t, UINT32 pos, const WCHAR **text, UINT32 *len) {
    text_t *s = (text_t *)t;
    *text = pos < s->len ? s->text + pos : 0;
    *len = pos < s->len ? s->len - pos : 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_before(IDWriteTextAnalysisSource *t, UINT32 pos, const WCHAR **text, UINT32 *len) {
    (void)t, (void)pos;
    *text = 0, *len = 0;
    return S_OK;
}
static DWRITE_READING_DIRECTION STDMETHODCALLTYPE src_dir(IDWriteTextAnalysisSource *t) {
    (void)t;
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}
static HRESULT STDMETHODCALLTYPE src_locale(IDWriteTextAnalysisSource *t, UINT32 pos, UINT32 *len, const WCHAR **loc) {
    text_t *s = (text_t *)t;
    *len = pos < s->len ? s->len - pos : 0;
    *loc = L"en-us";
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_numsub(IDWriteTextAnalysisSource *t, UINT32 pos, UINT32 *len,
                                           IDWriteNumberSubstitution **sub) {
    text_t *s = (text_t *)t;
    *len = pos < s->len ? s->len - pos : 0;
    *sub = 0;
    return S_OK;
}
static IDWriteTextAnalysisSourceVtbl src_vtbl = { src_qi, src_ref, src_ref, src_at, src_before, src_dir, src_locale, src_numsub };

static void fallback(IDWriteFontFallback *fb, const WCHAR *text, const char *label, const char *want_family,
                     UINT32 want_len) {
    text_t src = { { &src_vtbl }, text, (UINT32)wcslen(text) };
    UINT32 mapped = 0;
    IDWriteFont *font = 0;
    FLOAT scale = 0;
    char fam[128] = "";
    HRESULT hr = IDWriteFontFallback_MapCharacters(fb, &src.iface, 0, src.len, 0, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mapped,
                                                   &font, &scale);
    CHECK(SUCCEEDED(hr) && font, "MapCharacters %s: 0x%08lx", label, (unsigned long)hr);
    if (font) {
        family_of(font, fam, sizeof(fam));
        IDWriteFont_Release(font);
    }
    printf("fallback %s -> %s length %u scale %.1f\n", label, fam, mapped, scale);
    CHECK(!strcmp(fam, want_family) && mapped == want_len, "fallback %s: wanted %s for %u", label, want_family, want_len);
}

/* ---- a font from memory, through the program's own loaders ---- */

static BYTE *mem_font;
static UINT64 mem_size;
static IDWriteFactory *the_factory;

typedef struct {
    IDWriteFontFileStream iface;
    LONG refs;
} mstream_t;
static HRESULT STDMETHODCALLTYPE ms_qi(IDWriteFontFileStream *t, REFIID iid, void **out) {
    (void)iid;
    *out = t;
    IDWriteFontFileStream_AddRef(t);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE ms_addref(IDWriteFontFileStream *t) { return (ULONG)InterlockedIncrement(&((mstream_t *)t)->refs); }
static ULONG STDMETHODCALLTYPE ms_release(IDWriteFontFileStream *t) {
    LONG r = InterlockedDecrement(&((mstream_t *)t)->refs);
    if (!r) free(t);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ms_read(IDWriteFontFileStream *t, const void **start, UINT64 off, UINT64 size, void **ctx) {
    (void)t;
    *ctx = 0;
    if (off > mem_size || size > mem_size - off) return E_FAIL;
    *start = mem_font + off;
    return S_OK;
}
static void STDMETHODCALLTYPE ms_release_frag(IDWriteFontFileStream *t, void *ctx) { (void)t, (void)ctx; }
static HRESULT STDMETHODCALLTYPE ms_size(IDWriteFontFileStream *t, UINT64 *size) { (void)t; *size = mem_size; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_time(IDWriteFontFileStream *t, UINT64 *time) { (void)t; *time = 0; return E_NOTIMPL; }
static IDWriteFontFileStreamVtbl ms_vtbl = { ms_qi, ms_addref, ms_release, ms_read, ms_release_frag, ms_size, ms_time };

static HRESULT STDMETHODCALLTYPE ml_qi(IDWriteFontFileLoader *t, REFIID iid, void **out) { (void)iid; *out = t; return S_OK; }
static ULONG STDMETHODCALLTYPE ml_ref(IDWriteFontFileLoader *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE ml_stream(IDWriteFontFileLoader *t, const void *key, UINT32 size, IDWriteFontFileStream **out) {
    mstream_t *s = calloc(1, sizeof(*s));
    (void)t, (void)key, (void)size;
    s->iface.lpVtbl = &ms_vtbl;
    s->refs = 1;
    *out = &s->iface;
    return S_OK;
}
static IDWriteFontFileLoaderVtbl ml_vtbl = { ml_qi, ml_ref, ml_ref, ml_stream };
static IDWriteFontFileLoader mem_loader = { &ml_vtbl };

typedef struct {
    IDWriteFontFileEnumerator iface;
    int at;
} menum_t;
static HRESULT STDMETHODCALLTYPE me_qi(IDWriteFontFileEnumerator *t, REFIID iid, void **out) { (void)iid; *out = t; return S_OK; }
static ULONG STDMETHODCALLTYPE me_ref(IDWriteFontFileEnumerator *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE me_next(IDWriteFontFileEnumerator *t, BOOL *has) {
    *has = ((menum_t *)t)->at++ == 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE me_file(IDWriteFontFileEnumerator *t, IDWriteFontFile **out) {
    static const char key[] = "memory font";
    (void)t;
    return IDWriteFactory_CreateCustomFontFileReference(the_factory, key, sizeof(key), &mem_loader, out);
}
static IDWriteFontFileEnumeratorVtbl me_vtbl = { me_qi, me_ref, me_ref, me_next, me_file };
static menum_t mem_enum = { { &me_vtbl }, 0 };

static HRESULT STDMETHODCALLTYPE mc_qi(IDWriteFontCollectionLoader *t, REFIID iid, void **out) { (void)iid; *out = t; return S_OK; }
static ULONG STDMETHODCALLTYPE mc_ref(IDWriteFontCollectionLoader *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE mc_enum(IDWriteFontCollectionLoader *t, IDWriteFactory *f, const void *key, UINT32 size,
                                         IDWriteFontFileEnumerator **out) {
    (void)t, (void)f, (void)key, (void)size;
    mem_enum.at = 0;
    *out = &mem_enum.iface;
    return S_OK;
}
static IDWriteFontCollectionLoaderVtbl mc_vtbl = { mc_qi, mc_ref, mc_ref, mc_enum };
static IDWriteFontCollectionLoader mem_coll_loader = { &mc_vtbl };

int main(int argc, char **argv) {
    IDWriteFactory *f = 0;
    IDWriteFactory2 *f2 = 0;
    IDWriteFontCollection *sys = 0;
    IDWriteFontFamily *fam = 0;
    IDWriteFont *font = 0;
    IDWriteFontFace *face = 0;
    HRESULT hr;
    UINT32 index = 0;
    BOOL exists = FALSE;
    char name[128];
    UINT16 g[3] = { 0 };
    UINT32 cps[3] = { 'A', 'B', ' ' };
    DWRITE_FONT_METRICS fm;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&f);
    CHECK(SUCCEEDED(hr) && f, "DWriteCreateFactory 0x%08lx", (unsigned long)hr);
    if (!f) return 1;
    the_factory = f;
    hr = IDWriteFactory_QueryInterface(f, &IID_IDWriteFactory2, (void **)&f2);
    printf("factory ok, factory2 %s\n", SUCCEEDED(hr) && f2 ? "ok" : "missing");

    hr = IDWriteFactory_GetSystemFontCollection(f, &sys, FALSE);
    CHECK(SUCCEEDED(hr) && sys, "GetSystemFontCollection 0x%08lx", (unsigned long)hr);
    if (!sys) return 1;
    printf("families %u\n", IDWriteFontCollection_GetFontFamilyCount(sys));
    IDWriteFontCollection_FindFamilyName(sys, L"No Such Font Family", &index, &exists);
    CHECK(!exists, "a family that is not there was found");

    /* Windows' UI font: the closest there is */
    IDWriteFontCollection_FindFamilyName(sys, L"Segoe UI", &index, &exists);
    CHECK(exists, "Segoe UI not found");
    if (!exists) return 1;
    IDWriteFontCollection_GetFontFamily(sys, index, &fam);
    IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, &font);
    family_of(font, name, sizeof(name));
    printf("segoe ui -> %s, %u fonts\n", name, IDWriteFontFamily_GetFontCount(fam));
    IDWriteFont_Release(font);
    IDWriteFontFamily_Release(fam);

    /* a family with every style: DejaVu Sans */
    IDWriteFontCollection_FindFamilyName(sys, L"Arial", &index, &exists);
    CHECK(exists, "Arial not found");
    IDWriteFontCollection_GetFontFamily(sys, index, &fam);
    IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL,
                                           DWRITE_FONT_STYLE_ITALIC, &font);
    family_of(font, name, sizeof(name));
    printf("arial bold italic -> %s weight %d style %d simulations %d\n", name, IDWriteFont_GetWeight(font),
           IDWriteFont_GetStyle(font), IDWriteFont_GetSimulations(font));
    CHECK(IDWriteFont_GetWeight(font) == 700 && IDWriteFont_GetStyle(font) != DWRITE_FONT_STYLE_NORMAL, "bold italic");
    IDWriteFont_Release(font);
    IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, &font);
    IDWriteFont_CreateFontFace(font, &face);
    CHECK(face != 0, "CreateFontFace");
    if (!face) return 1;
    {
        IDWriteFontFace *again = 0;
        IDWriteFont_CreateFontFace(font, &again);
        CHECK(again == face, "a font's face is one object");
        if (again) IDWriteFontFace_Release(again);
    }

    /* metrics, glyphs, tables */
    IDWriteFontFace_GetMetrics(face, &fm);
    printf("upem %u ascent %u descent %u glyphs %u type %d\n", fm.designUnitsPerEm, fm.ascent, fm.descent,
           IDWriteFontFace_GetGlyphCount(face), IDWriteFontFace_GetType(face));
    CHECK(fm.designUnitsPerEm >= 256 && fm.ascent > fm.designUnitsPerEm / 2 && fm.descent > 0, "metrics");
    IDWriteFontFace_GetGlyphIndices(face, cps, 3, g);
    printf("glyphs A=%u B=%u space=%u\n", g[0], g[1], g[2]);
    CHECK(g[0] && g[1] && g[2] && g[0] != g[1], "glyph indices");
    {
        DWRITE_GLYPH_METRICS gm;
        IDWriteFontFace_GetDesignGlyphMetrics(face, g, 1, &gm, FALSE);
        printf("A advance %u lsb %d\n", gm.advanceWidth, gm.leftSideBearing);
        CHECK(gm.advanceWidth > fm.designUnitsPerEm / 3 && gm.advanceWidth < fm.designUnitsPerEm, "advance");
    }
    {
        const BYTE *t = 0;
        UINT32 size = 0;
        void *ctx = 0;
        BOOL there = FALSE;
        IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('h', 'e', 'a', 'd'), (const void **)&t, &size,
                                        &ctx, &there);
        CHECK(there && size >= 54 && t[12] == 0x5F && t[13] == 0x0F && t[14] == 0x3C && t[15] == 0xF5, "head table");
        printf("head table %u bytes\n", size);
        IDWriteFontFace_ReleaseFontTable(face, ctx);
        IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('z', 'z', 'z', 'z'), (const void **)&t, &size,
                                        &ctx, &there);
        CHECK(!there, "a table that is not there");
    }
    {
        IDWriteFontFace1 *f1 = 0;
        UINT32 n = 0;
        IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace1, (void **)&f1);
        CHECK(f1 != 0, "IDWriteFontFace1");
        if (f1) {
            hr = IDWriteFontFace1_GetUnicodeRanges(f1, 0, 0, &n);
            printf("unicode ranges %u\n", n);
            CHECK(hr == E_NOT_SUFFICIENT_BUFFER && n > 10, "unicode ranges");
            IDWriteFontFace1_Release(f1);
        }
    }

    /* the outline of A at 16 DIPs: above the baseline (y < 0), about 11 wide */
    {
        sink_t s = { { &sink_vtbl }, 0, 0, 0, 0, 1e9f, -1e9f, 1e9f, -1e9f };
        hr = IDWriteFontFace_GetGlyphRunOutline(face, 16.0f, g, 0, 0, 1, FALSE, FALSE, (IDWriteGeometrySink *)&s.iface);
        printf("outline figures %d segments %d x %.1f..%.1f y %.1f..%.1f\n", s.figures, s.segments, s.minx, s.maxx,
               s.miny, s.maxy);
        CHECK(SUCCEEDED(hr) && s.figures >= 2 && s.segments >= 6 && !s.unbalanced && !s.open, "outline figures");
        CHECK(s.minx > -1 && s.maxx < 16 && s.maxx - s.minx > 6 && s.miny < -9 && s.miny > -16 && s.maxy > -1 &&
                  s.maxy < 1,
              "outline bounds");
    }

    /* what A covers at 32 pixels */
    {
        DWRITE_GLYPH_RUN run;
        FLOAT adv = 0;
        DWRITE_GLYPH_OFFSET off = { 0, 0 };
        IDWriteGlyphRunAnalysis *an = 0;
        RECT r, r2;
        memset(&run, 0, sizeof(run));
        run.fontFace = face, run.fontEmSize = 32, run.glyphCount = 1, run.glyphIndices = g, run.glyphAdvances = &adv;
        run.glyphOffsets = &off;
        hr = IDWriteFactory_CreateGlyphRunAnalysis(f, &run, 1.0f, 0, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                   DWRITE_MEASURING_MODE_NATURAL, 0, 0, &an);
        CHECK(SUCCEEDED(hr) && an, "CreateGlyphRunAnalysis 0x%08lx", (unsigned long)hr);
        if (an) {
            LONG w, h;
            BYTE *buf;
            long sum = 0, full = 0;
            IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &r);
            IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(an, DWRITE_TEXTURE_ALIASED_1x1, &r2);
            w = r.right - r.left, h = r.bottom - r.top;
            buf = calloc((size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1) * 3, 1);
            hr = IDWriteGlyphRunAnalysis_CreateAlphaTexture(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &r, buf,
                                                            (UINT32)(w * h * 3));
            for (LONG i = 0; i < w * h * 3; i++) sum += buf[i], full += buf[i] == 255;
            printf("coverage %ldx%ld at %ld,%ld: %ld full of %ld\n", w, h, r.left, r.top, full / 3, w * h);
            CHECK(SUCCEEDED(hr) && w >= 15 && w <= 30 && h >= 18 && h <= 30 && r.top < -18 && r.bottom >= -1 &&
                      r.bottom <= 2 && full > 3 * 40,
                  "coverage");
            CHECK(r2.right == r2.left, "a ClearType analysis has no aliased texture");
            free(buf);
            IDWriteGlyphRunAnalysis_Release(an);
        }
        if (f2) {
            /* the method shares its name with a macro: called through the vtable */
            hr = (f2->lpVtbl->IDWriteFactory2_CreateGlyphRunAnalysis)(
                f2, &run, 0, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
                DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &an);
            CHECK(SUCCEEDED(hr) && an, "IDWriteFactory2::CreateGlyphRunAnalysis");
            if (an) {
                IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(an, DWRITE_TEXTURE_ALIASED_1x1, &r2);
                printf("grayscale %ldx%ld\n", r2.right - r2.left, r2.bottom - r2.top);
                CHECK(r2.right - r2.left == r.right - r.left && r2.bottom - r2.top == r.bottom - r.top, "grayscale");
                IDWriteGlyphRunAnalysis_Release(an);
            }
        }
    }
    IDWriteFontFace_Release(face);
    IDWriteFont_Release(font);
    IDWriteFontFamily_Release(fam);

    /* characters the UI font lacks: a font that has them */
    if (f2) {
        IDWriteFontFallback *fb = 0;
        IDWriteFactory2_GetSystemFontFallback(f2, &fb);
        CHECK(fb != 0, "GetSystemFontFallback");
        if (fb) {
            fallback(fb, L"abc", "abc", "Inter", 3);
            fallback(fb, L"\x05D0\x05D1", "05D0", "DejaVu Sans", 2);
            IDWriteFontFallback_Release(fb);
        }
    }

    /* a font from memory, through the program's loaders */
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        CHECK(fp != 0, "cannot open %s", argv[1]);
        if (fp) {
            IDWriteFontCollection *mc = 0;
            fseek(fp, 0, SEEK_END);
            mem_size = (UINT64)ftell(fp);
            fseek(fp, 0, SEEK_SET);
            mem_font = malloc((size_t)mem_size);
            fread(mem_font, 1, (size_t)mem_size, fp);
            fclose(fp);
            CHECK(SUCCEEDED(IDWriteFactory_RegisterFontFileLoader(f, &mem_loader)), "RegisterFontFileLoader");
            CHECK(IDWriteFactory_RegisterFontFileLoader(f, &mem_loader) == DWRITE_E_ALREADYREGISTERED, "registered twice");
            CHECK(SUCCEEDED(IDWriteFactory_RegisterFontCollectionLoader(f, &mem_coll_loader)), "RegisterFontCollectionLoader");
            hr = IDWriteFactory_CreateCustomFontCollection(f, &mem_coll_loader, 0, 0, &mc);
            CHECK(SUCCEEDED(hr) && mc, "CreateCustomFontCollection 0x%08lx", (unsigned long)hr);
            if (mc) {
                IDWriteFontFamily *mf = 0;
                IDWriteFont *mfont = 0;
                IDWriteFontFace *mface = 0;
                UINT16 mg = 0;
                UINT32 cp = 'g';
                CHECK(IDWriteFontCollection_GetFontFamilyCount(mc) == 1, "one family");
                IDWriteFontCollection_GetFontFamily(mc, 0, &mf);
                IDWriteFontFamily_GetFont(mf, 0, &mfont);
                family_of(mfont, name, sizeof(name));
                IDWriteFont_CreateFontFace(mfont, &mface);
                if (mface) IDWriteFontFace_GetGlyphIndices(mface, &cp, 1, &mg);
                printf("memory font %s, glyph of g %u\n", name, mg);
                CHECK(name[0] && mg, "memory font");
                {
                    IDWriteFontFile *file = 0;
                    IDWriteFontFileLoader *loader = 0;
                    UINT32 nf = 1;
                    if (mface) IDWriteFontFace_GetFiles(mface, &nf, &file);
                    if (file) IDWriteFontFile_GetLoader(file, &loader);
                    CHECK(loader == &mem_loader, "the face's file has the program's loader");
                    if (file) IDWriteFontFile_Release(file);
                }
                if (mface) IDWriteFontFace_Release(mface);
                if (mfont) IDWriteFont_Release(mfont);
                if (mf) IDWriteFontFamily_Release(mf);
                IDWriteFontCollection_Release(mc);
            }
            IDWriteFactory_UnregisterFontCollectionLoader(f, &mem_coll_loader);
            IDWriteFactory_UnregisterFontFileLoader(f, &mem_loader);
        }
    }

    IDWriteFontCollection_Release(sys);
    if (f2) IDWriteFactory2_Release(f2);
    IDWriteFactory_Release(f);
    printf("done, %d failed\n", fails);
    return fails ? 1 : 0;
}
