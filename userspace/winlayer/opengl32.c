/* opengl32: OpenGL for Windows programs, on Mesa (llvmpipe, through OSMesa).
 *
 * A context renders into its window's surface (for the main window, the
 * ICDA window's own pixels), in BGRA with the top row first, which is what
 * the desktop shows: SwapBuffers is a glFinish (Mesa then writes the frame
 * into the surface) and a flush to the desktop.  The GL functions are
 * Mesa's behind thunks that take the Windows calling convention
 * (gl_thunks.c, generated); WGL and its ARB extensions are here.
 *
 * Swap interval 1 or more holds frames to 60 a second (there is no
 * vertical blank to wait for), so a game's menus don't use a core for
 * nothing. */
#define _GNU_SOURCE
#include "wingui.h"
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

typedef struct { const char *name; void *thunk; void **mesa; } gl_func_t;
extern const gl_func_t gl_funcs[];
extern const int gl_nfuncs, gl_ncore;

/* ---- the functions ------------------------------------------------------------------------- */

static pthread_once_t resolved = PTHREAD_ONCE_INIT;
static int *ftable;        /* hash: index + 1 */
static int fcap;

static uint32_t fhash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void gl_resolve(void) {
    fcap = 8192;
    ftable = calloc((size_t)fcap, sizeof(int));
    for (int i = 0; i < gl_nfuncs; i++) {
        uint32_t h = fhash(gl_funcs[i].name) & (uint32_t)(fcap - 1);
        *gl_funcs[i].mesa = (void *)OSMesaGetProcAddress(gl_funcs[i].name);
        while (ftable[h]) h = (h + 1) & (uint32_t)(fcap - 1);
        ftable[h] = i + 1;
    }
}

static const gl_func_t *gl_find(const char *name) {
    uint32_t h;
    pthread_once(&resolved, gl_resolve);
    h = fhash(name) & (uint32_t)(fcap - 1);
    while (ftable[h]) {
        const gl_func_t *f = &gl_funcs[ftable[h] - 1];
        if (!strcmp(f->name, name)) return f;
        h = (h + 1) & (uint32_t)(fcap - 1);
    }
    return 0;
}

/* debug output: the program's callback is a Windows one */
typedef void (WCALLBACK *win_debugproc)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
static win_debugproc debug_cb, debug_cb_arb, debug_cb_amd;

static void debug_tramp(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *user) {
    if (debug_cb) debug_cb(src, type, id, sev, len, msg, user);
}
static void debug_tramp_arb(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *user) {
    if (debug_cb_arb) debug_cb_arb(src, type, id, sev, len, msg, user);
}
typedef void (WCALLBACK *win_debugproc_amd)(GLuint, GLenum, GLenum, GLsizei, const GLchar *, void *);
static void debug_tramp_amd(GLuint id, GLenum cat, GLenum sev, GLsizei len, const GLchar *msg, void *user) {
    if (debug_cb_amd) ((win_debugproc_amd)(void *)debug_cb_amd)(id, cat, sev, len, msg, user);
}

static void WINAPI t_glDebugMessageCallback(win_debugproc cb, const void *user) {
    PFNGLDEBUGMESSAGECALLBACKPROC f = (PFNGLDEBUGMESSAGECALLBACKPROC)OSMesaGetProcAddress("glDebugMessageCallback");
    debug_cb = cb;
    if (f) f(cb ? debug_tramp : 0, user);
}
static void WINAPI t_glDebugMessageCallbackARB(win_debugproc cb, const void *user) {
    PFNGLDEBUGMESSAGECALLBACKARBPROC f = (PFNGLDEBUGMESSAGECALLBACKARBPROC)OSMesaGetProcAddress("glDebugMessageCallbackARB");
    debug_cb_arb = cb;
    if (f) f(cb ? debug_tramp_arb : 0, user);
}
static void WINAPI t_glDebugMessageCallbackAMD(void *cb, void *user) {
    PFNGLDEBUGMESSAGECALLBACKAMDPROC f = (PFNGLDEBUGMESSAGECALLBACKAMDPROC)OSMesaGetProcAddress("glDebugMessageCallbackAMD");
    debug_cb_amd = (win_debugproc)cb;
    if (f) f(cb ? debug_tramp_amd : 0, user);
}

/* ---- anisotropic filtering, capped ----------------------------------------------------------- */

/* llvmpipe filters anisotropically by taking a sample for each step of a
 * texture's stretch: a floor or water seen at a low angle costs several
 * times a plain surface, and one seen edge-on (the camera at water level)
 * took Mesa 24.0 nearly two minutes for one frame.  Without it, Ironwail's
 * LibreQuake timedemo runs 3.9 times as fast.  So the most a program gets
 * is 1 (trilinear filtering), or what ICDA_WIN_ANISO allows, and that is
 * the maximum it is told of. */
#define ANISO     0x84FE        /* GL_TEXTURE_MAX_ANISOTROPY */
#define ANISO_MAX 0x84FF        /* GL_MAX_TEXTURE_MAX_ANISOTROPY */

static float aniso_cap(void) {
    static float cap;
    if (!cap) {
        const char *e = getenv("ICDA_WIN_ANISO");
        cap = e && atof(e) >= 1.0 ? (float)atof(e) : 1.0f;
    }
    return cap;
}

static void *mesa_of(const char *name) {
    const gl_func_t *f = gl_find(name);
    return f ? *f->mesa : 0;
}

#define CAPF(v) ((v) > aniso_cap() ? aniso_cap() : (v))
#define CAPI(v) ((float)(v) > aniso_cap() ? (GLint)aniso_cap() : (v))

/* the setters: (object), pname, value; the vector forms read one value for this pname */
#define SETTER(name, obj_t, val_t, cap)                                                   \
    static void WINAPI o_##name(obj_t o, GLenum pname, val_t v) {                         \
        static void (*m)(obj_t, GLenum, val_t);                                            \
        if (!m) m = (void (*)(obj_t, GLenum, val_t))mesa_of(#name);                        \
        if (pname == ANISO) v = cap(v);                                                    \
        if (m) m(o, pname, v);                                                             \
    }
#define SETTERV(name, obj_t, val_t, cap)                                                  \
    static void WINAPI o_##name(obj_t o, GLenum pname, const val_t *v) {                  \
        static void (*m)(obj_t, GLenum, const val_t *);                                    \
        val_t one;                                                                         \
        if (!m) m = (void (*)(obj_t, GLenum, const val_t *))mesa_of(#name);                \
        if (pname == ANISO && v) one = cap(*v), v = &one;                                  \
        if (m) m(o, pname, v);                                                             \
    }
#define SETTER_EXT(name, val_t, cap)                                                      \
    static void WINAPI o_##name(GLuint o, GLenum target, GLenum pname, val_t v) {         \
        static void (*m)(GLuint, GLenum, GLenum, val_t);                                   \
        if (!m) m = (void (*)(GLuint, GLenum, GLenum, val_t))mesa_of(#name);               \
        if (pname == ANISO) v = cap(v);                                                    \
        if (m) m(o, target, pname, v);                                                     \
    }
#define SETTERV_EXT(name, val_t, cap)                                                     \
    static void WINAPI o_##name(GLuint o, GLenum target, GLenum pname, const val_t *v) {  \
        static void (*m)(GLuint, GLenum, GLenum, const val_t *);                           \
        val_t one;                                                                         \
        if (!m) m = (void (*)(GLuint, GLenum, GLenum, const val_t *))mesa_of(#name);       \
        if (pname == ANISO && v) one = cap(*v), v = &one;                                  \
        if (m) m(o, target, pname, v);                                                     \
    }

SETTER(glTexParameterf, GLenum, GLfloat, CAPF)
SETTER(glTexParameteri, GLenum, GLint, CAPI)
SETTERV(glTexParameterfv, GLenum, GLfloat, CAPF)
SETTERV(glTexParameteriv, GLenum, GLint, CAPI)
SETTER(glTextureParameterf, GLuint, GLfloat, CAPF)
SETTER(glTextureParameteri, GLuint, GLint, CAPI)
SETTERV(glTextureParameterfv, GLuint, GLfloat, CAPF)
SETTERV(glTextureParameteriv, GLuint, GLint, CAPI)
SETTER(glSamplerParameterf, GLuint, GLfloat, CAPF)
SETTER(glSamplerParameteri, GLuint, GLint, CAPI)
SETTERV(glSamplerParameterfv, GLuint, GLfloat, CAPF)
SETTERV(glSamplerParameteriv, GLuint, GLint, CAPI)
SETTER_EXT(glTextureParameterfEXT, GLfloat, CAPF)
SETTER_EXT(glTextureParameteriEXT, GLint, CAPI)
SETTERV_EXT(glTextureParameterfvEXT, GLfloat, CAPF)
SETTERV_EXT(glTextureParameterivEXT, GLint, CAPI)

/* the maximum a program is told of */
static void WINAPI o_glGetFloatv(GLenum pname, GLfloat *v) {
    static void (*m)(GLenum, GLfloat *);
    if (!m) m = (void (*)(GLenum, GLfloat *))mesa_of("glGetFloatv");
    if (m) m(pname, v);
    if (pname == ANISO_MAX && v) *v = CAPF(*v);
}
static void WINAPI o_glGetIntegerv(GLenum pname, GLint *v) {
    static void (*m)(GLenum, GLint *);
    if (!m) m = (void (*)(GLenum, GLint *))mesa_of("glGetIntegerv");
    if (m) m(pname, v);
    if (pname == ANISO_MAX && v) *v = CAPI(*v);
}
static void WINAPI o_glGetDoublev(GLenum pname, GLdouble *v) {
    static void (*m)(GLenum, GLdouble *);
    if (!m) m = (void (*)(GLenum, GLdouble *))mesa_of("glGetDoublev");
    if (m) m(pname, v);
    if (pname == ANISO_MAX && v && *v > aniso_cap()) *v = aniso_cap();
}

/* the GL functions answered here rather than by Mesa directly */
static const struct {
    const char *name;
    void       *fn;
} gl_overrides[] = {
#define O(n) { #n, (void *)o_##n }
    O(glTexParameterf), O(glTexParameteri), O(glTexParameterfv), O(glTexParameteriv),
    O(glTextureParameterf), O(glTextureParameteri), O(glTextureParameterfv), O(glTextureParameteriv),
    O(glSamplerParameterf), O(glSamplerParameteri), O(glSamplerParameterfv), O(glSamplerParameteriv),
    O(glTextureParameterfEXT), O(glTextureParameteriEXT), O(glTextureParameterfvEXT), O(glTextureParameterivEXT),
    O(glGetFloatv), O(glGetIntegerv), O(glGetDoublev),
#undef O
};

static void *gl_override(const char *name) {
    for (unsigned i = 0; i < sizeof(gl_overrides) / sizeof(gl_overrides[0]); i++)
        if (!strcmp(gl_overrides[i].name, name)) return gl_overrides[i].fn;
    return 0;
}

/* ---- contexts ------------------------------------------------------------------------------- */

typedef struct glctx {
    int           magic;
    OSMesaContext osm;
    int           major, minor, core;
    uint32_t     *bound;        /* the buffer it renders into */
    int           bw, bh;
    HWND          hwnd;
} glctx_t;

#define CTX_MAGIC 0x474C4358
static __thread glctx_t *cur;
static __thread HDC cur_dc;
static int swap_interval = 1;

static glctx_t *ctx_get(HGLRC h) {
    glctx_t *c = h;
    return c && (uintptr_t)c >= 0x10000 && c->magic == CTX_MAGIC ? c : 0;
}

static HGLRC create_context(HDC dc, glctx_t *share, int major, int minor, int core, int debug) {
    glctx_t *c;
    int attrs[20], n = 0;
    (void)dc, (void)debug;
    pthread_once(&resolved, gl_resolve);
    attrs[n++] = OSMESA_FORMAT, attrs[n++] = OSMESA_BGRA;
    attrs[n++] = OSMESA_DEPTH_BITS, attrs[n++] = 24;
    attrs[n++] = OSMESA_STENCIL_BITS, attrs[n++] = 8;
    attrs[n++] = OSMESA_ACCUM_BITS, attrs[n++] = 0;
    attrs[n++] = OSMESA_PROFILE, attrs[n++] = core ? OSMESA_CORE_PROFILE : OSMESA_COMPAT_PROFILE;
    if (major) {
        attrs[n++] = OSMESA_CONTEXT_MAJOR_VERSION, attrs[n++] = major;
        attrs[n++] = OSMESA_CONTEXT_MINOR_VERSION, attrs[n++] = minor;
    }
    attrs[n] = 0;
    c = calloc(1, sizeof(*c));
    c->magic = CTX_MAGIC;
    c->osm = OSMesaCreateContextAttribs(attrs, share ? share->osm : 0);
    if (!c->osm && major) {
        /* a version Mesa lacks: the newest it has will do, as drivers give */
        attrs[n - 4] = 0;
        c->osm = OSMesaCreateContextAttribs(attrs, share ? share->osm : 0);
    }
    if (!c->osm) {
        free(c);
        wset_last_error(2000);
        wlog("OpenGL: no context (%d.%d %s)", major, minor, core ? "core" : "compatibility");
        return 0;
    }
    c->major = major, c->minor = minor, c->core = core;
    if (wtrace) wlog("OpenGL context %p: %d.%d %s", (void *)c, major, minor, core ? "core" : "compatibility");
    return c;
}

HGLRC WINAPI wglCreateContext(HDC dc) { return create_context(dc, 0, 0, 0, 0, 0); }

HGLRC WINAPI wglCreateLayerContext(HDC dc, int layer) {
    (void)layer;
    return wglCreateContext(dc);
}

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB         0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126

static HGLRC WINAPI wglCreateContextAttribsARB(HDC dc, HGLRC share, const int *attr) {
    int major = 0, minor = 0, core = 0, flags = 0;
    for (const int *a = attr; a && a[0]; a += 2) {
        switch (a[0]) {
        case WGL_CONTEXT_MAJOR_VERSION_ARB: major = a[1]; break;
        case WGL_CONTEXT_MINOR_VERSION_ARB: minor = a[1]; break;
        case WGL_CONTEXT_FLAGS_ARB: flags = a[1]; break;
        case WGL_CONTEXT_PROFILE_MASK_ARB: core = (a[1] & 1) != 0; break;
        default: break;
        }
    }
    /* compatibility contexts before 3.2 come as the newest compatibility one */
    if (!core && major && (major < 3 || (major == 3 && minor < 2))) major = minor = 0;
    return create_context(dc, ctx_get(share), major, minor, core, flags & 1);
}

BOOL WINAPI wglDeleteContext(HGLRC h) {
    glctx_t *c = ctx_get(h);
    if (!c) return FALSE;
    if (c == cur) {
        OSMesaMakeCurrent(0, 0, 0, 0, 0);
        cur = 0;
        cur_dc = 0;
    }
    OSMesaDestroyContext(c->osm);
    c->magic = 0;
    free(c);
    return TRUE;
}

/* binds c to its window's surface (again, after a resize) */
static BOOL bind(glctx_t *c, wwnd_t *w) {
    wnd_surface(w);
    if (!w->surface) return FALSE;
    if (!OSMesaMakeCurrent(c->osm, w->surface, GL_UNSIGNED_BYTE, w->sw, w->sh)) {
        wlog("OpenGL: the context could not be made current (%dx%d)", w->sw, w->sh);
        return FALSE;
    }
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    c->bound = w->surface;
    c->bw = w->sw, c->bh = w->sh;
    c->hwnd = w->h;
    return TRUE;
}

BOOL WINAPI wglMakeCurrent(HDC dc, HGLRC h) {
    glctx_t *c = ctx_get(h);
    wwnd_t *w;
    if (!h) {
        OSMesaMakeCurrent(0, 0, 0, 0, 0);
        cur = 0;
        cur_dc = 0;
        return TRUE;
    }
    w = dc_window(dc);
    if (!c || !w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!bind(c, w)) return FALSE;
    cur = c;
    cur_dc = dc;
    return TRUE;
}

static BOOL WINAPI wglMakeContextCurrentARB(HDC draw, HDC read, HGLRC h) {
    (void)read;
    return wglMakeCurrent(draw, h);
}

HGLRC WINAPI wglGetCurrentContext(void) { return cur; }
HDC WINAPI wglGetCurrentDC(void) { return cur_dc; }
static HDC WINAPI wglGetCurrentReadDCARB(void) { return cur_dc; }

BOOL WINAPI wglShareLists(HGLRC a, HGLRC b) {
    (void)a, (void)b;
    return TRUE;     /* OSMesa shares at creation only */
}

BOOL WINAPI wglCopyContext(HGLRC a, HGLRC b, UINT mask) {
    (void)a, (void)b, (void)mask;
    return FALSE;
}

/* ---- presenting --------------------------------------------------------------------------- */

static uint64_t last_swap;

BOOL gl_swap_buffers(HDC dc) {
    wwnd_t *w = dc_window(dc);
    glctx_t *c = cur;
    if (!w) return FALSE;
    if (c && c->hwnd == w->h) {
        glFinish();            /* Mesa writes the frame into the surface */
        wnd_present(w);
        /* the window changed size: render into its new surface */
        if (w->surface != c->bound || w->sw != c->bw || w->sh != c->bh) bind(c, w);
    }
    if (swap_interval > 0) {
        uint64_t frame = 16666667ull * (uint64_t)swap_interval, now = mono_ns();
        if (last_swap && now - last_swap < frame) {
            struct timespec ts = { 0, (long)(frame - (now - last_swap)) };
            nanosleep(&ts, 0);
            now = mono_ns();
        }
        last_swap = now;
    }
    return TRUE;
}

BOOL WINAPI wglSwapBuffers(HDC dc) { return gl_swap_buffers(dc); }

void gl_window_changed(wwnd_t *w) {
    glctx_t *c = cur;
    if (c && c->hwnd == w->h && (w->surface != c->bound || w->sw != c->bw || w->sh != c->bh)) bind(c, w);
}
BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) {
    (void)planes;
    return gl_swap_buffers(dc);
}
DWORD WINAPI wglSwapMultipleBuffers(UINT n, const void *bufs) {
    (void)n, (void)bufs;
    return 0;
}

static BOOL WINAPI wglSwapIntervalEXT(int n) {
    swap_interval = n < 0 ? 1 : n;
    if (wtrace) wlog("swap interval %d", n);
    return TRUE;
}
static int WINAPI wglGetSwapIntervalEXT(void) { return swap_interval; }

/* ---- pixel formats ----------------------------------------------------------------------- */

int WINAPI DescribePixelFormat(HDC dc, int i, UINT n, void *p);
int WINAPI ChoosePixelFormat(HDC dc, const void *p);
BOOL WINAPI SetPixelFormat(HDC dc, int fmt, const void *p);
int WINAPI GetPixelFormat(HDC dc);

int WINAPI wglDescribePixelFormat(HDC dc, int i, UINT n, void *p) { return DescribePixelFormat(dc, i, n, p); }
int WINAPI wglChoosePixelFormat(HDC dc, const void *p) { return ChoosePixelFormat(dc, p); }
BOOL WINAPI wglSetPixelFormat(HDC dc, int f, const void *p) { return SetPixelFormat(dc, f, p); }
int WINAPI wglGetPixelFormat(HDC dc) { return GetPixelFormat(dc); }

static BOOL WINAPI wglChoosePixelFormatARB(HDC dc, const int *ia, const float *fa, UINT max, int *formats, UINT *n) {
    (void)dc, (void)ia, (void)fa;
    /* one format for everything asked (a request for multisampling gets it
     * without: refusing would fail the program's window) */
    if (max >= 1 && formats) formats[0] = 1;
    *n = 1;
    return TRUE;
}

static int format_attr(int a) {
    switch (a) {
    case 0x2000: return 1;          /* NUMBER_PIXEL_FORMATS */
    case 0x2001: return 1;          /* DRAW_TO_WINDOW */
    case 0x2002: return 0;          /* DRAW_TO_BITMAP */
    case 0x2003: return 0x2027;     /* ACCELERATION: full */
    case 0x2007: return 0x2028;     /* SWAP_METHOD: exchange */
    case 0x2010: return 1;          /* SUPPORT_OPENGL */
    case 0x2011: return 1;          /* DOUBLE_BUFFER */
    case 0x2012: return 0;          /* STEREO */
    case 0x2013: return 0x202B;     /* PIXEL_TYPE: RGBA */
    case 0x2014: return 32;         /* COLOR_BITS */
    case 0x2015: case 0x2017: case 0x2019: return 8;     /* RED/GREEN/BLUE_BITS */
    case 0x2016: return 16;         /* RED_SHIFT */
    case 0x2018: return 8;          /* GREEN_SHIFT */
    case 0x201A: return 0;          /* BLUE_SHIFT */
    case 0x201B: return 8;          /* ALPHA_BITS */
    case 0x201C: return 24;         /* ALPHA_SHIFT */
    case 0x2022: return 24;         /* DEPTH_BITS */
    case 0x2023: return 8;          /* STENCIL_BITS */
    case 0x2041: case 0x2042: return 0;   /* SAMPLE_BUFFERS, SAMPLES */
    case 0x20A9: return 0;          /* FRAMEBUFFER_SRGB_CAPABLE */
    default: return 0;
    }
}

static BOOL WINAPI wglGetPixelFormatAttribivARB(HDC dc, int fmt, int layer, UINT n, const int *attrs, int *vals) {
    (void)dc, (void)fmt, (void)layer;
    for (UINT i = 0; i < n; i++) vals[i] = format_attr(attrs[i]);
    return TRUE;
}

static BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC dc, int fmt, int layer, UINT n, const int *attrs, float *vals) {
    (void)dc, (void)fmt, (void)layer;
    for (UINT i = 0; i < n; i++) vals[i] = (float)format_attr(attrs[i]);
    return TRUE;
}

static const char wgl_exts[] =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_create_context WGL_ARB_create_context_profile "
    "WGL_ARB_pixel_format WGL_ARB_make_current_read WGL_EXT_swap_control WGL_EXT_swap_control_tear";

static const char *WINAPI wglGetExtensionsStringARB(HDC dc) {
    (void)dc;
    return wgl_exts;
}
static const char *WINAPI wglGetExtensionsStringEXT(void) { return wgl_exts; }

/* ---- wglGetProcAddress -------------------------------------------------------------------- */

static const struct {
    const char *name;
    void       *fn;
} wgl_procs[] = {
    { "wglCreateContextAttribsARB", (void *)wglCreateContextAttribsARB },
    { "wglChoosePixelFormatARB", (void *)wglChoosePixelFormatARB },
    { "wglGetPixelFormatAttribivARB", (void *)wglGetPixelFormatAttribivARB },
    { "wglGetPixelFormatAttribfvARB", (void *)wglGetPixelFormatAttribfvARB },
    { "wglGetExtensionsStringARB", (void *)wglGetExtensionsStringARB },
    { "wglGetExtensionsStringEXT", (void *)wglGetExtensionsStringEXT },
    { "wglSwapIntervalEXT", (void *)wglSwapIntervalEXT },
    { "wglGetSwapIntervalEXT", (void *)wglGetSwapIntervalEXT },
    { "wglMakeContextCurrentARB", (void *)wglMakeContextCurrentARB },
    { "wglGetCurrentReadDCARB", (void *)wglGetCurrentReadDCARB },
    { "glDebugMessageCallback", (void *)t_glDebugMessageCallback },
    { "glDebugMessageCallbackARB", (void *)t_glDebugMessageCallbackARB },
    { "glDebugMessageCallbackKHR", (void *)t_glDebugMessageCallback },
    { "glDebugMessageCallbackAMD", (void *)t_glDebugMessageCallbackAMD },
};

void *WINAPI wglGetProcAddress(const char *name) {
    const gl_func_t *f;
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(wgl_procs) / sizeof(wgl_procs[0]); i++)
        if (!strcmp(wgl_procs[i].name, name)) return wgl_procs[i].fn;
    f = gl_find(name);
    if (f && *f->mesa) return gl_override(name) ? gl_override(name) : f->thunk;
    if (wtrace >= 2) wlog("wglGetProcAddress(%s): none", name);
    return 0;
}

/* ---- the rest of WGL ---------------------------------------------------------------------- */

BOOL WINAPI wglUseFontBitmapsA(HDC dc, DWORD a, DWORD b, DWORD c) {
    (void)dc, (void)a, (void)b, (void)c;
    return FALSE;
}
BOOL WINAPI wglUseFontBitmapsW(HDC dc, DWORD a, DWORD b, DWORD c) {
    (void)dc, (void)a, (void)b, (void)c;
    return FALSE;
}
BOOL WINAPI wglUseFontOutlinesA(HDC dc, DWORD a, DWORD b, DWORD c, float d, float e, int f, void *g) {
    (void)dc, (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g;
    return FALSE;
}
BOOL WINAPI wglDescribeLayerPlane(HDC dc, int a, int b, UINT c, void *d) {
    (void)dc, (void)a, (void)b, (void)c, (void)d;
    return FALSE;
}
int WINAPI wglGetLayerPaletteEntries(HDC dc, int a, int b, int c, void *d) {
    (void)dc, (void)a, (void)b, (void)c, (void)d;
    return 0;
}
int WINAPI wglSetLayerPaletteEntries(HDC dc, int a, int b, int c, const void *d) {
    (void)dc, (void)a, (void)b, (void)c, (void)d;
    return 0;
}
BOOL WINAPI wglRealizeLayerPalette(HDC dc, int a, BOOL b) {
    (void)dc, (void)a, (void)b;
    return FALSE;
}

/* ---- the DLL ------------------------------------------------------------------------------- */

#define WGL_EXPORTS 26
static wexport_t gl_exports[4096 + WGL_EXPORTS];

const wdll_t dll_opengl32 = { "opengl32.dll", gl_exports, 4096 + WGL_EXPORTS };

__attribute__((constructor)) static void gl_exports_init(void) {
    static const wexport_t wgl[WGL_EXPORTS] = {
        WEXP(wglCreateContext), WEXP(wglCreateLayerContext), WEXP(wglDeleteContext), WEXP(wglMakeCurrent),
        WEXP(wglGetCurrentContext), WEXP(wglGetCurrentDC), WEXP(wglShareLists), WEXP(wglCopyContext),
        WEXP(wglGetProcAddress), WEXP(wglSwapBuffers), WEXP(wglSwapLayerBuffers), WEXP(wglSwapMultipleBuffers),
        WEXP(wglDescribePixelFormat), WEXP(wglChoosePixelFormat), WEXP(wglSetPixelFormat), WEXP(wglGetPixelFormat),
        WEXP(wglUseFontBitmapsA), WEXP(wglUseFontBitmapsW), WEXP(wglUseFontOutlinesA), WEXP(wglDescribeLayerPlane),
        WEXP(wglGetLayerPaletteEntries), WEXP(wglSetLayerPaletteEntries), WEXP(wglRealizeLayerPalette),
        WEXPN("wglUseFontOutlinesW", wglUseFontOutlinesA), WEXPN("glDebugMessageCallback", t_glDebugMessageCallback),
        WEXPN("wglGetExtensionsStringARB", wglGetExtensionsStringARB),
    };
    int n = 0;
    for (int i = 0; i < WGL_EXPORTS; i++) gl_exports[n++] = wgl[i];
    /* the GL functions opengl32.dll has itself (gl.h's: 1.0 to 1.3) */
    for (int i = 0; i < gl_ncore && n < 4096 + WGL_EXPORTS; i++) {
        void *o = gl_override(gl_funcs[i].name);
        gl_exports[n].name = gl_funcs[i].name;
        gl_exports[n].fn = o ? o : gl_funcs[i].thunk;
        gl_exports[n].ordinal = 0;
        n++;
    }
}

