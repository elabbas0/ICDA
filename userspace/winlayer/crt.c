/* The C runtime for Windows programs: msvcrt.dll (what mingw builds use),
 * the Universal CRT (ucrtbase.dll and its api-ms-win-crt-* names) and
 * vcruntime140.dll (what Visual C++ builds use).
 *
 * Most functions hand on to the C library, with the Windows calling
 * convention on the outside and the Windows sizes in the types: long is 32
 * bits, wchar_t is 16, struct tm has no extra fields, and a FILE is ours.
 * Paths go through the drive mapping and are found case-blind. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <fenv.h>
#include <time.h>
#include <ctype.h>
#include <malloc.h>
#include <limits.h>
#include <pthread.h>
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/random.h>

int path_find(const char *wpath, char *out, size_t cap);     /* kernel32.c */
int file_fd(HANDLE h);
HANDLE file_from_fd(int fd);
int ms_vscan_ex(const char *in, FILE *f, const char *fmt, ms_va_list ap, int secure);
WCHAR win_towupper(WCHAR c);
WCHAR win_towlower(WCHAR c);

typedef void (WCALLBACK *vfn_t)(void);
typedef int (WCALLBACK *ifn_t)(void);

/* ---- FILE ---------------------------------------------------------------------------------
 * A program's FILE * is one of ours.  The first 48 bytes are msvcrt's FILE
 * as mingw headers know it (_file is the descriptor); the C library's
 * stream follows.  stdin, stdout and stderr are msvcrt's _iob array. */

typedef struct {
    char *_ptr;
    int   _cnt;
    char *_base;
    int   _flag;
    int   _file;
    int   _charbuf;
    int   _bufsiz;
    char *_tmpfname;
} legacy_file_t;

typedef struct {
    legacy_file_t l;
    FILE         *f;
} wfile_t;

static legacy_file_t iob[3] = { { 0, 0, 0, 0x1, 0, 0, 0, 0 }, { 0, 0, 0, 0x2, 1, 0, 0, 0 }, { 0, 0, 0, 0x2, 2, 0, 0, 0 } };

static FILE *F(void *p) {
    if (p == &iob[0]) return stdin;
    if (p == &iob[1]) return stdout;
    if (p == &iob[2]) return stderr;
    if (!p) return 0;
    return ((wfile_t *)p)->f;
}

static void *wrap_file(FILE *f) {
    wfile_t *w;
    if (!f) return 0;
    w = calloc(1, sizeof(*w));
    w->f = f;
    w->l._file = fileno(f);
    w->l._flag = 0x3;
    return w;
}

static void *WINAPI c___iob_func(void) { return iob; }
static void *WINAPI c___acrt_iob_func(unsigned i) { return i < 3 ? &iob[i] : 0; }

/* "rt", "wb+", "r, ccs=UTF-8": the C library's mode */
static void c_mode(const char *in, char *out) {
    int o = 0;
    for (const char *p = in; *p && *p != ',' && o < 6; p++)
        if (*p == 'r' || *p == 'w' || *p == 'a' || *p == '+' || *p == 'x') out[o++] = *p;
    out[o++] = 'e';     /* close-on-exec */
    out[o] = 0;
}

static void *open_file(const char *wpath, const char *mode) {
    char up[1024], m[8];
    FILE *f;
    if (!wpath) {
        errno = EINVAL;
        return 0;
    }
    if (path_find(wpath, up, sizeof(up)) != 0) {
        errno = ENOENT;
        return 0;
    }
    c_mode(mode, m);
    f = fopen(up, m);
    if (wtrace >= 2) wlog("fopen(%s, %s) -> %s", wpath, mode, f ? "ok" : strerror(errno));
    return wrap_file(f);
}

static void *WINAPI c_fopen(const char *p, const char *m) { return open_file(p, m); }

static void *WINAPI c__wfopen(const WCHAR *p, const WCHAR *m) {
    char a[1024], b[32];
    w_to_utf8(p, -1, a, sizeof(a));
    w_to_utf8(m, -1, b, sizeof(b));
    return open_file(a, b);
}

static int WINAPI c_fopen_s(void **out, const char *p, const char *m) {
    *out = open_file(p, m);
    return *out ? 0 : errno;
}

static int WINAPI c__wfopen_s(void **out, const WCHAR *p, const WCHAR *m) {
    *out = c__wfopen(p, m);
    return *out ? 0 : errno;
}

static void *WINAPI c__fsopen(const char *p, const char *m, int sh) {
    (void)sh;
    return open_file(p, m);
}

static void *WINAPI c__wfsopen(const WCHAR *p, const WCHAR *m, int sh) {
    (void)sh;
    return c__wfopen(p, m);
}

static void *WINAPI c_freopen(const char *p, const char *m, void *wf) {
    char up[1024], mm[8];
    FILE *f = F(wf), *n;
    c_mode(m, mm);
    if (!p) return wf;
    if (path_find(p, up, sizeof(up)) != 0) return 0;
    n = freopen(up, mm, f);
    if (!n) return 0;
    if (wf != &iob[0] && wf != &iob[1] && wf != &iob[2]) ((wfile_t *)wf)->f = n;
    return wf;
}

static void *WINAPI c__wfreopen(const WCHAR *p, const WCHAR *m, void *wf) {
    char a[1024], b[32];
    if (!p) return wf;
    w_to_utf8(p, -1, a, sizeof(a));
    w_to_utf8(m, -1, b, sizeof(b));
    return c_freopen(a, b, wf);
}

static void *WINAPI c__fdopen(int fd, const char *m) {
    char mm[8];
    c_mode(m, mm);
    return wrap_file(fdopen(fd, mm));
}

static void *WINAPI c__wfdopen(int fd, const WCHAR *m) {
    char b[32];
    w_to_utf8(m, -1, b, sizeof(b));
    return c__fdopen(fd, b);
}

static int WINAPI c_fclose(void *wf) {
    int r;
    if (!wf) return EOF;
    if (wf == &iob[0] || wf == &iob[1] || wf == &iob[2]) return fflush(F(wf));
    r = fclose(F(wf));
    free(wf);
    return r;
}

static int WINAPI c__fcloseall(void) { return 0; }
static size_t WINAPI c_fread(void *b, size_t s, size_t n, void *f) { return fread(b, s, n, F(f)); }
static size_t WINAPI c_fwrite(const void *b, size_t s, size_t n, void *f) { return fwrite(b, s, n, F(f)); }
static char *WINAPI c_fgets(char *b, int n, void *f) { return fgets(b, n, F(f)); }
static int WINAPI c_fputs(const char *s, void *f) { return fputs(s, F(f)); }
static int WINAPI c_fgetc(void *f) { return fgetc(F(f)); }
static int WINAPI c_fputc(int c, void *f) { return fputc(c, F(f)); }
static int WINAPI c_ungetc(int c, void *f) { return ungetc(c, F(f)); }
static int WINAPI c_fflush(void *f) { return fflush(f ? F(f) : 0); }
static int WINAPI c_feof(void *f) { return feof(F(f)); }
static int WINAPI c_ferror(void *f) { return ferror(F(f)); }
static void WINAPI c_clearerr(void *f) { clearerr(F(f)); }
static void WINAPI c_rewind(void *f) { rewind(F(f)); }
static int WINAPI c_fseek(void *f, LONG off, int wh) { return fseeko(F(f), off, wh); }
static LONG WINAPI c_ftell(void *f) { return (LONG)ftello(F(f)); }
static int WINAPI c__fseeki64(void *f, int64_t off, int wh) { return fseeko(F(f), off, wh); }
static int64_t WINAPI c__ftelli64(void *f) { return ftello(F(f)); }
static int WINAPI c_fgetpos(void *f, int64_t *pos) {
    *pos = ftello(F(f));
    return *pos < 0 ? -1 : 0;
}
static int WINAPI c_fsetpos(void *f, const int64_t *pos) { return fseeko(F(f), *pos, SEEK_SET); }
static int WINAPI c__fileno(void *f) { return fileno(F(f)); }
static int WINAPI c_setvbuf(void *f, char *b, int mode, size_t n) {
    /* Windows: _IOFBF 0, _IOLBF 0x40, _IONBF 4 */
    int m = mode == 4 ? _IONBF : mode == 0x40 ? _IOLBF : _IOFBF;
    if (m == _IOLBF) m = _IOFBF;    /* Windows treats line buffering as full */
    if (F(f) == stdout || F(f) == stderr) m = (m == _IONBF) ? _IONBF : _IOLBF;
    return setvbuf(F(f), b, m, n ? n : BUFSIZ);
}
static void WINAPI c_setbuf(void *f, char *b) { setbuf(F(f), b); }
static int WINAPI c_getc(void *f) { return getc(F(f)); }
static int WINAPI c_putc(int c, void *f) { return putc(c, F(f)); }
static int WINAPI c_getchar(void) { return getchar(); }
static int WINAPI c_putchar(int c) { return putchar(c); }
static int WINAPI c_puts(const char *s) { return puts(s); }
static void WINAPI c__lock_file(void *f) { flockfile(F(f)); }
static void WINAPI c__unlock_file(void *f) { funlockfile(F(f)); }
static int WINAPI c__fgetc_nolock(void *f) { return getc_unlocked(F(f)); }
static int WINAPI c__fputc_nolock(int c, void *f) { return putc_unlocked(c, F(f)); }
static size_t WINAPI c__fread_nolock(void *b, size_t s, size_t n, void *f) { return fread_unlocked(b, s, n, F(f)); }
static size_t WINAPI c__fwrite_nolock(const void *b, size_t s, size_t n, void *f) { return fwrite_unlocked(b, s, n, F(f)); }
static void *WINAPI c_tmpfile(void) { return wrap_file(tmpfile()); }
static int WINAPI c__flushall(void) { return fflush(0); }

static void WINAPI c_perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

static int WINAPI c_fputws(const WCHAR *s, void *f) {
    char *a = w_to_utf8_dup(s);
    int r = fputs(a, F(f));
    free(a);
    return r < 0 ? -1 : 0;
}

static WCHAR *WINAPI c_fgetws(WCHAR *b, int n, void *f) {
    char *tmp = malloc((size_t)n * 3 + 4);
    if (!fgets(tmp, n, F(f))) {
        free(tmp);
        return 0;
    }
    utf8_to_w(tmp, -1, b, n);
    free(tmp);
    return b;
}

static unsigned short WINAPI c_fgetwc(void *f) {
    int c = fgetc(F(f));
    return c == EOF ? 0xFFFF : (unsigned short)c;
}

static unsigned short WINAPI c_fputwc(unsigned short c, void *f) {
    char b[8];
    WCHAR w = c;
    int n = w_to_utf8(&w, 1, b, sizeof(b)) - 1;
    fwrite(b, 1, (size_t)n, F(f));
    return c;
}

static unsigned short WINAPI c_ungetwc(unsigned short c, void *f) { return (unsigned short)ungetc(c, F(f)); }
static int WINAPI c__setmode(int fd, int mode) {
    (void)fd, (void)mode;
    return 0x8000;     /* was binary */
}
static int WINAPI c__isatty(int fd) { return isatty(fd); }

/* ---- printf and scanf ------------------------------------------------------------------------ */

#define FMT_STACK 2048

/* formats into a buffer of its own: *outp is small or malloc'ed */
static int vformat_alloc(char *small, char **outp, const char *fmt, ms_va_list ap, int wide) {
    int n = ms_vformat(small, FMT_STACK, fmt, ap, wide);
    *outp = small;
    if (n >= FMT_STACK) {
        *outp = malloc((size_t)n + 1);
        ms_vformat(*outp, (size_t)n + 1, fmt, ap, wide);
    }
    return n;
}

static int vfprintf_ms(void *f, const char *fmt, ms_va_list ap) {
    char small[FMT_STACK], *out;
    int n = vformat_alloc(small, &out, fmt, ap, 0);
    fwrite(out, 1, (size_t)n, F(f));
    if (out != small) free(out);
    return n;
}

static int WINAPI c_vfprintf(void *f, const char *fmt, ms_va_list ap) { return vfprintf_ms(f, fmt, ap); }
static int WINAPI c_vprintf(const char *fmt, ms_va_list ap) { return vfprintf_ms(&iob[1], fmt, ap); }

static int WINAPI c_fprintf(void *f, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = vfprintf_ms(f, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_printf(const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = vfprintf_ms(&iob[1], fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_vsprintf(char *b, const char *fmt, ms_va_list ap) { return ms_vformat(b, (size_t)INT_MAX, fmt, ap, 0); }

static int WINAPI c_sprintf(char *b, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformat(b, (size_t)INT_MAX, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

/* the legacy _snprintf: no NUL when it just fits, -1 when it does not */
static int legacy_snprintf(char *b, size_t cap, const char *fmt, ms_va_list ap) {
    char small[FMT_STACK], *out;
    int n = vformat_alloc(small, &out, fmt, ap, 0);
    if ((size_t)n <= cap) {
        memcpy(b, out, (size_t)n);
        if ((size_t)n < cap) b[n] = 0;
    } else {
        memcpy(b, out, cap);
        n = -1;
    }
    if (out != small) free(out);
    return n;
}

static int WINAPI c__vsnprintf(char *b, size_t cap, const char *fmt, ms_va_list ap) { return legacy_snprintf(b, cap, fmt, ap); }

static int WINAPI c__snprintf(char *b, size_t cap, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = legacy_snprintf(b, cap, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_vsnprintf(char *b, size_t cap, const char *fmt, ms_va_list ap) { return ms_vformat(b, cap, fmt, ap, 0); }

static int WINAPI c_snprintf(char *b, size_t cap, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformat(b, cap, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c__vscprintf(const char *fmt, ms_va_list ap) { return ms_vformat(0, 0, fmt, ap, 0); }

static int WINAPI c__scprintf(const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformat(0, 0, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_sprintf_s(char *b, size_t cap, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformat(b, cap, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    if ((size_t)n >= cap) {
        if (cap) b[0] = 0;
        return -1;
    }
    return n;
}

static int WINAPI c_vsprintf_s(char *b, size_t cap, const char *fmt, ms_va_list ap) {
    int n = ms_vformat(b, cap, fmt, ap, 0);
    if ((size_t)n >= cap) {
        if (cap) b[0] = 0;
        return -1;
    }
    return n;
}

static int WINAPI c__vsnprintf_s(char *b, size_t cap, size_t count, const char *fmt, ms_va_list ap) {
    size_t lim = count < cap ? count + 1 : cap;
    int n = ms_vformat(b, lim, fmt, ap, 0);
    return (size_t)n >= lim ? -1 : n;
}

/* wide */
static int vfwprintf_ms(void *f, const WCHAR *fmt, ms_va_list ap) {
    char *a = w_to_utf8_dup(fmt), small[FMT_STACK], *out;
    int n = vformat_alloc(small, &out, a, ap, 1);
    fwrite(out, 1, (size_t)n, F(f));
    free(a);
    n = utf8_to_w(out, n, 0, 0) - 1;
    if (out != small) free(out);
    return n;
}

static int WINAPI c_vfwprintf(void *f, const WCHAR *fmt, ms_va_list ap) { return vfwprintf_ms(f, fmt, ap); }
static int WINAPI c_fwprintf(void *f, const WCHAR *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = vfwprintf_ms(f, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static int WINAPI c_wprintf(const WCHAR *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = vfwprintf_ms(&iob[1], fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static int WINAPI c_vwprintf(const WCHAR *fmt, ms_va_list ap) { return vfwprintf_ms(&iob[1], fmt, ap); }

static int WINAPI c__vsnwprintf(WCHAR *b, size_t cap, const WCHAR *fmt, ms_va_list ap) {
    int n = ms_vformatw(0, 0, fmt, ap);
    if ((size_t)n <= cap) {
        WCHAR *tmp = malloc(((size_t)n + 1) * 2);
        ms_vformatw(tmp, (size_t)n + 1, fmt, ap);
        memcpy(b, tmp, (size_t)n * 2);
        if ((size_t)n < cap) b[n] = 0;
        free(tmp);
        return n;
    }
    ms_vformatw(b, cap, fmt, ap);   /* cap-1 and a NUL; Windows leaves no NUL: close enough */
    return -1;
}

static int WINAPI c__snwprintf(WCHAR *b, size_t cap, const WCHAR *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = c__vsnwprintf(b, cap, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_vswprintf(WCHAR *b, size_t cap, const WCHAR *fmt, ms_va_list ap) {
    int n = ms_vformatw(b, cap, fmt, ap);
    return (size_t)n >= cap ? -1 : n;
}

static int WINAPI c_swprintf(WCHAR *b, size_t cap, const WCHAR *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = c_vswprintf(b, cap, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c__vscwprintf(const WCHAR *fmt, ms_va_list ap) { return ms_vformatw(0, 0, fmt, ap); }

/* scanf */
static int WINAPI c_sscanf(const char *in, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vscan_ex(in, 0, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_sscanf_s(const char *in, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vscan_ex(in, 0, fmt, ap, 1);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_fscanf(void *f, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vscan_ex(0, F(f), fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_scanf(const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vscan_ex(0, stdin, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_vsscanf(const char *in, const char *fmt, ms_va_list ap) { return ms_vscan_ex(in, 0, fmt, ap, 0); }

/* the UCRT's one entry point for each family */
#define PF_LEGACY_NUL   1ULL
#define PF_STANDARD     2ULL
#define PF_LEGACY_WIDE  4ULL

static int WINAPI c___stdio_common_vfprintf(uint64_t opt, void *f, const char *fmt, void *loc, ms_va_list ap) {
    (void)opt, (void)loc;
    return vfprintf_ms(f, fmt, ap);
}

static int WINAPI c___stdio_common_vfprintf_s(uint64_t opt, void *f, const char *fmt, void *loc, ms_va_list ap) {
    (void)opt, (void)loc;
    return vfprintf_ms(f, fmt, ap);
}

static int WINAPI c___stdio_common_vsprintf(uint64_t opt, char *b, size_t cap, const char *fmt, void *loc, ms_va_list ap) {
    (void)loc;
    if (!b && !cap) return ms_vformat(0, 0, fmt, ap, 0);
    if (opt & PF_STANDARD) return ms_vformat(b, cap, fmt, ap, 0);
    if (opt & PF_LEGACY_NUL) return legacy_snprintf(b, cap, fmt, ap);
    {
        int n = ms_vformat(b, cap, fmt, ap, 0);
        return (size_t)n >= cap ? -1 : n;
    }
}

static int WINAPI c___stdio_common_vsprintf_s(uint64_t opt, char *b, size_t cap, const char *fmt, void *loc, ms_va_list ap) {
    int n;
    (void)opt, (void)loc;
    n = ms_vformat(b, cap, fmt, ap, 0);
    if ((size_t)n >= cap) {
        if (cap) b[0] = 0;
        return -1;
    }
    return n;
}

static int WINAPI c___stdio_common_vsnprintf_s(uint64_t opt, char *b, size_t cap, size_t count, const char *fmt, void *loc,
                                               ms_va_list ap) {
    (void)opt, (void)loc;
    return c__vsnprintf_s(b, cap, count, fmt, ap);
}

/* wide: %s is wide in the legacy mode (the default), narrow in the ISO one */
static int vformatw_opt(WCHAR *out, size_t cap, const WCHAR *fmt, ms_va_list ap, int legacy) {
    char *f = w_to_utf8_dup(fmt), small[FMT_STACK], *buf = small;
    int n, wn;
    n = ms_vformat(small, sizeof(small), f, ap, legacy);
    if (n >= (int)sizeof(small)) {
        buf = malloc((size_t)n + 1);
        ms_vformat(buf, (size_t)n + 1, f, ap, legacy);
    }
    free(f);
    wn = utf8_to_w(buf, n, 0, 0) - 1;
    if (out && cap) {
        WCHAR *tmp = malloc(((size_t)wn + 1) * 2);
        utf8_to_w(buf, n, tmp, wn + 1);
        memcpy(out, tmp, ((size_t)wn < cap ? (size_t)wn : cap - 1) * 2);
        out[(size_t)wn < cap ? (size_t)wn : cap - 1] = 0;
        free(tmp);
    }
    if (buf != small) free(buf);
    return wn;
}

static int WINAPI c___stdio_common_vfwprintf(uint64_t opt, void *f, const WCHAR *fmt, void *loc, ms_va_list ap) {
    char *a = w_to_utf8_dup(fmt), small[FMT_STACK], *out;
    int n;
    (void)loc;
    n = ms_vformat(small, sizeof(small), a, ap, (opt & PF_LEGACY_WIDE) != 0);
    out = small;
    if (n >= (int)sizeof(small)) {
        out = malloc((size_t)n + 1);
        ms_vformat(out, (size_t)n + 1, a, ap, (opt & PF_LEGACY_WIDE) != 0);
    }
    fwrite(out, 1, (size_t)n, F(f));
    n = utf8_to_w(out, n, 0, 0) - 1;
    if (out != small) free(out);
    free(a);
    return n;
}

static int WINAPI c___stdio_common_vswprintf(uint64_t opt, WCHAR *b, size_t cap, const WCHAR *fmt, void *loc, ms_va_list ap) {
    int n;
    (void)loc;
    if (!b && !cap) return vformatw_opt(0, 0, fmt, ap, (opt & PF_LEGACY_WIDE) != 0);
    n = vformatw_opt(b, cap, fmt, ap, (opt & PF_LEGACY_WIDE) != 0);
    if (opt & PF_STANDARD) return n;
    return (size_t)n >= cap ? -1 : n;
}

static int WINAPI c___stdio_common_vswprintf_s(uint64_t opt, WCHAR *b, size_t cap, const WCHAR *fmt, void *loc, ms_va_list ap) {
    int n;
    (void)loc;
    n = vformatw_opt(b, cap, fmt, ap, (opt & PF_LEGACY_WIDE) != 0);
    if ((size_t)n >= cap) {
        if (cap) b[0] = 0;
        return -1;
    }
    return n;
}

static int WINAPI c___stdio_common_vsnwprintf_s(uint64_t opt, WCHAR *b, size_t cap, size_t count, const WCHAR *fmt, void *loc,
                                                ms_va_list ap) {
    size_t lim = count < cap ? count + 1 : cap;
    int n;
    (void)loc;
    n = vformatw_opt(b, lim, fmt, ap, (opt & PF_LEGACY_WIDE) != 0);
    return (size_t)n >= lim ? -1 : n;
}

static int WINAPI c___stdio_common_vsscanf(uint64_t opt, const char *in, size_t n, const char *fmt, void *loc, ms_va_list ap) {
    (void)loc;
    if (n != (size_t)-1 && n < strlen(in)) {
        char *t = strndup(in, n);
        int r = ms_vscan_ex(t, 0, fmt, ap, (opt & 1) != 0);
        free(t);
        return r;
    }
    return ms_vscan_ex(in, 0, fmt, ap, (opt & 1) != 0);
}

static int WINAPI c___stdio_common_vfscanf(uint64_t opt, void *f, const char *fmt, void *loc, ms_va_list ap) {
    (void)loc;
    return ms_vscan_ex(0, F(f), fmt, ap, (opt & 1) != 0);
}

/* ---- low-level files ------------------------------------------------------------------------- */

static int open_flags(int wf) {
    int f = O_CLOEXEC;
    switch (wf & 3) {
    case 0: f |= O_RDONLY; break;
    case 1: f |= O_WRONLY; break;
    default: f |= O_RDWR; break;
    }
    if (wf & 0x8) f |= O_APPEND;
    if (wf & 0x100) f |= O_CREAT;
    if (wf & 0x200) f |= O_TRUNC;
    if (wf & 0x400) f |= O_EXCL;
    return f;
}

static int c_open_path(const char *p, int wf, int mode) {
    char up[1024];
    if (path_find(p, up, sizeof(up)) != 0) {
        errno = ENOENT;
        return -1;
    }
    return open(up, open_flags(wf), mode & 0200 ? 0644 : 0444);
}

static int WINAPI c__open(const char *p, int wf, ...) {
    ms_va_list ap;
    int mode;
    __builtin_ms_va_start(ap, wf);
    mode = (wf & 0x100) ? *(int *)ap : 0600;
    __builtin_ms_va_end(ap);
    return c_open_path(p, wf, mode);
}

static int WINAPI c__wopen(const WCHAR *p, int wf, ...) {
    char a[1024];
    ms_va_list ap;
    int mode;
    __builtin_ms_va_start(ap, wf);
    mode = (wf & 0x100) ? *(int *)ap : 0600;
    __builtin_ms_va_end(ap);
    w_to_utf8(p, -1, a, sizeof(a));
    return c_open_path(a, wf, mode);
}

static int WINAPI c__sopen_s(int *fd, const char *p, int wf, int sh, int mode) {
    (void)sh;
    *fd = c_open_path(p, wf, mode);
    return *fd < 0 ? errno : 0;
}

static int WINAPI c__wsopen_s(int *fd, const WCHAR *p, int wf, int sh, int mode) {
    char a[1024];
    (void)sh;
    w_to_utf8(p, -1, a, sizeof(a));
    *fd = c_open_path(a, wf, mode);
    return *fd < 0 ? errno : 0;
}

static int WINAPI c__creat(const char *p, int mode) { return c_open_path(p, 0x301, mode); }
static int WINAPI c__read(int fd, void *b, unsigned n) { return (int)read(fd, b, n); }
static int WINAPI c__write(int fd, const void *b, unsigned n) { return (int)write(fd, b, n); }
static int WINAPI c__close(int fd) { return close(fd); }
static LONG WINAPI c__lseek(int fd, LONG off, int wh) { return (LONG)lseek(fd, off, wh); }
static int64_t WINAPI c__lseeki64(int fd, int64_t off, int wh) { return lseek(fd, off, wh); }
static LONG WINAPI c__tell(int fd) { return (LONG)lseek(fd, 0, SEEK_CUR); }
static int64_t WINAPI c__telli64(int fd) { return lseek(fd, 0, SEEK_CUR); }
static int WINAPI c__dup(int fd) { return dup(fd); }
static int WINAPI c__dup2(int a, int b) { return dup2(a, b) < 0 ? -1 : 0; }
static int WINAPI c__commit(int fd) { return fsync(fd); }
static int WINAPI c__chsize(int fd, LONG n) { return ftruncate(fd, n); }
static int WINAPI c__chsize_s(int fd, int64_t n) { return ftruncate(fd, n) ? errno : 0; }
static int WINAPI c__eof(int fd) {
    off_t c = lseek(fd, 0, SEEK_CUR), e = lseek(fd, 0, SEEK_END);
    lseek(fd, c, SEEK_SET);
    return c >= e;
}
static LONG WINAPI c__filelength(int fd) {
    struct stat st;
    return fstat(fd, &st) ? -1 : (LONG)st.st_size;
}
static int64_t WINAPI c__filelengthi64(int fd) {
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}
static intptr_t WINAPI c__get_osfhandle(int fd) {
    if (fd >= 0 && fd <= 2) return (intptr_t)h_std(fd);
    return (intptr_t)file_from_fd(fd);
}
static int WINAPI c__open_osfhandle(intptr_t h, int flags) {
    int fd = file_fd((HANDLE)h);
    (void)flags;
    return fd < 0 ? -1 : dup(fd);
}
static int WINAPI c__locking(int fd, int mode, LONG n) {
    (void)fd, (void)mode, (void)n;
    return 0;
}
static int WINAPI c__umask(int m) { return (int)umask((mode_t)m); }

static int path_call(const char *p, int (*fn)(const char *)) {
    char up[1024];
    if (path_find(p, up, sizeof(up)) != 0) {
        errno = ENOENT;
        return -1;
    }
    return fn(up);
}

static int wpath_call(const WCHAR *p, int (*fn)(const char *)) {
    char a[1024];
    w_to_utf8(p, -1, a, sizeof(a));
    return path_call(a, fn);
}

static int do_mkdir(const char *p) { return mkdir(p, 0755); }
static int WINAPI c__mkdir(const char *p) { return path_call(p, do_mkdir); }
static int WINAPI c__wmkdir(const WCHAR *p) { return wpath_call(p, do_mkdir); }
static int WINAPI c__rmdir(const char *p) { return path_call(p, rmdir); }
static int WINAPI c__wrmdir(const WCHAR *p) { return wpath_call(p, rmdir); }
static int WINAPI c__unlink(const char *p) { return path_call(p, unlink); }
static int WINAPI c__wunlink(const WCHAR *p) { return wpath_call(p, unlink); }
static int WINAPI c_remove(const char *p) { return path_call(p, remove); }
static int WINAPI c__wremove(const WCHAR *p) { return wpath_call(p, remove); }
static int WINAPI c__chdir(const char *p) { return path_call(p, chdir); }
static int WINAPI c__wchdir(const WCHAR *p) { return wpath_call(p, chdir); }

static int WINAPI c_rename(const char *a, const char *b) {
    char ua[1024], ub[1024];
    if (path_find(a, ua, sizeof(ua)) != 0 || path_find(b, ub, sizeof(ub)) != 0) {
        errno = ENOENT;
        return -1;
    }
    return rename(ua, ub);
}

static int WINAPI c__wrename(const WCHAR *a, const WCHAR *b) {
    char x[1024], y[1024];
    w_to_utf8(a, -1, x, sizeof(x));
    w_to_utf8(b, -1, y, sizeof(y));
    return c_rename(x, y);
}

static int WINAPI c__access(const char *p, int mode) {
    char up[1024];
    int m = F_OK;
    if (mode & 2) m |= W_OK;
    if (mode & 4) m |= R_OK;
    if (path_find(p, up, sizeof(up)) != 0) {
        errno = ENOENT;
        return -1;
    }
    return access(up, m);
}

static int WINAPI c__waccess(const WCHAR *p, int mode) {
    char a[1024];
    w_to_utf8(p, -1, a, sizeof(a));
    return c__access(a, mode);
}

static int WINAPI c__access_s(const char *p, int mode) { return c__access(p, mode) ? errno : 0; }
static int WINAPI c__waccess_s(const WCHAR *p, int mode) { return c__waccess(p, mode) ? errno : 0; }

static int WINAPI c__chmod(const char *p, int mode) {
    (void)p, (void)mode;
    return 0;
}
static int WINAPI c__wchmod(const WCHAR *p, int mode) {
    (void)p, (void)mode;
    return 0;
}

static char *WINAPI c__getcwd(char *b, int n) {
    char cwd[1024], wp[1100];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    path_to_win(cwd, wp, sizeof(wp));
    if (!b) return strdup(wp);
    if ((int)strlen(wp) + 1 > n) {
        errno = ERANGE;
        return 0;
    }
    strcpy(b, wp);
    return b;
}

static WCHAR *WINAPI c__wgetcwd(WCHAR *b, int n) {
    char tmp[1100];
    if (!c__getcwd(tmp, sizeof(tmp))) return 0;
    if (!b) return utf8_to_w_dup(tmp);
    if (utf8_to_w(tmp, -1, 0, 0) > n) {
        errno = ERANGE;
        return 0;
    }
    utf8_to_w(tmp, -1, b, n);
    return b;
}

static int WINAPI c__getdrive(void) {
    char cwd[1024], wp[1100];
    if (!getcwd(cwd, sizeof(cwd))) return 3;
    path_to_win(cwd, wp, sizeof(wp));
    return (wp[0] | 32) - 'a' + 1;
}

static char *WINAPI c__fullpath(char *out, const char *in, size_t n) {
    DWORD WINAPI GetFullPathNameA(const char *in, DWORD n, char *buf, char **filepart);
    char tmp[2048];
    DWORD r = GetFullPathNameA(in, sizeof(tmp), tmp, 0);
    if (!r || r >= sizeof(tmp)) return 0;
    if (!out) return strdup(tmp);
    if (r + 1 > n) return 0;
    memcpy(out, tmp, r + 1);
    return out;
}

static WCHAR *WINAPI c__wfullpath(WCHAR *out, const WCHAR *in, size_t n) {
    char a[1024], b[2048];
    w_to_utf8(in, -1, a, sizeof(a));
    if (!c__fullpath(b, a, sizeof(b))) return 0;
    if (!out) return utf8_to_w_dup(b);
    utf8_to_w(b, -1, out, (int)n);
    return out;
}

static void WINAPI c__splitpath(const char *path, char *drive, char *dir, char *fname, char *ext) {
    const char *p = path, *last_slash = 0, *dot = 0;
    if (drive) drive[0] = 0;
    if (path[0] && path[1] == ':') {
        if (drive) drive[0] = path[0], drive[1] = ':', drive[2] = 0;
        p = path + 2;
    }
    for (const char *q = p; *q; q++) {
        if (*q == '\\' || *q == '/') last_slash = q, dot = 0;
        else if (*q == '.') dot = q;
    }
    if (dir) {
        size_t n = last_slash ? (size_t)(last_slash - p + 1) : 0;
        memcpy(dir, p, n);
        dir[n] = 0;
    }
    p = last_slash ? last_slash + 1 : p;
    if (fname) {
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        memcpy(fname, p, n);
        fname[n] = 0;
    }
    if (ext) strcpy(ext, dot ? dot : "");
}

static void WINAPI c__makepath(char *out, const char *drive, const char *dir, const char *fname, const char *ext) {
    out[0] = 0;
    if (drive && *drive) {
        out[0] = drive[0], out[1] = ':', out[2] = 0;
    }
    if (dir && *dir) {
        strcat(out, dir);
        if (dir[strlen(dir) - 1] != '\\' && dir[strlen(dir) - 1] != '/') strcat(out, "\\");
    }
    if (fname) strcat(out, fname);
    if (ext && *ext) {
        if (*ext != '.') strcat(out, ".");
        strcat(out, ext);
    }
}

/* stat, in the layouts Windows has: 64-bit times always (x64), 32- or
 * 64-bit sizes */
static void fill_stat(const struct stat *st, uint8_t *o, int size64, int time32) {
    unsigned short mode = 0;
    if (S_ISDIR(st->st_mode)) mode |= 0x4000;
    else if (S_ISCHR(st->st_mode)) mode |= 0x2000;
    else if (S_ISFIFO(st->st_mode)) mode |= 0x1000;
    else mode |= 0x8000;
    if (st->st_mode & 0444) mode |= 0x100 | 0x20 | 0x4;
    if (st->st_mode & 0222) mode |= 0x80 | 0x10 | 0x2;
    if (S_ISDIR(st->st_mode) || (st->st_mode & 0111)) mode |= 0x40 | 0x8 | 0x1;
    *(uint32_t *)(o + 0) = 2;      /* drive C: */
    *(uint16_t *)(o + 4) = 0;
    *(uint16_t *)(o + 6) = mode;
    *(int16_t *)(o + 8) = (int16_t)st->st_nlink;
    *(int16_t *)(o + 10) = 0;
    *(int16_t *)(o + 12) = 0;
    *(uint16_t *)(o + 14) = 0;
    *(uint32_t *)(o + 16) = 2;
    if (time32) {
        *(int32_t *)(o + 20) = (int32_t)st->st_size;
        *(int32_t *)(o + 24) = (int32_t)st->st_atime;
        *(int32_t *)(o + 28) = (int32_t)st->st_mtime;
        *(int32_t *)(o + 32) = (int32_t)st->st_ctime;
    } else if (size64) {
        *(uint32_t *)(o + 20) = 0;
        *(int64_t *)(o + 24) = st->st_size;
        *(int64_t *)(o + 32) = st->st_atime;
        *(int64_t *)(o + 40) = st->st_mtime;
        *(int64_t *)(o + 48) = st->st_ctime;
    } else {
        *(int32_t *)(o + 20) = (int32_t)st->st_size;
        *(int64_t *)(o + 24) = st->st_atime;
        *(int64_t *)(o + 32) = st->st_mtime;
        *(int64_t *)(o + 40) = st->st_ctime;
    }
}

static int stat_path(const char *p, void *out, int size64, int time32) {
    char up[1024];
    struct stat st;
    size_t n = strlen(p);
    char tmp[1024];
    /* "dir\" is the folder itself */
    if (n > 1 && n < sizeof(tmp) && (p[n - 1] == '\\' || p[n - 1] == '/') && p[n - 2] != ':') {
        memcpy(tmp, p, n - 1);
        tmp[n - 1] = 0;
        p = tmp;
    }
    if (path_find(p, up, sizeof(up)) != 0 || stat(up, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    fill_stat(&st, out, size64, time32);
    return 0;
}

static int wstat_path(const WCHAR *p, void *out, int size64, int time32) {
    char a[1024];
    w_to_utf8(p, -1, a, sizeof(a));
    return stat_path(a, out, size64, time32);
}

static int fstat_fd(int fd, void *out, int size64, int time32) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    fill_stat(&st, out, size64, time32);
    return 0;
}

static int WINAPI c__stat64(const char *p, void *o) { return stat_path(p, o, 1, 0); }
static int WINAPI c__wstat64(const WCHAR *p, void *o) { return wstat_path(p, o, 1, 0); }
static int WINAPI c__fstat64(int fd, void *o) { return fstat_fd(fd, o, 1, 0); }
static int WINAPI c__stat64i32(const char *p, void *o) { return stat_path(p, o, 0, 0); }
static int WINAPI c__wstat64i32(const WCHAR *p, void *o) { return wstat_path(p, o, 0, 0); }
static int WINAPI c__fstat64i32(int fd, void *o) { return fstat_fd(fd, o, 0, 0); }
static int WINAPI c__stat32(const char *p, void *o) { return stat_path(p, o, 0, 1); }
static int WINAPI c__wstat32(const WCHAR *p, void *o) { return wstat_path(p, o, 0, 1); }
static int WINAPI c__fstat32(int fd, void *o) { return fstat_fd(fd, o, 0, 1); }

/* _findfirst: _finddata64i32_t (msvcrt's _findfirst) and __finddata64_t */
typedef struct {
    DIR  *d;
    char  dir[1024];
    char  pat[256];
} cfind_t;

static int find_fill(cfind_t *f, uint8_t *o, int size64, int wide) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        char full[1400];
        struct stat st;
        unsigned attrib;
        if (strcmp(f->pat, "*") && strcmp(f->pat, "*.*") && fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        snprintf(full, sizeof(full), "%s/%s", f->dir, e->d_name);
        if (stat(full, &st) != 0) memset(&st, 0, sizeof(st));
        attrib = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        if (!(st.st_mode & 0222)) attrib |= 1;
        *(uint32_t *)o = attrib;
        *(int64_t *)(o + 8) = st.st_ctime;
        *(int64_t *)(o + 16) = st.st_atime;
        *(int64_t *)(o + 24) = st.st_mtime;
        if (size64) {
            *(int64_t *)(o + 32) = st.st_size;
            if (wide) utf8_to_w(e->d_name, -1, (WCHAR *)(o + 40), 260);
            else snprintf((char *)(o + 40), 260, "%s", e->d_name);
        } else {
            *(uint32_t *)(o + 32) = (uint32_t)st.st_size;
            if (wide) utf8_to_w(e->d_name, -1, (WCHAR *)(o + 36), 260);
            else snprintf((char *)(o + 36), 260, "%s", e->d_name);
        }
        return 0;
    }
    return -1;
}

static intptr_t find_first(const char *spec, void *o, int size64, int wide) {
    cfind_t *f = calloc(1, sizeof(*f));
    char wp[1024], up[1024];
    char *ws, *fs, *sep;
    snprintf(wp, sizeof(wp), "%s", spec);
    ws = strrchr(wp, '\\'), fs = strrchr(wp, '/');
    sep = ws > fs ? ws : fs;
    if (sep) {
        snprintf(f->pat, sizeof(f->pat), "%s", sep + 1);
        *sep = 0;
        if (!wp[0]) snprintf(wp, sizeof(wp), "\\");
    } else {
        snprintf(f->pat, sizeof(f->pat), "%s", wp);
        snprintf(wp, sizeof(wp), ".");
    }
    if (path_find(wp, up, sizeof(up)) != 0) snprintf(up, sizeof(up), ".");
    snprintf(f->dir, sizeof(f->dir), "%s", up);
    f->d = opendir(f->dir);
    if (!f->d || find_fill(f, o, size64, wide) != 0) {
        if (f->d) closedir(f->d);
        free(f);
        errno = ENOENT;
        return -1;
    }
    return (intptr_t)f;
}

static intptr_t WINAPI c__findfirst64(const char *s, void *o) { return find_first(s, o, 1, 0); }
static intptr_t WINAPI c__findfirst64i32(const char *s, void *o) { return find_first(s, o, 0, 0); }
static int WINAPI c__findnext64(intptr_t h, void *o) { return find_fill((cfind_t *)h, o, 1, 0); }
static int WINAPI c__findnext64i32(intptr_t h, void *o) { return find_fill((cfind_t *)h, o, 0, 0); }
static intptr_t WINAPI c__wfindfirst64(const WCHAR *s, void *o) {
    char a[1024];
    w_to_utf8(s, -1, a, sizeof(a));
    return find_first(a, o, 1, 1);
}
static intptr_t WINAPI c__wfindfirst64i32(const WCHAR *s, void *o) {
    char a[1024];
    w_to_utf8(s, -1, a, sizeof(a));
    return find_first(a, o, 0, 1);
}
static int WINAPI c__wfindnext64(intptr_t h, void *o) { return find_fill((cfind_t *)h, o, 1, 1); }
static int WINAPI c__wfindnext64i32(intptr_t h, void *o) { return find_fill((cfind_t *)h, o, 0, 1); }
static int WINAPI c__findclose(intptr_t h) {
    cfind_t *f = (cfind_t *)h;
    if (!f || h == -1) return -1;
    closedir(f->d);
    free(f);
    return 0;
}

/* ---- memory --------------------------------------------------------------------------------- */

static void *WINAPI c_malloc(size_t n) { return malloc(n); }
static void WINAPI c_free(void *p) { free(p); }
static void *WINAPI c_calloc(size_t a, size_t b) { return calloc(a, b); }
static void *WINAPI c_realloc(void *p, size_t n) {
    if (!n && p) {
        free(p);
        return 0;
    }
    return realloc(p, n);
}
static size_t WINAPI c__msize(void *p) { return malloc_usable_size(p); }
static void *WINAPI c__expand(void *p, size_t n) { return n <= malloc_usable_size(p) ? p : 0; }
static void *WINAPI c__recalloc(void *p, size_t a, size_t b) {
    size_t old = p ? malloc_usable_size(p) : 0, n = a * b;
    void *q = realloc(p, n ? n : 1);
    if (q && n > old) memset((uint8_t *)q + old, 0, n - old);
    return q;
}

/* aligned blocks: a header before the block holds the start */
static void *WINAPI c__aligned_malloc(size_t n, size_t align) {
    void *p = 0;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (posix_memalign(&p, align, n ? n : 1) != 0) return 0;
    return p;
}
static void WINAPI c__aligned_free(void *p) { free(p); }
static void *WINAPI c__aligned_realloc(void *p, size_t n, size_t align) {
    void *q;
    size_t old;
    if (!p) return c__aligned_malloc(n, align);
    if (!n) {
        free(p);
        return 0;
    }
    old = malloc_usable_size(p);
    q = c__aligned_malloc(n, align);
    if (q) memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}
static void *WINAPI c__aligned_offset_malloc(size_t n, size_t align, size_t off) {
    (void)off;
    return c__aligned_malloc(n, align);
}
static size_t WINAPI c__aligned_msize(void *p, size_t a, size_t o) {
    (void)a, (void)o;
    return malloc_usable_size(p);
}

static int new_mode;
static int WINAPI c__set_new_mode(int m) {
    int o = new_mode;
    new_mode = m;
    return o;
}
static int WINAPI c__query_new_mode(void) { return new_mode; }
static int WINAPI c__callnewh(size_t n) {
    (void)n;
    return 0;
}
static int WINAPI c__heapchk(void) { return -2; /* _HEAPOK */ }
static int WINAPI c__heapmin(void) { return 0; }

/* ---- strings -------------------------------------------------------------------------------- */

static void *WINAPI c_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *WINAPI c_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *WINAPI c_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int WINAPI c_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *WINAPI c_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
static int WINAPI c_memcpy_s(void *d, size_t dn, const void *s, size_t n) {
    if (n > dn) {
        if (d) memset(d, 0, dn);
        return ERANGE;
    }
    memcpy(d, s, n);
    return 0;
}
static int WINAPI c_memmove_s(void *d, size_t dn, const void *s, size_t n) {
    if (n > dn) return ERANGE;
    memmove(d, s, n);
    return 0;
}
static int WINAPI c__memicmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        int c = tolower(x[i]) - tolower(y[i]);
        if (c) return c;
    }
    return 0;
}
static size_t WINAPI c_strlen(const char *s) { return strlen(s); }
static size_t WINAPI c_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static char *WINAPI c_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *WINAPI c_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *WINAPI c_strcat(char *d, const char *s) { return strcat(d, s); }
static char *WINAPI c_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static int WINAPI c_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int WINAPI c_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static int WINAPI c_strcoll(const char *a, const char *b) { return strcmp(a, b); }
static size_t WINAPI c_strxfrm(char *d, const char *s, size_t n) { return strxfrm(d, s, n); }
static char *WINAPI c_strchr(const char *s, int c) { return strchr(s, c); }
static char *WINAPI c_strrchr(const char *s, int c) { return strrchr(s, c); }
static char *WINAPI c_strstr(const char *a, const char *b) { return strstr(a, b); }
static size_t WINAPI c_strspn(const char *a, const char *b) { return strspn(a, b); }
static size_t WINAPI c_strcspn(const char *a, const char *b) { return strcspn(a, b); }
static char *WINAPI c_strpbrk(const char *a, const char *b) { return strpbrk(a, b); }
static char *WINAPI c_strtok(char *s, const char *d) { return strtok(s, d); }
static char *WINAPI c_strtok_s(char *s, const char *d, char **ctx) { return strtok_r(s, d, ctx); }
static char *WINAPI c__strdup(const char *s) { return s ? strdup(s) : 0; }
static int WINAPI c__stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static int WINAPI c__strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static int WINAPI c__stricoll(const char *a, const char *b) { return strcasecmp(a, b); }
static char *WINAPI c__strlwr(char *s) {
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}
static char *WINAPI c__strupr(char *s) {
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    return s;
}
static int WINAPI c__strlwr_s(char *s, size_t n) {
    (void)n;
    c__strlwr(s);
    return 0;
}
static int WINAPI c__strupr_s(char *s, size_t n) {
    (void)n;
    c__strupr(s);
    return 0;
}
static char *WINAPI c__strrev(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}
static char *WINAPI c__strset(char *s, int c) {
    for (char *p = s; *p; p++) *p = (char)c;
    return s;
}
static char *WINAPI c__strnset(char *s, int c, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) s[i] = (char)c;
    return s;
}
static int WINAPI c_strcpy_s(char *d, size_t n, const char *s) {
    size_t l = strlen(s);
    if (!d || !n) return EINVAL;
    if (l + 1 > n) {
        d[0] = 0;
        return ERANGE;
    }
    memcpy(d, s, l + 1);
    return 0;
}
static int WINAPI c_strncpy_s(char *d, size_t n, const char *s, size_t count) {
    size_t l = strnlen(s, count == (size_t)-1 ? n : count);
    if (!d || !n) return EINVAL;
    if (l + 1 > n) {
        if (count == (size_t)-1) {     /* _TRUNCATE */
            memcpy(d, s, n - 1);
            d[n - 1] = 0;
            return 80;                 /* STRUNCATE */
        }
        d[0] = 0;
        return ERANGE;
    }
    memcpy(d, s, l);
    d[l] = 0;
    return 0;
}
static int WINAPI c_strcat_s(char *d, size_t n, const char *s) {
    size_t a = strnlen(d, n), b = strlen(s);
    if (a + b + 1 > n) {
        if (n) d[0] = 0;
        return ERANGE;
    }
    memcpy(d + a, s, b + 1);
    return 0;
}
static int WINAPI c_strncat_s(char *d, size_t n, const char *s, size_t count) {
    size_t a = strnlen(d, n), b = strnlen(s, count == (size_t)-1 ? n : count);
    if (a + b + 1 > n) {
        if (count == (size_t)-1 && a < n) {
            memcpy(d + a, s, n - a - 1);
            d[n - 1] = 0;
            return 80;
        }
        if (n) d[0] = 0;
        return ERANGE;
    }
    memcpy(d + a, s, b);
    d[a + b] = 0;
    return 0;
}
static char *WINAPI c_strerror(int e) { return strerror(e); }
static int WINAPI c_strerror_s(char *b, size_t n, int e) {
    snprintf(b, n, "%s", strerror(e));
    return 0;
}
static char *WINAPI c__strerror(const char *s) {
    static __thread char buf[512];
    if (s) snprintf(buf, sizeof(buf), "%s: %s\n", s, strerror(errno));
    else snprintf(buf, sizeof(buf), "%s\n", strerror(errno));
    return buf;
}

/* ---- wide strings (16-bit) ------------------------------------------------------------------- */

typedef uint16_t wint16;

static size_t WINAPI c_wcslen(const WCHAR *s) { return w_len(s); }
static size_t WINAPI c_wcsnlen(const WCHAR *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}
static WCHAR *WINAPI c_wcscpy(WCHAR *d, const WCHAR *s) {
    memmove(d, s, (w_len(s) + 1) * 2);
    return d;
}
static WCHAR *WINAPI c_wcsncpy(WCHAR *d, const WCHAR *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static WCHAR *WINAPI c_wcscat(WCHAR *d, const WCHAR *s) {
    c_wcscpy(d + w_len(d), s);
    return d;
}
static WCHAR *WINAPI c_wcsncat(WCHAR *d, const WCHAR *s, size_t n) {
    WCHAR *e = d + w_len(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
static int WINAPI c_wcscmp(const WCHAR *a, const WCHAR *b) {
    int r = w_cmp(a, b);
    return r < 0 ? -1 : r > 0;
}
static int WINAPI c_wcsncmp(const WCHAR *a, const WCHAR *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}
static int WINAPI c__wcsicmp(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) {
        WCHAR x = win_towlower(*a), y = win_towlower(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static int WINAPI c__wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        WCHAR x = win_towlower(a[i]), y = win_towlower(b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static WCHAR *WINAPI c_wcschr(const WCHAR *s, WCHAR c) {
    for (;; s++) {
        if (*s == c) return (WCHAR *)s;
        if (!*s) return 0;
    }
}
static WCHAR *WINAPI c_wcsrchr(const WCHAR *s, WCHAR c) {
    const WCHAR *r = 0;
    for (;; s++) {
        if (*s == c) r = s;
        if (!*s) return (WCHAR *)r;
    }
}
static WCHAR *WINAPI c_wcsstr(const WCHAR *h, const WCHAR *n) {
    size_t nl = w_len(n);
    if (!nl) return (WCHAR *)h;
    for (; *h; h++)
        if (*h == *n && !memcmp(h, n, nl * 2)) return (WCHAR *)h;
    return 0;
}
static WCHAR *WINAPI c_wcspbrk(const WCHAR *s, const WCHAR *set) {
    for (; *s; s++)
        for (const WCHAR *q = set; *q; q++)
            if (*s == *q) return (WCHAR *)s;
    return 0;
}
static size_t WINAPI c_wcsspn(const WCHAR *s, const WCHAR *set) {
    size_t i = 0;
    for (; s[i]; i++) {
        const WCHAR *q = set;
        while (*q && *q != s[i]) q++;
        if (!*q) break;
    }
    return i;
}
static size_t WINAPI c_wcscspn(const WCHAR *s, const WCHAR *set) {
    size_t i = 0;
    for (; s[i]; i++)
        for (const WCHAR *q = set; *q; q++)
            if (*q == s[i]) return i;
    return i;
}
static WCHAR *WINAPI c_wcstok(WCHAR *s, const WCHAR *d, WCHAR **ctx) {
    static __thread WCHAR *last;
    WCHAR **c = ctx ? ctx : &last;
    if (!s) s = *c;
    if (!s) return 0;
    s += c_wcsspn(s, d);
    if (!*s) {
        *c = 0;
        return 0;
    }
    {
        WCHAR *e = s + c_wcscspn(s, d);
        if (*e) *e++ = 0;
        else e = 0;
        *c = e;
    }
    return s;
}
static WCHAR *WINAPI c__wcsdup(const WCHAR *s) { return s ? w_dup(s) : 0; }
static WCHAR *WINAPI c__wcslwr(WCHAR *s) {
    for (WCHAR *p = s; *p; p++) *p = win_towlower(*p);
    return s;
}
static WCHAR *WINAPI c__wcsupr(WCHAR *s) {
    for (WCHAR *p = s; *p; p++) *p = win_towupper(*p);
    return s;
}
static int WINAPI c__wcslwr_s(WCHAR *s, size_t n) {
    (void)n;
    c__wcslwr(s);
    return 0;
}
static int WINAPI c__wcsupr_s(WCHAR *s, size_t n) {
    (void)n;
    c__wcsupr(s);
    return 0;
}
static int WINAPI c_wcscpy_s(WCHAR *d, size_t n, const WCHAR *s) {
    size_t l = w_len(s);
    if (!d || !n) return EINVAL;
    if (l + 1 > n) {
        d[0] = 0;
        return ERANGE;
    }
    memmove(d, s, (l + 1) * 2);
    return 0;
}
static int WINAPI c_wcsncpy_s(WCHAR *d, size_t n, const WCHAR *s, size_t count) {
    size_t l = c_wcsnlen(s, count == (size_t)-1 ? n : count);
    if (!d || !n) return EINVAL;
    if (l + 1 > n) {
        if (count == (size_t)-1) {
            memcpy(d, s, (n - 1) * 2);
            d[n - 1] = 0;
            return 80;
        }
        d[0] = 0;
        return ERANGE;
    }
    memcpy(d, s, l * 2);
    d[l] = 0;
    return 0;
}
static int WINAPI c_wcscat_s(WCHAR *d, size_t n, const WCHAR *s) {
    size_t a = c_wcsnlen(d, n), b = w_len(s);
    if (a + b + 1 > n) {
        if (n) d[0] = 0;
        return ERANGE;
    }
    memcpy(d + a, s, (b + 1) * 2);
    return 0;
}
static int WINAPI c_wcsncat_s(WCHAR *d, size_t n, const WCHAR *s, size_t count) {
    size_t a = c_wcsnlen(d, n), b = c_wcsnlen(s, count == (size_t)-1 ? n : count);
    if (a + b + 1 > n) {
        if (count == (size_t)-1 && a < n) {
            memcpy(d + a, s, (n - a - 1) * 2);
            d[n - 1] = 0;
            return 80;
        }
        if (n) d[0] = 0;
        return ERANGE;
    }
    memcpy(d + a, s, b * 2);
    d[a + b] = 0;
    return 0;
}

static long long wcs_to_ll(const WCHAR *s, WCHAR **end, int base, int is_unsigned) {
    char tmp[128], *e;
    size_t i = 0;
    long long v;
    while (i < sizeof(tmp) - 1 && s[i] && s[i] < 128) tmp[i] = (char)s[i], i++;
    tmp[i] = 0;
    errno = 0;
    v = is_unsigned ? (long long)strtoull(tmp, &e, base) : strtoll(tmp, &e, base);
    if (end) *end = (WCHAR *)s + (e - tmp);
    return v;
}

static LONG clamp32(long long v) {
    if (v > INT32_MAX) {
        errno = ERANGE;
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        errno = ERANGE;
        return INT32_MIN;
    }
    return (LONG)v;
}

static ULONG clampu32(long long v, const char *s) {
    /* "-1" is ULONG_MAX, as in C */
    (void)s;
    if (v > (long long)UINT32_MAX || v < -(long long)UINT32_MAX) {
        errno = ERANGE;
        return UINT32_MAX;
    }
    return (ULONG)v;
}

static LONG WINAPI c_wcstol(const WCHAR *s, WCHAR **e, int b) { return clamp32(wcs_to_ll(s, e, b, 0)); }
static ULONG WINAPI c_wcstoul(const WCHAR *s, WCHAR **e, int b) { return clampu32(wcs_to_ll(s, e, b, 0), 0); }
static long long WINAPI c_wcstoll(const WCHAR *s, WCHAR **e, int b) { return wcs_to_ll(s, e, b, 0); }
static unsigned long long WINAPI c_wcstoull(const WCHAR *s, WCHAR **e, int b) { return (unsigned long long)wcs_to_ll(s, e, b, 1); }
static double WINAPI c_wcstod(const WCHAR *s, WCHAR **end) {
    char tmp[256], *e;
    size_t i = 0;
    double v;
    while (i < sizeof(tmp) - 1 && s[i] && s[i] < 128) tmp[i] = (char)s[i], i++;
    tmp[i] = 0;
    v = strtod(tmp, &e);
    if (end) *end = (WCHAR *)s + (e - tmp);
    return v;
}
static float WINAPI c_wcstof(const WCHAR *s, WCHAR **e) { return (float)c_wcstod(s, e); }
static int WINAPI c__wtoi(const WCHAR *s) { return (int)wcs_to_ll(s, 0, 10, 0); }
static LONG WINAPI c__wtol(const WCHAR *s) { return clamp32(wcs_to_ll(s, 0, 10, 0)); }
static long long WINAPI c__wtoi64(const WCHAR *s) { return wcs_to_ll(s, 0, 10, 0); }
static double WINAPI c__wtof(const WCHAR *s) { return c_wcstod(s, 0); }

static unsigned short WINAPI c_towlower(unsigned short c) { return win_towlower(c); }
static unsigned short WINAPI c_towupper(unsigned short c) { return win_towupper(c); }
static int WINAPI c_iswalpha(unsigned short c) { return (c < 128 && isalpha(c)) || (c >= 0xC0 && c != 0xD7 && c != 0xF7); }
static int WINAPI c_iswdigit(unsigned short c) { return c >= '0' && c <= '9'; }
static int WINAPI c_iswxdigit(unsigned short c) { return c < 128 && isxdigit(c); }
static int WINAPI c_iswspace(unsigned short c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x3000; }
static int WINAPI c_iswalnum(unsigned short c) { return c_iswalpha(c) || c_iswdigit(c); }
static int WINAPI c_iswupper(unsigned short c) { return c_iswalpha(c) && win_towlower(c) != c; }
static int WINAPI c_iswlower(unsigned short c) { return c_iswalpha(c) && win_towupper(c) != c; }
static int WINAPI c_iswprint(unsigned short c) { return c >= 32 && c != 127; }
static int WINAPI c_iswgraph(unsigned short c) { return c > 32 && c != 127; }
static int WINAPI c_iswcntrl(unsigned short c) { return c < 32 || c == 127; }
static int WINAPI c_iswpunct(unsigned short c) { return c < 128 && ispunct(c); }
static int WINAPI c_iswascii(unsigned short c) { return c < 128; }
static int WINAPI c_iswctype(unsigned short c, unsigned short t) {
    int r = 0;
    if ((t & 0x1) && c_iswupper(c)) r |= 0x1;
    if ((t & 0x2) && c_iswlower(c)) r |= 0x2;
    if ((t & 0x4) && c_iswdigit(c)) r |= 0x4;
    if ((t & 0x8) && c_iswspace(c)) r |= 0x8;
    if ((t & 0x10) && c_iswpunct(c)) r |= 0x10;
    if ((t & 0x20) && c_iswcntrl(c)) r |= 0x20;
    if ((t & 0x40) && (c == ' ' || c == '\t')) r |= 0x40;
    if ((t & 0x80) && c_iswxdigit(c)) r |= 0x80;
    if ((t & 0x100) && c_iswalpha(c)) r |= 0x100;
    return r;
}
static int WINAPI c_wcscoll(const WCHAR *a, const WCHAR *b) { return c_wcscmp(a, b); }
static int WINAPI c__wcsicoll(const WCHAR *a, const WCHAR *b) { return c__wcsicmp(a, b); }

/* multibyte is UTF-8 */
static size_t WINAPI c_wcstombs(char *d, const WCHAR *s, size_t n) {
    int need = w_to_utf8(s, -1, 0, 0) - 1;
    if (!d) return (size_t)need;
    {
        char *tmp = malloc((size_t)need + 1);
        size_t k;
        w_to_utf8(s, -1, tmp, need + 1);
        k = (size_t)need < n ? (size_t)need : n;
        memcpy(d, tmp, k);
        if (k < n) d[k] = 0;
        free(tmp);
        return k;
    }
}
static size_t WINAPI c_mbstowcs(WCHAR *d, const char *s, size_t n) {
    int need = utf8_to_w(s, -1, 0, 0) - 1;
    if (!d) return (size_t)need;
    {
        WCHAR *tmp = malloc(((size_t)need + 1) * 2);
        size_t k;
        utf8_to_w(s, -1, tmp, need + 1);
        k = (size_t)need < n ? (size_t)need : n;
        memcpy(d, tmp, k * 2);
        if (k < n) d[k] = 0;
        free(tmp);
        return k;
    }
}
static int WINAPI c_wcstombs_s(size_t *ret, char *d, size_t dn, const WCHAR *s, size_t n) {
    size_t r;
    (void)n;
    if (!d) {
        if (ret) *ret = c_wcstombs(0, s, 0) + 1;
        return 0;
    }
    r = c_wcstombs(d, s, dn - 1);
    d[r < dn ? r : dn - 1] = 0;
    if (ret) *ret = r + 1;
    return 0;
}
static int WINAPI c_mbstowcs_s(size_t *ret, WCHAR *d, size_t dn, const char *s, size_t n) {
    size_t r;
    (void)n;
    if (!d) {
        if (ret) *ret = c_mbstowcs(0, s, 0) + 1;
        return 0;
    }
    r = c_mbstowcs(d, s, dn - 1);
    d[r < dn ? r : dn - 1] = 0;
    if (ret) *ret = r + 1;
    return 0;
}
static int WINAPI c_mblen(const char *s, size_t n) {
    if (!s || !*s) return 0;
    if ((unsigned char)*s < 0x80) return 1;
    return n >= 2 ? ((unsigned char)*s >= 0xF0 ? 4 : (unsigned char)*s >= 0xE0 ? 3 : 2) : -1;
}
static int WINAPI c_mbtowc(WCHAR *d, const char *s, size_t n) {
    int l;
    WCHAR w[4];
    if (!s) return 0;
    l = c_mblen(s, n);
    if (l <= 0) {
        if (d) *d = 0;
        return l;
    }
    utf8_to_w(s, l, w, 4);
    if (d) *d = w[0];
    return l;
}
static int WINAPI c_wctomb(char *d, WCHAR c) {
    char b[8];
    int n = w_to_utf8(&c, 1, b, sizeof(b)) - 1;
    if (d) memcpy(d, b, (size_t)n);
    return n;
}
static size_t WINAPI c_mbrtowc(WCHAR *d, const char *s, size_t n, void *st) {
    (void)st;
    if (!s) return 0;
    {
        int l = c_mbtowc(d, s, n);
        return l < 0 ? (size_t)-1 : (size_t)l;
    }
}
static size_t WINAPI c_wcrtomb(char *d, WCHAR c, void *st) {
    (void)st;
    if (!d) return 1;
    return (size_t)c_wctomb(d, c);
}
static unsigned short WINAPI c_btowc(int c) { return c == EOF ? 0xFFFF : (unsigned short)(c & 0xFF); }
static int WINAPI c_wctob(unsigned short c) { return c < 128 ? c : EOF; }
static size_t WINAPI c_mbrlen(const char *s, size_t n, void *st) {
    (void)st;
    return (size_t)c_mblen(s, n);
}
static int WINAPI c_mbsinit(const void *st) {
    (void)st;
    return 1;
}

/* ---- ctype ------------------------------------------------------------------------------------ */

static unsigned short ctype_table[257];

static void ctype_init(void) {
    for (int c = 0; c < 256; c++) {
        unsigned short t = 0;
        if (c < 128) {
            if (isupper(c)) t |= 0x1;
            if (islower(c)) t |= 0x2;
            if (isdigit(c)) t |= 0x4;
            if (isspace(c)) t |= 0x8;
            if (ispunct(c)) t |= 0x10;
            if (iscntrl(c)) t |= 0x20;
            if (c == ' ' || c == '\t') t |= 0x40;
            if (isxdigit(c)) t |= 0x80;
            if (isalpha(c)) t |= 0x100;
        }
        ctype_table[c + 1] = t;
    }
}

static const unsigned short *pctype = ctype_table + 1;
static const unsigned short *WINAPI c___pctype_func(void) { return ctype_table + 1; }
static int WINAPI c__isctype(int c, int mask) { return (c >= -1 && c < 256) ? (ctype_table[c + 1] & mask) : 0; }
static int WINAPI c__isctype_l(int c, int mask, void *l) {
    (void)l;
    return c__isctype(c, mask);
}
static int WINAPI c_isalpha(int c) { return c >= 0 && c < 128 && isalpha(c); }
static int WINAPI c_isdigit(int c) { return c >= '0' && c <= '9'; }
static int WINAPI c_isalnum(int c) { return c >= 0 && c < 128 && isalnum(c); }
static int WINAPI c_isspace(int c) { return c >= 0 && c < 128 && isspace(c); }
static int WINAPI c_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int WINAPI c_islower(int c) { return c >= 'a' && c <= 'z'; }
static int WINAPI c_isxdigit(int c) { return c >= 0 && c < 128 && isxdigit(c); }
static int WINAPI c_isprint(int c) { return c >= 32 && c < 127; }
static int WINAPI c_isgraph(int c) { return c > 32 && c < 127; }
static int WINAPI c_iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
static int WINAPI c_ispunct(int c) { return c >= 0 && c < 128 && ispunct(c); }
static int WINAPI c_isblank(int c) { return c == ' ' || c == '\t'; }
static int WINAPI c_isascii(int c) { return (unsigned)c < 128; }
static int WINAPI c___iscsym(int c) { return c_isalnum(c) || c == '_'; }
static int WINAPI c___iscsymf(int c) { return c_isalpha(c) || c == '_'; }
static int WINAPI c_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int WINAPI c_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int WINAPI c__toupper(int c) { return c - 32; }
static int WINAPI c__tolower(int c) { return c + 32; }

/* ---- numbers ---------------------------------------------------------------------------------- */

static int WINAPI c_atoi(const char *s) { return atoi(s); }
static LONG WINAPI c_atol(const char *s) { return clamp32(atoll(s)); }
static long long WINAPI c_atoll(const char *s) { return atoll(s); }
static double WINAPI c_atof(const char *s) { return atof(s); }
static LONG WINAPI c_strtol(const char *s, char **e, int b) {
    errno = 0;
    return clamp32(strtoll(s, e, b));
}
static ULONG WINAPI c_strtoul(const char *s, char **e, int b) {
    errno = 0;
    return clampu32(strtoll(s, e, b), s);
}
static long long WINAPI c_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
static unsigned long long WINAPI c_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
static double WINAPI c_strtod(const char *s, char **e) { return strtod(s, e); }
static float WINAPI c_strtof(const char *s, char **e) { return strtof(s, e); }
static double WINAPI c_strtold(const char *s, char **e) { return strtod(s, e); }
static double WINAPI c__strtod_l(const char *s, char **e, void *l) {
    (void)l;
    return strtod(s, e);
}
static long long WINAPI c__strtoi64_l(const char *s, char **e, int b, void *l) {
    (void)l;
    return strtoll(s, e, b);
}
static unsigned long long WINAPI c__strtoui64_l(const char *s, char **e, int b, void *l) {
    (void)l;
    return strtoull(s, e, b);
}
static LONG WINAPI c__strtol_l(const char *s, char **e, int b, void *l) {
    (void)l;
    return c_strtol(s, e, b);
}
static double WINAPI c__atof_l(const char *s, void *l) {
    (void)l;
    return atof(s);
}

static char *utoa_base(unsigned long long v, char *out, int base, int neg) {
    char tmp[72];
    int n = 0, o = 0;
    if (base < 2 || base > 36) base = 10;
    do {
        int d = (int)(v % (unsigned)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (unsigned)base;
    } while (v);
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    return out;
}

static char *WINAPI c__itoa(int v, char *o, int b) { return utoa_base(v < 0 && b == 10 ? (unsigned)-(long long)v : (unsigned)v, o, b, v < 0 && b == 10); }
static char *WINAPI c__ltoa(LONG v, char *o, int b) { return c__itoa(v, o, b); }
static char *WINAPI c__ultoa(ULONG v, char *o, int b) { return utoa_base(v, o, b, 0); }
static char *WINAPI c__i64toa(long long v, char *o, int b) {
    return utoa_base(v < 0 && b == 10 ? (unsigned long long)-v : (unsigned long long)v, o, b, v < 0 && b == 10);
}
static char *WINAPI c__ui64toa(unsigned long long v, char *o, int b) { return utoa_base(v, o, b, 0); }
static int WINAPI c__itoa_s(int v, char *o, size_t n, int b) {
    char t[72];
    c__itoa(v, t, b);
    return c_strcpy_s(o, n, t);
}
static int WINAPI c__i64toa_s(long long v, char *o, size_t n, int b) {
    char t[72];
    c__i64toa(v, t, b);
    return c_strcpy_s(o, n, t);
}
static int WINAPI c__ui64toa_s(unsigned long long v, char *o, size_t n, int b) {
    char t[72];
    c__ui64toa(v, t, b);
    return c_strcpy_s(o, n, t);
}
static WCHAR *WINAPI c__itow(int v, WCHAR *o, int b) {
    char t[72];
    c__itoa(v, t, b);
    utf8_to_w(t, -1, o, 72);
    return o;
}
static WCHAR *WINAPI c__i64tow(long long v, WCHAR *o, int b) {
    char t[72];
    c__i64toa(v, t, b);
    utf8_to_w(t, -1, o, 72);
    return o;
}
static WCHAR *WINAPI c__ultow(ULONG v, WCHAR *o, int b) {
    char t[72];
    c__ultoa(v, t, b);
    utf8_to_w(t, -1, o, 72);
    return o;
}
static WCHAR *WINAPI c__ui64tow(unsigned long long v, WCHAR *o, int b) {
    char t[72];
    c__ui64toa(v, t, b);
    utf8_to_w(t, -1, o, 72);
    return o;
}
static char *WINAPI c__gcvt(double v, int digits, char *buf) {
    sprintf(buf, "%.*g", digits, v);
    return buf;
}
static char *WINAPI c__ecvt(double v, int digits, int *dec, int *sign) { return ecvt(v, digits, dec, sign); }
static char *WINAPI c__fcvt(double v, int digits, int *dec, int *sign) { return fcvt(v, digits, dec, sign); }

static int WINAPI c_abs(int v) { return v < 0 ? -v : v; }
static LONG WINAPI c_labs(LONG v) { return v < 0 ? -v : v; }
static long long WINAPI c_llabs(long long v) { return v < 0 ? -v : v; }
typedef struct { int q, r; } div_ms_t;
typedef struct { long long q, r; } lldiv_ms_t;
static div_ms_t WINAPI c_div(int a, int b) {
    div_ms_t r = { a / b, a % b };
    return r;
}
static lldiv_ms_t WINAPI c_lldiv(long long a, long long b) {
    lldiv_ms_t r = { a / b, a % b };
    return r;
}
static unsigned WINAPI c__rotl(unsigned v, int s) { return (v << (s & 31)) | (v >> ((32 - s) & 31)); }
static unsigned WINAPI c__rotr(unsigned v, int s) { return (v >> (s & 31)) | (v << ((32 - s) & 31)); }
static unsigned long long WINAPI c__rotl64(unsigned long long v, int s) { return (v << (s & 63)) | (v >> ((64 - s) & 63)); }
static unsigned long long WINAPI c__rotr64(unsigned long long v, int s) { return (v >> (s & 63)) | (v << ((64 - s) & 63)); }
static unsigned short WINAPI c__byteswap_ushort(unsigned short v) { return (unsigned short)(v << 8 | v >> 8); }
static ULONG WINAPI c__byteswap_ulong(ULONG v) { return __builtin_bswap32(v); }
static unsigned long long WINAPI c__byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }

/* random numbers: Windows's rand() is 15 bits, per thread */
static __thread unsigned rand_state = 1;
static int WINAPI c_rand(void) {
    rand_state = rand_state * 214013u + 2531011u;
    return (int)((rand_state >> 16) & 0x7FFF);
}
static void WINAPI c_srand(unsigned s) { rand_state = s; }
static int WINAPI c_rand_s(unsigned *v) {
    if (getrandom(v, sizeof(*v), 0) != sizeof(*v)) *v = (unsigned)mono_ns();
    return 0;
}

/* qsort and bsearch call back the Windows way */
typedef int (WCALLBACK *cmp_t)(const void *, const void *);
typedef int (WCALLBACK *cmp_s_t)(void *, const void *, const void *);
static __thread cmp_t cur_cmp;
static __thread cmp_s_t cur_cmp_s;
static __thread void *cur_ctx;
static int cmp_tramp(const void *a, const void *b) { return cur_cmp(a, b); }
static int cmp_s_tramp(const void *a, const void *b) { return cur_cmp_s(cur_ctx, a, b); }

static void WINAPI c_qsort(void *base, size_t n, size_t sz, cmp_t cmp) {
    cmp_t saved = cur_cmp;
    cur_cmp = cmp;
    qsort(base, n, sz, cmp_tramp);
    cur_cmp = saved;
}

static void WINAPI c_qsort_s(void *base, size_t n, size_t sz, cmp_s_t cmp, void *ctx) {
    cmp_s_t saved = cur_cmp_s;
    void *sctx = cur_ctx;
    cur_cmp_s = cmp;
    cur_ctx = ctx;
    qsort(base, n, sz, cmp_s_tramp);
    cur_cmp_s = saved;
    cur_ctx = sctx;
}

static void *WINAPI c_bsearch(const void *key, const void *base, size_t n, size_t sz, cmp_t cmp) {
    cmp_t saved = cur_cmp;
    void *r;
    cur_cmp = cmp;
    r = bsearch(key, base, n, sz, cmp_tramp);
    cur_cmp = saved;
    return r;
}

/* ---- math ------------------------------------------------------------------------------------- */

#define M1(n) static double WINAPI c_##n(double x) { return n(x); }
#define M1F(n) static float WINAPI c_##n(float x) { return n(x); }
#define M2(n) static double WINAPI c_##n(double x, double y) { return n(x, y); }
#define M2F(n) static float WINAPI c_##n(float x, float y) { return n(x, y); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(sinh) M1(cosh) M1(tanh) M1(asinh) M1(acosh) M1(atanh)
M1(exp) M1(exp2) M1(expm1) M1(log) M1(log10) M1(log2) M1(log1p) M1(logb) M1(sqrt) M1(cbrt) M1(ceil) M1(floor)
M1(round) M1(trunc) M1(rint) M1(nearbyint) M1(fabs) M1(erf) M1(erfc) M1(tgamma) M1(lgamma)
M2(atan2) M2(pow) M2(fmod) M2(hypot) M2(fmin) M2(fmax) M2(fdim) M2(copysign) M2(nextafter) M2(remainder)
M1F(sinf) M1F(cosf) M1F(tanf) M1F(asinf) M1F(acosf) M1F(atanf) M1F(sinhf) M1F(coshf) M1F(tanhf) M1F(expf)
M1F(exp2f) M1F(logf) M1F(log10f) M1F(log2f) M1F(log1pf) M1F(sqrtf) M1F(cbrtf) M1F(ceilf) M1F(floorf) M1F(roundf)
M1F(truncf) M1F(rintf) M1F(fabsf) M1F(nearbyintf) M1F(expm1f) M1F(logbf)
M2F(atan2f) M2F(powf) M2F(fmodf) M2F(hypotf) M2F(fminf) M2F(fmaxf) M2F(copysignf) M2F(nextafterf) M2F(remainderf)
static double WINAPI c_frexp(double x, int *e) { return frexp(x, e); }
static float WINAPI c_frexpf(float x, int *e) { return frexpf(x, e); }
static double WINAPI c_ldexp(double x, int e) { return ldexp(x, e); }
static float WINAPI c_ldexpf(float x, int e) { return ldexpf(x, e); }
static double WINAPI c_modf(double x, double *i) { return modf(x, i); }
static float WINAPI c_modff(float x, float *i) { return modff(x, i); }
static double WINAPI c_scalbn(double x, int e) { return scalbn(x, e); }
static float WINAPI c_scalbnf(float x, int e) { return scalbnf(x, e); }
static double WINAPI c_fma(double a, double b, double c) { return fma(a, b, c); }
static float WINAPI c_fmaf(float a, float b, float c) { return fmaf(a, b, c); }
static double WINAPI c_remquo(double a, double b, int *q) { return remquo(a, b, q); }
static int WINAPI c_ilogb(double x) { return ilogb(x); }
static LONG WINAPI c_lround(double x) { return (LONG)lround(x); }
static LONG WINAPI c_lroundf(float x) { return (LONG)lroundf(x); }
static LONG WINAPI c_lrint(double x) { return (LONG)lrint(x); }
static LONG WINAPI c_lrintf(float x) { return (LONG)lrintf(x); }
static long long WINAPI c_llround(double x) { return llround(x); }
static long long WINAPI c_llroundf(float x) { return llroundf(x); }
static long long WINAPI c_llrint(double x) { return llrint(x); }
static long long WINAPI c_llrintf(float x) { return llrintf(x); }
static double WINAPI c_nan(const char *s) { return nan(s); }
static float WINAPI c_nanf(const char *s) { return nanf(s); }
static int WINAPI c__isnan(double x) { return isnan(x); }
static int WINAPI c__isnanf(float x) { return isnan(x); }
static int WINAPI c__finite(double x) { return isfinite(x); }
static int WINAPI c__finitef(float x) { return isfinite(x); }
static double WINAPI c__chgsign(double x) { return -x; }
static double WINAPI c__logb(double x) { return logb(x); }
static double WINAPI c__scalb(double x, LONG e) { return scalbn(x, (int)e); }
static int WINAPI c__fpclass(double x) {
    switch (fpclassify(x)) {
    case FP_NAN: return 2;                               /* _FPCLASS_QNAN */
    case FP_INFINITE: return signbit(x) ? 4 : 0x200;     /* NINF / PINF */
    case FP_ZERO: return signbit(x) ? 0x20 : 0x40;
    case FP_SUBNORMAL: return signbit(x) ? 0x10 : 0x80;
    default: return signbit(x) ? 8 : 0x100;
    }
}
/* the UCRT's fpclassify and signbit */
static short WINAPI c__dclass(double x) {
    switch (fpclassify(x)) {
    case FP_NAN: return 2;
    case FP_INFINITE: return 1;
    case FP_ZERO: return 0;
    case FP_SUBNORMAL: return -2;
    default: return -1;
    }
}
static short WINAPI c__fdclass(float x) { return c__dclass(x); }
static short WINAPI c__ldclass(double x) { return c__dclass(x); }
static int WINAPI c__dsign(double x) { return signbit(x) ? 0x8000 : 0; }
static int WINAPI c__fdsign(float x) { return signbit(x) ? 0x8000 : 0; }
static int WINAPI c__ldsign(double x) { return signbit(x) ? 0x8000 : 0; }
static short WINAPI c__dtest(double *x) { return c__dclass(*x); }
static short WINAPI c__fdtest(float *x) { return c__dclass(*x); }
static double hugeval = HUGE_VAL;

static unsigned WINAPI c__controlfp(unsigned nw, unsigned mask) {
    (void)nw, (void)mask;
    return 0x9001F;    /* _CW_DEFAULT: all exceptions masked, near, 53 bits */
}
static int WINAPI c__controlfp_s(unsigned *cur, unsigned nw, unsigned mask) {
    if (cur) *cur = c__controlfp(nw, mask);
    return 0;
}
static unsigned WINAPI c__control87(unsigned nw, unsigned mask) { return c__controlfp(nw, mask); }
static unsigned WINAPI c__clearfp(void) {
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}
static unsigned WINAPI c__statusfp(void) { return 0; }
static void WINAPI c__fpreset(void) { feclearexcept(FE_ALL_EXCEPT); }
static int WINAPI c__set_SSE2_enable(int f) { return f; }
static int WINAPI c_fegetround(void) {
    int r = fegetround();
    return r == FE_TONEAREST ? 0 : r == FE_DOWNWARD ? 0x100 : r == FE_UPWARD ? 0x200 : 0x300;
}
static int WINAPI c_fesetround(int r) {
    return fesetround(r == 0 ? FE_TONEAREST : r == 0x100 ? FE_DOWNWARD : r == 0x200 ? FE_UPWARD : FE_TOWARDZERO);
}
static int WINAPI c_feclearexcept(int e) {
    (void)e;
    return feclearexcept(FE_ALL_EXCEPT);
}
static int WINAPI c_fetestexcept(int e) {
    (void)e;
    return 0;
}
static int WINAPI c_fegetenv(void *e) {
    memset(e, 0, 8);
    return 0;
}
static int WINAPI c_fesetenv(const void *e) {
    (void)e;
    return 0;
}
static int WINAPI c_feholdexcept(void *e) {
    memset(e, 0, 8);
    return 0;
}
static int WINAPI c_feupdateenv(const void *e) {
    (void)e;
    return 0;
}
static void WINAPI c___setusermatherr(void *f) { (void)f; }

/* ---- time ------------------------------------------------------------------------------------ */

/* a Windows struct tm: nine ints and nothing else */
typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; } wtm_t;

static void tm_out(const struct tm *t, wtm_t *o) { memcpy(o, t, sizeof(*o)); }
static void tm_in(const wtm_t *w, struct tm *t) {
    memset(t, 0, sizeof(*t));
    memcpy(t, w, sizeof(*w));
}

static int64_t WINAPI c__time64(int64_t *t) {
    int64_t v = time(0);
    if (t) *t = v;
    return v;
}
static int32_t WINAPI c__time32(int32_t *t) {
    int32_t v = (int32_t)time(0);
    if (t) *t = v;
    return v;
}
static LONG WINAPI c_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (LONG)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);   /* CLOCKS_PER_SEC 1000, since start-up on Windows */
}
static double WINAPI c__difftime64(int64_t a, int64_t b) { return (double)(a - b); }
static double WINAPI c__difftime32(int32_t a, int32_t b) { return (double)(a - b); }

static __thread wtm_t tm_buf;
static wtm_t *WINAPI c__gmtime64(const int64_t *t) {
    struct tm r;
    time_t v = (time_t)*t;
    if (!gmtime_r(&v, &r)) return 0;
    tm_out(&r, &tm_buf);
    return &tm_buf;
}
static wtm_t *WINAPI c__localtime64(const int64_t *t) {
    struct tm r;
    time_t v = (time_t)*t;
    if (!localtime_r(&v, &r)) return 0;
    tm_out(&r, &tm_buf);
    return &tm_buf;
}
static wtm_t *WINAPI c__gmtime32(const int32_t *t) {
    int64_t v = *t;
    return c__gmtime64(&v);
}
static wtm_t *WINAPI c__localtime32(const int32_t *t) {
    int64_t v = *t;
    return c__localtime64(&v);
}
static int WINAPI c__gmtime64_s(wtm_t *o, const int64_t *t) {
    wtm_t *r = c__gmtime64(t);
    if (!r) return EINVAL;
    *o = *r;
    return 0;
}
static int WINAPI c__localtime64_s(wtm_t *o, const int64_t *t) {
    wtm_t *r = c__localtime64(t);
    if (!r) return EINVAL;
    *o = *r;
    return 0;
}
static int64_t WINAPI c__mktime64(wtm_t *w) {
    struct tm t;
    int64_t r;
    tm_in(w, &t);
    r = mktime(&t);
    tm_out(&t, w);
    return r;
}
static int32_t WINAPI c__mktime32(wtm_t *w) { return (int32_t)c__mktime64(w); }
static int64_t WINAPI c__mkgmtime64(wtm_t *w) {
    struct tm t;
    int64_t r;
    tm_in(w, &t);
    r = timegm(&t);
    tm_out(&t, w);
    return r;
}
static size_t WINAPI c_strftime(char *b, size_t n, const char *fmt, const wtm_t *w) {
    struct tm t;
    tm_in(w, &t);
    return strftime(b, n, fmt, &t);
}
static size_t WINAPI c__strftime_l(char *b, size_t n, const char *fmt, const wtm_t *w, void *l) {
    (void)l;
    return c_strftime(b, n, fmt, w);
}
static size_t WINAPI c_wcsftime(WCHAR *b, size_t n, const WCHAR *fmt, const wtm_t *w) {
    char f[256], out[512];
    struct tm t;
    size_t r;
    w_to_utf8(fmt, -1, f, sizeof(f));
    tm_in(w, &t);
    r = strftime(out, sizeof(out), f, &t);
    if (!r) return 0;
    if ((size_t)utf8_to_w(out, -1, 0, 0) > n) return 0;
    return (size_t)utf8_to_w(out, -1, b, (int)n) - 1;
}
static char *WINAPI c_asctime(const wtm_t *w) {
    static __thread char buf[32];
    struct tm t;
    tm_in(w, &t);
    return asctime_r(&t, buf);
}
static char *WINAPI c__ctime64(const int64_t *v) {
    static __thread char buf[32];
    time_t t = (time_t)*v;
    return ctime_r(&t, buf);
}
static void WINAPI c__tzset(void) { tzset(); }
static int WINAPI c__get_timezone(LONG *s) {
    *s = (LONG)timezone;
    return 0;
}
static int WINAPI c__get_daylight(int *d) {
    *d = daylight;
    return 0;
}
static int WINAPI c__get_dstbias(LONG *s) {
    *s = -3600;
    return 0;
}
static LONG win_timezone;
static int win_daylight;
static char *win_tzname[2] = { "UTC", "UTC" };
static LONG *WINAPI c___timezone(void) { return &win_timezone; }
static int *WINAPI c___daylight(void) { return &win_daylight; }
static char **WINAPI c___tzname(void) { return win_tzname; }

typedef struct { int64_t time; uint16_t millitm; int16_t timezone, dstflag; } timeb64_t;
static void WINAPI c__ftime64(timeb64_t *t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    t->time = ts.tv_sec;
    t->millitm = (uint16_t)(ts.tv_nsec / 1000000);
    t->timezone = 0;
    t->dstflag = 0;
}
static int WINAPI c__ftime64_s(timeb64_t *t) {
    c__ftime64(t);
    return 0;
}
static int WINAPI c__timespec64_get(struct timespec *ts, int base) {
    clock_gettime(CLOCK_REALTIME, ts);
    return base;
}
static char *WINAPI c__strdate(char *b) {
    time_t t = time(0);
    struct tm r;
    localtime_r(&t, &r);
    strftime(b, 9, "%m/%d/%y", &r);
    return b;
}
static char *WINAPI c__strtime(char *b) {
    time_t t = time(0);
    struct tm r;
    localtime_r(&t, &r);
    strftime(b, 9, "%H:%M:%S", &r);
    return b;
}

/* ---- the process: start-up, exit, errno, environment ----------------------------------------- */

typedef struct { vfn_t *first, *last, *end; } onexit_table_t;

static onexit_table_t atexit_table;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;

static int WINAPI c__initialize_onexit_table(onexit_table_t *t) {
    if (!t) return -1;
    if (t->first == t->end && t->first) return 0;
    t->first = t->last = t->end = 0;
    return 0;
}

static int WINAPI c__register_onexit_function(onexit_table_t *t, vfn_t fn) {
    pthread_mutex_lock(&exit_lock);
    if (t->last == t->end) {
        size_t n = (size_t)(t->end - t->first), nn = n ? n * 2 : 32;
        vfn_t *p = realloc(t->first, nn * sizeof(vfn_t));
        if (!p) {
            pthread_mutex_unlock(&exit_lock);
            return -1;
        }
        t->last = p + n;
        t->first = p;
        t->end = p + nn;
    }
    *t->last++ = fn;
    pthread_mutex_unlock(&exit_lock);
    return 0;
}

static int WINAPI c__execute_onexit_table(onexit_table_t *t) {
    vfn_t *first, *last;
    pthread_mutex_lock(&exit_lock);
    first = t->first;
    last = t->last;
    t->first = t->last = t->end = 0;
    pthread_mutex_unlock(&exit_lock);
    while (last > first) {
        vfn_t fn = *--last;
        if (fn) fn();
    }
    free(first);
    return 0;
}

static int WINAPI c__crt_atexit(vfn_t fn) { return c__register_onexit_function(&atexit_table, fn); }
static int WINAPI c_atexit(vfn_t fn) { return c__crt_atexit(fn); }
static void *WINAPI c__onexit(void *fn) { return c__crt_atexit((vfn_t)fn) == 0 ? fn : 0; }
static int WINAPI c__crt_at_quick_exit(vfn_t fn) {
    (void)fn;
    return 0;
}

/* msvcrt's DLL form: the DLL keeps its own table, grown here */
static void *WINAPI c___dllonexit(void *fn, vfn_t **begin, vfn_t **end) {
    size_t n = (size_t)(*end - *begin);
    vfn_t *p = realloc(*begin, (n + 1) * sizeof(vfn_t));
    if (!p) return 0;
    p[n] = (vfn_t)fn;
    *begin = p;
    *end = p + n + 1;
    return fn;
}

static void run_atexit(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    c__execute_onexit_table(&atexit_table);
}

void WINAPI ExitProcess(UINT code);
BOOL WINAPI TerminateProcess(HANDLE h, UINT code);

/* as on Windows: the CRT's exit ends in ExitProcess (which tells a parent the whole code) */
static void WINAPI c_exit(int code) {
    run_atexit();
    fflush(0);
    ExitProcess((UINT)code);
}
static void WINAPI c__exit(int code) {
    fflush(0);
    TerminateProcess((HANDLE)(intptr_t)-1, (UINT)code);
}
static void WINAPI c_quick_exit(int code) { c__exit(code); }
static void WINAPI c__cexit(void) {
    run_atexit();
    fflush(0);
}
static void WINAPI c__c_exit(void) { fflush(0); }
static void WINAPI c_abort(void) {
    wnotice("Windows program", "The program stopped itself (abort).");
    fflush(0);
    _exit(3);
}
static void WINAPI c__amsg_exit(int n) {
    wlog("runtime error R60%02d", n);
    c__exit(255);
}
static void WINAPI c_terminate(void) { c_abort(); }
static void WINAPI c__invalid_parameter_noinfo(void) {
    if (wtrace) wlog("invalid parameter given to the C runtime");
}
static void WINAPI c__invalid_parameter_noinfo_noreturn(void) {
    wlog("invalid parameter given to the C runtime");
    c_abort();
}
static void WINAPI c__invalid_parameter(const WCHAR *e, const WCHAR *f, const WCHAR *file, unsigned line, uintptr_t r) {
    (void)e, (void)f, (void)file, (void)line, (void)r;
    c__invalid_parameter_noinfo();
}
static void WINAPI c__invoke_watson(const WCHAR *e, const WCHAR *f, const WCHAR *file, unsigned line, uintptr_t r) {
    (void)e, (void)f, (void)file, (void)line, (void)r;
    c_abort();
}
static void *ipar_handler;
static void *WINAPI c__set_invalid_parameter_handler(void *h) {
    void *o = ipar_handler;
    ipar_handler = h;
    return o;
}
static void *WINAPI c__get_invalid_parameter_handler(void) { return ipar_handler; }
static void *WINAPI c__set_purecall_handler(void *h) { return h; }
static void WINAPI c__purecall(void) {
    wlog("pure virtual function call");
    c_abort();
}
static unsigned WINAPI c__set_abort_behavior(unsigned f, unsigned m) {
    (void)f, (void)m;
    return 0;
}
static int WINAPI c__set_error_mode(int m) {
    (void)m;
    return 0;
}
static void WINAPI c__crt_debugger_hook(int r) { (void)r; }

/* start-up */
static void WINAPI c__initterm(vfn_t *a, vfn_t *b) {
    for (; a < b; a++)
        if (*a) (*a)();
}
static int WINAPI c__initterm_e(ifn_t *a, ifn_t *b) {
    for (; a < b; a++)
        if (*a) {
            int r = (*a)();
            if (r) return r;
        }
    return 0;
}

static int app_type, fmode_value, commode_value;
static void WINAPI c__set_app_type(int t) { app_type = t; }
static int WINAPI c__set_fmode(int m) {
    fmode_value = m;
    return 0;
}
static int WINAPI c__get_fmode(int *m) {
    *m = fmode_value;
    return 0;
}
static int *WINAPI c___p__fmode(void) { return &fmode_value; }
static int *WINAPI c___p__commode(void) { return &commode_value; }

static char **env_a;
static WCHAR **env_w;
static int argc_value;
static char **argv_value;
static WCHAR **wargv_value;
static char *pgmptr;
static WCHAR *wpgmptr;

static int WINAPI c__configure_narrow_argv(int mode) {
    (void)mode;
    argc_value = g_argc;
    argv_value = g_argv_a;
    return 0;
}
static int WINAPI c__configure_wide_argv(int mode) {
    (void)mode;
    argc_value = g_argc;
    wargv_value = g_argv_w;
    return 0;
}
extern char **environ;
static int WINAPI c__initialize_narrow_environment(void) {
    env_a = environ;
    return 0;
}
static int WINAPI c__initialize_wide_environment(void) {
    size_t n = 0;
    while (environ[n]) n++;
    env_w = calloc(n + 1, sizeof(WCHAR *));
    for (size_t i = 0; i < n; i++) env_w[i] = utf8_to_w_dup(environ[i]);
    return 0;
}
static char **WINAPI c__get_initial_narrow_environment(void) { return environ; }
static WCHAR **WINAPI c__get_initial_wide_environment(void) {
    if (!env_w) c__initialize_wide_environment();
    return env_w;
}
static int *WINAPI c___p___argc(void) {
    argc_value = g_argc;
    return &argc_value;
}
static char ***WINAPI c___p___argv(void) {
    argv_value = g_argv_a;
    return &argv_value;
}
static WCHAR ***WINAPI c___p___wargv(void) {
    wargv_value = g_argv_w;
    return &wargv_value;
}
static char ***WINAPI c___p__environ(void) {
    env_a = environ;
    return &env_a;
}
static WCHAR ***WINAPI c___p__wenviron(void) {
    if (!env_w) c__initialize_wide_environment();
    return &env_w;
}
static char **WINAPI c___p__pgmptr(void) {
    pgmptr = g_exe_win;
    return &pgmptr;
}
static WCHAR **WINAPI c___p__wpgmptr(void) {
    if (!wpgmptr) wpgmptr = utf8_to_w_dup(g_exe_win);
    return &wpgmptr;
}
static int WINAPI c__get_pgmptr(char **p) {
    *p = g_exe_win;
    return 0;
}
static int WINAPI c__get_wpgmptr(WCHAR **p) {
    *p = *c___p__wpgmptr();
    return 0;
}
static char *WINAPI c__get_narrow_winmain_command_line(void) {
    /* after the program's name, as WinMain gets it */
    char *p = g_cmdline_a;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
static WCHAR *WINAPI c__get_wide_winmain_command_line(void) {
    WCHAR *p = g_cmdline_w;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
/* msvcrt's form */
static int WINAPI c___getmainargs(int *argc, char ***argv, char ***envp, int glob, void *si) {
    (void)glob, (void)si;
    *argc = g_argc;
    *argv = g_argv_a;
    *envp = environ;
    return 0;
}
static int WINAPI c___wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***envp, int glob, void *si) {
    (void)glob, (void)si;
    *argc = g_argc;
    *argv = g_argv_w;
    *envp = c__get_initial_wide_environment();
    return 0;
}
static void WINAPI c___set_app_type(int t) { app_type = t; }
static int WINAPI c__seh_filter_exe(DWORD code, void *info) {
    (void)info;
    wlog("exception 0x%08X reached the program's start-up", code);
    return 0;      /* EXCEPTION_CONTINUE_SEARCH */
}
static int WINAPI c__seh_filter_dll(DWORD code, void *info) {
    (void)code, (void)info;
    return 0;
}
static void WINAPI c__register_thread_local_exe_atexit_callback(void *cb) { (void)cb; }
static int WINAPI c__configthreadlocale(int f) {
    (void)f;
    return 2;      /* _DISABLE_PER_THREAD_LOCALE */
}

/* errno */
static int *WINAPI c__errno(void) { return &errno; }
static __thread ULONG doserrno;
static ULONG *WINAPI c___doserrno(void) { return &doserrno; }
static int WINAPI c__get_errno(int *v) {
    *v = errno;
    return 0;
}
static int WINAPI c__set_errno(int v) {
    errno = v;
    return 0;
}
static int WINAPI c__get_doserrno(ULONG *v) {
    *v = doserrno;
    return 0;
}
static const char *errlist[44];
static int nerr = 43;
static void errlist_init(void) {
    for (int i = 0; i < 44; i++) errlist[i] = strerror(i);
}
static const char **WINAPI c___sys_errlist(void) { return errlist; }
static int *WINAPI c___sys_nerr(void) { return &nerr; }

/* the environment */
static char *WINAPI c_getenv(const char *n) { return getenv(n); }
static WCHAR *WINAPI c__wgetenv(const WCHAR *n) {
    /* each name keeps its wide copy: programs hold on to the pointer */
    static struct { char name[64]; WCHAR *val; } cache[64];
    static int nc;
    char a[256];
    const char *v;
    w_to_utf8(n, -1, a, sizeof(a));
    v = getenv(a);
    if (!v) return 0;
    for (int i = 0; i < nc; i++)
        if (!strcmp(cache[i].name, a)) {
            free(cache[i].val);
            cache[i].val = utf8_to_w_dup(v);
            return cache[i].val;
        }
    if (nc < 64) {
        snprintf(cache[nc].name, sizeof(cache[0].name), "%s", a);
        cache[nc].val = utf8_to_w_dup(v);
        return cache[nc++].val;
    }
    return utf8_to_w_dup(v);
}
static int WINAPI c__putenv(const char *s) {
    const char *eq = strchr(s, '=');
    char name[256];
    if (!eq || (size_t)(eq - s) >= sizeof(name)) return -1;
    memcpy(name, s, (size_t)(eq - s));
    name[eq - s] = 0;
    if (!eq[1]) return unsetenv(name);
    return setenv(name, eq + 1, 1);
}
static int WINAPI c__wputenv(const WCHAR *s) {
    char *a = w_to_utf8_dup(s);
    int r = c__putenv(a);
    free(a);
    return r;
}
static int WINAPI c__putenv_s(const char *n, const char *v) { return *v ? setenv(n, v, 1) : unsetenv(n); }
static int WINAPI c__wputenv_s(const WCHAR *n, const WCHAR *v) {
    char *a = w_to_utf8_dup(n), *b = w_to_utf8_dup(v);
    int r = c__putenv_s(a, b);
    free(a), free(b);
    return r;
}
static int WINAPI c_getenv_s(size_t *ret, char *b, size_t n, const char *name) {
    const char *v = getenv(name);
    if (!v) {
        *ret = 0;
        if (b && n) b[0] = 0;
        return 0;
    }
    *ret = strlen(v) + 1;
    if (!b) return 0;
    if (*ret > n) return ERANGE;
    memcpy(b, v, *ret);
    return 0;
}
static int WINAPI c__dupenv_s(char **out, size_t *n, const char *name) {
    const char *v = getenv(name);
    *out = v ? strdup(v) : 0;
    if (n) *n = v ? strlen(v) + 1 : 0;
    return 0;
}
static int WINAPI c__wdupenv_s(WCHAR **out, size_t *n, const WCHAR *name) {
    WCHAR *v = c__wgetenv(name);
    *out = v ? w_dup(v) : 0;
    if (n) *n = v ? w_len(v) + 1 : 0;
    return 0;
}
static int WINAPI c_system(const char *cmd) {
    if (!cmd) return 0;
    wlog("system(\"%s\"): not run", cmd);
    return -1;
}
static int WINAPI c__getpid(void) { return getpid(); }

/* signals: kept, never delivered (Windows programs mostly ask for SIGINT) */
static void *sig_handlers[32];
static void *WINAPI c_signal(int s, void *h) {
    void *o;
    if (s < 0 || s >= 32) return (void *)(intptr_t)-1;
    o = sig_handlers[s];
    sig_handlers[s] = h;
    return o;
}
static int WINAPI c_raise(int s) {
    if (s == 22) c_abort();      /* SIGABRT */
    if (s == 2 || s == 15) c_exit(3);
    return 0;
}

/* locks the CRT has inside: one recursive lock is plenty */
static pthread_mutex_t crt_lock;
static void WINAPI c__lock(int n) {
    (void)n;
    pthread_mutex_lock(&crt_lock);
}
static void WINAPI c__unlock(int n) {
    (void)n;
    pthread_mutex_unlock(&crt_lock);
}

/* locale: C, with UTF-8 as the multibyte code page */
typedef struct {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
        *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn,
        n_sign_posn;
    WCHAR *w[10];
} wlconv_t;
static WCHAR wdot[] = u".", wempty[] = u"";
static wlconv_t lconv_c = { ".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127,
                            { wdot, wempty, wempty, wempty, wempty, wempty, wempty, wempty, wempty, wempty } };
static wlconv_t *WINAPI c_localeconv(void) { return &lconv_c; }
static char *WINAPI c_setlocale(int cat, const char *l) {
    (void)cat, (void)l;
    return "C";
}
static WCHAR *WINAPI c__wsetlocale(int cat, const WCHAR *l) {
    static WCHAR c[] = u"C";
    (void)cat, (void)l;
    return c;
}
static void *WINAPI c__create_locale(int cat, const char *l) {
    (void)cat, (void)l;
    return calloc(1, 64);
}
static void *WINAPI c__wcreate_locale(int cat, const WCHAR *l) {
    (void)cat, (void)l;
    return calloc(1, 64);
}
static void WINAPI c__free_locale(void *l) { free(l); }
static void *WINAPI c__get_current_locale(void) { return calloc(1, 64); }
static int mb_cur_max = 4;
static unsigned WINAPI c____lc_codepage_func(void) { return 65001; }
static int WINAPI c____mb_cur_max_func(void) { return mb_cur_max; }
static int WINAPI c____mb_cur_max_l_func(void *l) {
    (void)l;
    return mb_cur_max;
}
static WCHAR **WINAPI c____lc_locale_name_func(void) {
    static WCHAR *names[6];
    return names;
}
static int WINAPI c__getmbcp(void) { return 65001; }
static int WINAPI c__setmbcp(int cp) {
    (void)cp;
    return 0;
}

/* msvcrt's ioinfo array (mingw code reads text-mode flags from it) */
static uint8_t ioinfo_blocks[64 * 32 * 64];
static void *pioinfo[64];
static void pioinfo_init(void) {
    for (int i = 0; i < 64; i++) pioinfo[i] = ioinfo_blocks + (size_t)i * 32 * 64;
}

/* threads */
typedef unsigned (WCALLBACK *beginthreadex_fn)(void *);
typedef void (WCALLBACK *beginthread_fn)(void *);
HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *param, DWORD flags, DWORD *tid);
void WINAPI ExitThread(DWORD code);
static uintptr_t WINAPI c__beginthreadex(void *sa, unsigned stack, beginthreadex_fn fn, void *arg, unsigned flags, unsigned *tid) {
    return (uintptr_t)CreateThread(sa, stack, (LPTHREAD_START_ROUTINE)fn, arg, flags, (DWORD *)tid);
}
static void WINAPI c__endthreadex(unsigned code) { ExitThread(code); }

typedef struct {
    beginthread_fn fn;
    void *arg;
} bt_t;

static DWORD WCALLBACK bt_tramp(void *p) {
    bt_t b = *(bt_t *)p;
    free(p);
    b.fn(b.arg);
    return 0;
}

static uintptr_t WINAPI c__beginthread(beginthread_fn fn, unsigned stack, void *arg) {
    bt_t *b = malloc(sizeof(*b));
    HANDLE h;
    b->fn = fn;
    b->arg = arg;
    h = CreateThread(0, stack, bt_tramp, b, 0, 0);
    return h ? (uintptr_t)h : (uintptr_t)-1;
}
static void WINAPI c__endthread(void) { ExitThread(0); }

/* the console */
static int WINAPI c__kbhit(void) { return 0; }
static int WINAPI c__getch(void) { return getchar(); }
static int WINAPI c__getche(void) { return getchar(); }
static int WINAPI c__putch(int c) { return putchar(c); }
static int WINAPI c__cputs(const char *s) { return fputs(s, stdout); }

/* ---- vcruntime: setjmp, exceptions, type info ------------------------------------------------- */

/* MSVC's _JUMP_BUFFER: Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip,
 * MxCsr, FpCsr, Spare, Xmm6-Xmm15 */
__asm__(".text\n"
        ".globl ms_setjmp\n"
        "ms_setjmp:\n"
        "  mov %rdx, 0(%rcx)\n"
        "  mov %rbx, 8(%rcx)\n"
        "  lea 8(%rsp), %rax\n"
        "  mov %rax, 16(%rcx)\n"
        "  mov %rbp, 24(%rcx)\n"
        "  mov %rsi, 32(%rcx)\n"
        "  mov %rdi, 40(%rcx)\n"
        "  mov %r12, 48(%rcx)\n"
        "  mov %r13, 56(%rcx)\n"
        "  mov %r14, 64(%rcx)\n"
        "  mov %r15, 72(%rcx)\n"
        "  mov (%rsp), %rax\n"
        "  mov %rax, 80(%rcx)\n"
        "  stmxcsr 88(%rcx)\n"
        "  fnstcw 92(%rcx)\n"
        "  movdqu %xmm6, 96(%rcx)\n"
        "  movdqu %xmm7, 112(%rcx)\n"
        "  movdqu %xmm8, 128(%rcx)\n"
        "  movdqu %xmm9, 144(%rcx)\n"
        "  movdqu %xmm10, 160(%rcx)\n"
        "  movdqu %xmm11, 176(%rcx)\n"
        "  movdqu %xmm12, 192(%rcx)\n"
        "  movdqu %xmm13, 208(%rcx)\n"
        "  movdqu %xmm14, 224(%rcx)\n"
        "  movdqu %xmm15, 240(%rcx)\n"
        "  xor %eax, %eax\n"
        "  ret\n"
        ".globl ms_longjmp\n"
        "ms_longjmp:\n"
        "  mov %edx, %eax\n"
        "  test %eax, %eax\n"
        "  jnz 1f\n"
        "  inc %eax\n"
        "1:\n"
        "  mov 8(%rcx), %rbx\n"
        "  mov 24(%rcx), %rbp\n"
        "  mov 32(%rcx), %rsi\n"
        "  mov 40(%rcx), %rdi\n"
        "  mov 48(%rcx), %r12\n"
        "  mov 56(%rcx), %r13\n"
        "  mov 64(%rcx), %r14\n"
        "  mov 72(%rcx), %r15\n"
        "  ldmxcsr 88(%rcx)\n"
        "  fnclex\n"
        "  fldcw 92(%rcx)\n"
        "  movdqu 96(%rcx), %xmm6\n"
        "  movdqu 112(%rcx), %xmm7\n"
        "  movdqu 128(%rcx), %xmm8\n"
        "  movdqu 144(%rcx), %xmm9\n"
        "  movdqu 160(%rcx), %xmm10\n"
        "  movdqu 176(%rcx), %xmm11\n"
        "  movdqu 192(%rcx), %xmm12\n"
        "  movdqu 208(%rcx), %xmm13\n"
        "  movdqu 224(%rcx), %xmm14\n"
        "  movdqu 240(%rcx), %xmm15\n"
        "  mov 16(%rcx), %rsp\n"
        "  jmp *80(%rcx)\n");
extern char ms_setjmp[], ms_longjmp[];

static LONG WINAPI c___C_specific_handler(void *rec, void *frame, void *ctx, void *dc) {
    (void)rec, (void)frame, (void)ctx, (void)dc;
    return 1;     /* ExceptionContinueSearch */
}
static __thread void *cur_exception, *cur_exception_ctx;
static void **WINAPI c___current_exception(void) { return &cur_exception; }
static void **WINAPI c___current_exception_context(void) { return &cur_exception_ctx; }
static void WINAPI c___std_type_info_destroy_list(void *l) { (void)l; }
static const char *WINAPI c___std_type_info_name(void *ti, void *list) {
    (void)list;
    return (const char *)ti + 16 + 1;     /* the decorated name, past the '.' */
}
static int WINAPI c___std_type_info_compare(const void *a, const void *b) {
    if (a == b) return 0;
    return strcmp((const char *)a + 16 + 1, (const char *)b + 16 + 1);
}
static size_t WINAPI c___std_type_info_hash(const void *a) {
    size_t h = 14695981039346656037ull;
    for (const char *p = (const char *)a + 16 + 1; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return h;
}
static void WINAPI c___std_exception_copy(const void *from, void *to) {
    const char *const *f = from;
    char **t = to;
    t[0] = f[0] ? strdup(f[0]) : 0;
    ((uint8_t *)to)[8] = 1;
}
static void WINAPI c___std_exception_destroy(void *e) {
    char **p = e;
    if (((uint8_t *)e)[8]) free(p[0]);
    p[0] = 0;
    ((uint8_t *)e)[8] = 0;
}
static void WINAPI c__CxxThrowException(void *obj, void *info) {
    (void)obj, (void)info;
    wfatal("Windows program", "The program threw a C++ exception that ICDA cannot handle yet.");
}
static LONG WINAPI c___CxxFrameHandler(void *a, void *b, void *c, void *d) {
    (void)a, (void)b, (void)c, (void)d;
    return 1;
}
static int WINAPI c___uncaught_exceptions(void) { return 0; }
static void *WINAPI c__set_se_translator(void *f) { return f; }
static int WINAPI c__is_exception_typeof(void *t, void *e) {
    (void)t, (void)e;
    return 0;
}

static void WINAPI c__wassert(const WCHAR *msg, const WCHAR *file, unsigned line) {
    char m[512], f[512];
    w_to_utf8(msg, -1, m, sizeof(m));
    w_to_utf8(file, -1, f, sizeof(f));
    wfatal("Windows program", "Assertion failed: %s, file %s, line %u", m, f, line);
}
static void WINAPI c__assert(const char *msg, const char *file, unsigned line) {
    wfatal("Windows program", "Assertion failed: %s, file %s, line %u", msg, file, line);
}

/* ---- the tables -------------------------------------------------------------------------------- */

#define C(n) WEXPN(#n, c_##n)

/* what msvcrt.dll and the UCRT share */
#define COMMON_EXPORTS                                                                                                  \
    C(fopen), C(_wfopen), C(fopen_s), C(_wfopen_s), C(_fsopen), C(_wfsopen), C(freopen), C(_wfreopen), C(_fdopen),      \
    C(_wfdopen), C(fclose), C(_fcloseall), C(fread), C(fwrite), C(fgets), C(fputs), C(fgetc), C(fputc), C(ungetc),       \
    C(fflush), C(feof), C(ferror), C(clearerr), C(rewind), C(fseek), C(ftell), C(_fseeki64), C(_ftelli64), C(fgetpos),  \
    C(fsetpos), C(_fileno), C(setvbuf), C(setbuf), C(getc), C(putc), C(getchar), C(putchar), C(puts), C(_lock_file),     \
    C(_unlock_file), C(_fgetc_nolock), C(_fputc_nolock), C(_fread_nolock), C(_fwrite_nolock), C(tmpfile),             \
    C(_flushall), C(perror), C(fputws), C(fgetws), C(fgetwc), C(fputwc), C(ungetwc), C(_setmode), C(_isatty),          \
    WEXPN("_fgetwc_nolock", c_fgetwc), WEXPN("getwc", c_fgetwc), WEXPN("putwc", c_fputwc),                              \
    C(_open), C(_wopen), C(_sopen_s), C(_wsopen_s), C(_creat), C(_read), C(_write), C(_close), C(_lseek), C(_lseeki64), \
    C(_tell), C(_telli64), C(_dup), C(_dup2), C(_commit), C(_chsize), C(_chsize_s), C(_eof), C(_filelength),            \
    C(_filelengthi64), C(_get_osfhandle), C(_open_osfhandle), C(_locking), C(_umask), C(_mkdir), C(_wmkdir), C(_rmdir), \
    C(_wrmdir), C(_unlink), C(_wunlink), C(remove), C(_wremove), C(_chdir), C(_wchdir), C(rename), C(_wrename),         \
    C(_access), C(_waccess), C(_access_s), C(_waccess_s), C(_chmod), C(_wchmod), C(_getcwd), C(_wgetcwd),               \
    C(_getdrive), C(_fullpath), C(_wfullpath), C(_splitpath), C(_makepath), C(_stat64), C(_wstat64), C(_fstat64),       \
    C(_stat64i32), C(_wstat64i32), C(_fstat64i32), C(_stat32), C(_wstat32), C(_fstat32), C(_findfirst64),               \
    C(_findfirst64i32), C(_findnext64), C(_findnext64i32), C(_wfindfirst64), C(_wfindfirst64i32), C(_wfindnext64),      \
    C(_wfindnext64i32), C(_findclose), WEXPN("unlink", c__unlink), WEXPN("access", c__access),                           \
    WEXPN("mkdir", c__mkdir), WEXPN("rmdir", c__rmdir), WEXPN("chdir", c__chdir), WEXPN("getcwd", c__getcwd),           \
    WEXPN("fileno", c__fileno), WEXPN("isatty", c__isatty), WEXPN("close", c__close), WEXPN("read", c__read),           \
    WEXPN("write", c__write), WEXPN("lseek", c__lseek), WEXPN("open", c__open), WEXPN("dup", c__dup),                    \
    WEXPN("dup2", c__dup2), WEXPN("strdup", c__strdup), WEXPN("stricmp", c__stricmp), WEXPN("strnicmp", c__strnicmp),   \
    WEXPN("strcasecmp", c__stricmp), WEXPN("strncasecmp", c__strnicmp), WEXPN("_strcmpi", c__stricmp),                  \
    WEXPN("strcmpi", c__stricmp), WEXPN("_wcsdup", c__wcsdup), WEXPN("wcsdup", c__wcsdup),                              \
    WEXPN("_wcsicmp", c__wcsicmp), WEXPN("wcsicmp", c__wcsicmp), WEXPN("_wcsnicmp", c__wcsnicmp),                        \
    WEXPN("getpid", c__getpid), WEXPN("fdopen", c__fdopen), WEXPN("itoa", c__itoa), WEXPN("strupr", c__strupr),          \
    WEXPN("strlwr", c__strlwr), WEXPN("_stricoll", c__stricoll), WEXPN("_strlwr_s", c__strlwr_s),                        \
    WEXPN("_strupr_s", c__strupr_s),                                                                                     \
    C(vfprintf), C(vprintf), C(fprintf), C(printf), C(vsprintf), C(sprintf), C(_vsnprintf), C(_snprintf), C(_vscprintf), \
    C(_scprintf), C(sprintf_s), C(vsprintf_s), C(_vsnprintf_s), C(vfwprintf), C(fwprintf), C(wprintf), C(vwprintf),      \
    C(_vsnwprintf), C(_snwprintf), C(vswprintf), C(swprintf), C(_vscwprintf), C(sscanf), C(sscanf_s), C(fscanf),         \
    C(scanf), C(vsscanf),                                                                                                \
    C(malloc), C(free), C(calloc), C(realloc), C(_msize), C(_expand), C(_recalloc), C(_aligned_malloc),                  \
    C(_aligned_free), C(_aligned_realloc), C(_aligned_offset_malloc), C(_aligned_msize), C(_set_new_mode),               \
    C(_query_new_mode), C(_callnewh), C(_heapchk), C(_heapmin),                                                          \
    C(memcpy), C(memmove), C(memset), C(memcmp), C(memchr), C(memcpy_s), C(memmove_s), C(_memicmp), C(strlen),           \
    C(strnlen), C(strcpy), C(strncpy), C(strcat), C(strncat), C(strcmp), C(strncmp), C(strcoll), C(strxfrm), C(strchr),  \
    C(strrchr), C(strstr), C(strspn), C(strcspn), C(strpbrk), C(strtok), C(strtok_s), C(_strdup), C(_stricmp),          \
    C(_strnicmp), C(_strlwr), C(_strupr), C(_strrev), C(_strset), C(_strnset), C(strcpy_s), C(strncpy_s), C(strcat_s),   \
    C(strncat_s), C(strerror), C(strerror_s), C(_strerror),                                                              \
    C(wcslen), C(wcsnlen), C(wcscpy), C(wcsncpy), C(wcscat), C(wcsncat), C(wcscmp), C(wcsncmp), C(_wcsicmp),             \
    C(_wcsnicmp), C(wcschr), C(wcsrchr), C(wcsstr), C(wcspbrk), C(wcsspn), C(wcscspn), C(wcstok), C(_wcslwr),            \
    C(_wcsupr), C(_wcslwr_s), C(_wcsupr_s), C(wcscpy_s), C(wcsncpy_s), C(wcscat_s), C(wcsncat_s), C(wcstol), C(wcstoul), \
    C(wcstoll), C(wcstoull), C(wcstod), C(wcstof), C(_wtoi), C(_wtol), C(_wtoi64), C(_wtof), C(towlower), C(towupper),   \
    C(iswalpha), C(iswdigit), C(iswxdigit), C(iswspace), C(iswalnum), C(iswupper), C(iswlower), C(iswprint), C(iswgraph), \
    C(iswcntrl), C(iswpunct), C(iswascii), C(iswctype), C(wcscoll), C(_wcsicoll), C(wcstombs), C(mbstowcs),             \
    C(wcstombs_s), C(mbstowcs_s), C(mblen), C(mbtowc), C(wctomb), C(mbrtowc), C(wcrtomb), C(btowc), C(wctob), C(mbrlen), \
    C(mbsinit), WEXPN("_wcstoi64", c_wcstoll), WEXPN("_wcstoui64", c_wcstoull), WEXPN("is_wctype", c_iswctype),          \
    C(__pctype_func), C(_isctype), C(_isctype_l), C(isalpha), C(isdigit), C(isalnum), C(isspace), C(isupper),           \
    C(islower), C(isxdigit), C(isprint), C(isgraph), C(iscntrl), C(ispunct), C(isblank), C(isascii), C(__iscsym),       \
    C(__iscsymf), C(toupper), C(tolower), C(_toupper), C(_tolower), WEXPN("__isascii", c_isascii),                       \
    C(atoi), C(atol), C(atoll), C(atof), C(strtol), C(strtoul), C(strtoll), C(strtoull), C(strtod), C(strtof),          \
    C(strtold), C(_strtod_l), C(_strtoi64_l), C(_strtoui64_l), C(_strtol_l), C(_atof_l), WEXPN("_atoi64", c_atoll),      \
    WEXPN("_strtoi64", c_strtoll), WEXPN("_strtoui64", c_strtoull), C(_itoa), C(_ltoa), C(_ultoa), C(_i64toa),           \
    C(_ui64toa), C(_itoa_s), C(_i64toa_s), C(_ui64toa_s), C(_itow), C(_i64tow), C(_ultow), C(_ui64tow), C(_gcvt),        \
    C(_ecvt), C(_fcvt), C(abs), C(labs), C(llabs), C(div), C(lldiv), C(_rotl), C(_rotr), C(_rotl64), C(_rotr64),         \
    C(_byteswap_ushort), C(_byteswap_ulong), C(_byteswap_uint64), C(rand), C(srand), C(rand_s), C(qsort), C(qsort_s),   \
    C(bsearch), WEXPN("_abs64", c_llabs), WEXPN("ldiv", c_div), WEXPN("_lrotl", c__rotl), WEXPN("_lrotr", c__rotr),      \
    C(sin), C(cos), C(tan), C(asin), C(acos), C(atan), C(sinh), C(cosh), C(tanh), C(asinh), C(acosh), C(atanh), C(exp),  \
    C(exp2), C(expm1), C(log), C(log10), C(log2), C(log1p), C(logb), C(sqrt), C(cbrt), C(ceil), C(floor), C(round),      \
    C(trunc), C(rint), C(nearbyint), C(fabs), C(erf), C(erfc), C(tgamma), C(lgamma), C(atan2), C(pow), C(fmod),          \
    C(hypot), C(fmin), C(fmax), C(fdim), C(copysign), C(nextafter), C(remainder), C(sinf), C(cosf), C(tanf), C(asinf),   \
    C(acosf), C(atanf), C(sinhf), C(coshf), C(tanhf), C(expf), C(exp2f), C(logf), C(log10f), C(log2f), C(log1pf),        \
    C(sqrtf), C(cbrtf), C(ceilf), C(floorf), C(roundf), C(truncf), C(rintf), C(fabsf), C(nearbyintf), C(expm1f),         \
    C(logbf), C(atan2f), C(powf), C(fmodf), C(hypotf), C(fminf), C(fmaxf), C(copysignf), C(nextafterf), C(remainderf),   \
    C(frexp), C(frexpf), C(ldexp), C(ldexpf), C(modf), C(modff), C(scalbn), C(scalbnf), C(fma), C(fmaf), C(remquo),      \
    C(ilogb), C(lround), C(lroundf), C(lrint), C(lrintf), C(llround), C(llroundf), C(llrint), C(llrintf), C(nan),         \
    C(nanf), C(_isnan), C(_isnanf), C(_finite), C(_finitef), C(_chgsign), C(_logb), C(_scalb), C(_fpclass), C(_dclass),  \
    C(_fdclass), C(_ldclass), C(_dsign), C(_fdsign), C(_ldsign), C(_dtest), C(_fdtest), C(_controlfp), C(_controlfp_s),  \
    C(_control87), C(_clearfp), C(_statusfp), C(_fpreset), C(_set_SSE2_enable), C(fegetround), C(fesetround),            \
    C(feclearexcept), C(fetestexcept), C(fegetenv), C(fesetenv), C(feholdexcept), C(feupdateenv), C(__setusermatherr),   \
    WEXPN("_hypot", c_hypot), WEXPN("_hypotf", c_hypotf), WEXPN("_copysign", c_copysign),                                 \
    WEXPN("_copysignf", c_copysignf), WEXPN("_nextafter", c_nextafter), WEXPN("ldexpl", c_ldexp),                        \
    WEXPN("_HUGE", &hugeval), WEXPN("powl", c_pow), WEXPN("sqrtl", c_sqrt), WEXPN("fabsl", c_fabs),                      \
    C(_time64), C(_time32), C(clock), C(_difftime64), C(_difftime32), C(_gmtime64), C(_localtime64), C(_gmtime32),       \
    C(_localtime32), C(_gmtime64_s), C(_localtime64_s), C(_mktime64), C(_mktime32), C(_mkgmtime64), C(strftime),         \
    C(_strftime_l), C(wcsftime), C(asctime), C(_ctime64), C(_tzset), C(_get_timezone), C(_get_daylight),                \
    C(_get_dstbias), C(__timezone), C(__daylight), C(__tzname), C(_ftime64), C(_ftime64_s), C(_timespec64_get),          \
    C(_strdate), C(_strtime), WEXPN("time", c__time64), WEXPN("gmtime", c__gmtime64),                                    \
    WEXPN("localtime", c__localtime64), WEXPN("mktime", c__mktime64), WEXPN("difftime", c__difftime64),                  \
    WEXPN("ctime", c__ctime64), WEXPN("_ftime", c__ftime64), WEXPN("_mkgmtime", c__mkgmtime64),                          \
    WEXPN("timespec_get", c__timespec64_get),                                                                            \
    C(_initialize_onexit_table), C(_register_onexit_function), C(_execute_onexit_table), C(_crt_atexit), C(atexit),      \
    C(_onexit), C(_crt_at_quick_exit), C(__dllonexit), C(exit), C(_exit), C(quick_exit), C(_cexit), C(_c_exit),          \
    C(abort), C(_amsg_exit), C(terminate), C(_invalid_parameter_noinfo), C(_invalid_parameter_noinfo_noreturn),          \
    C(_invalid_parameter), C(_invoke_watson), C(_set_invalid_parameter_handler), C(_get_invalid_parameter_handler),      \
    WEXPN("_set_thread_local_invalid_parameter_handler", c__set_invalid_parameter_handler), C(_set_purecall_handler),    \
    C(_purecall), C(_set_abort_behavior), C(_set_error_mode), C(_crt_debugger_hook), C(_initterm), C(_initterm_e),       \
    C(_set_app_type), C(_set_fmode), C(_get_fmode), C(__p__fmode), C(__p__commode), C(_configure_narrow_argv),           \
    C(_configure_wide_argv), C(_initialize_narrow_environment), C(_initialize_wide_environment),                        \
    C(_get_initial_narrow_environment), C(_get_initial_wide_environment), C(__p___argc), C(__p___argv),                 \
    C(__p___wargv), C(__p__environ), C(__p__wenviron), C(__p__pgmptr), C(__p__wpgmptr), C(_get_pgmptr),                  \
    C(_get_wpgmptr), C(_get_narrow_winmain_command_line), C(_get_wide_winmain_command_line), C(__getmainargs),           \
    C(__wgetmainargs), C(__set_app_type), C(_seh_filter_exe), C(_seh_filter_dll),                                        \
    C(_register_thread_local_exe_atexit_callback), C(_configthreadlocale), C(_errno), C(__doserrno), C(_get_errno),     \
    C(_set_errno), C(_get_doserrno), C(__sys_errlist), C(__sys_nerr), C(getenv), C(_wgetenv), C(_putenv), C(_wputenv),   \
    C(_putenv_s), C(_wputenv_s), C(getenv_s), C(_dupenv_s), C(_wdupenv_s), C(system), C(_getpid), C(signal), C(raise),  \
    C(_lock), C(_unlock), C(localeconv), C(setlocale), C(_wsetlocale), C(_create_locale), C(_wcreate_locale),           \
    C(_free_locale), C(_get_current_locale), C(___lc_codepage_func), C(___mb_cur_max_func), C(___mb_cur_max_l_func),    \
    C(___lc_locale_name_func), C(_getmbcp), C(_setmbcp), C(_beginthreadex), C(_endthreadex), C(_beginthread),           \
    C(_endthread), C(_kbhit), C(_getch), C(_getche), C(_putch), C(_cputs), C(__iob_func), C(__acrt_iob_func),           \
    WEXPN("_setjmp", ms_setjmp), WEXPN("setjmp", ms_setjmp), WEXPN("_setjmpex", ms_setjmp),                             \
    WEXPN("longjmp", ms_longjmp), C(__C_specific_handler), C(_purecall), C(__std_type_info_destroy_list)

static const wexport_t msvcrt_exports[] = {
    COMMON_EXPORTS,
    WEXPN("_iob", iob), WEXPN("__mb_cur_max", &mb_cur_max), WEXPN("_pctype", &pctype), WEXPN("__pioinfo", pioinfo),
    WEXPN("_sys_errlist", errlist), WEXPN("_sys_nerr", &nerr), WEXPN("_environ", &env_a), WEXPN("__argc", &argc_value),
    WEXPN("__argv", &argv_value), WEXPN("__wargv", &wargv_value), WEXPN("_fmode", &fmode_value),
    WEXPN("_commode", &commode_value), WEXPN("_acmdln", &g_cmdline_a), WEXPN("_wcmdln", &g_cmdline_w),
    WEXPN("_timezone", &win_timezone), WEXPN("_daylight", &win_daylight), WEXPN("_tzname", win_tzname),
    WEXPN("_stat", c__stat64i32), WEXPN("_wstat", c__wstat64i32), WEXPN("_fstat", c__fstat64i32),
    WEXPN("_stati64", c__stat64), WEXPN("_wstati64", c__wstat64), WEXPN("_fstati64", c__fstat64),
    WEXPN("_findfirst", c__findfirst64i32), WEXPN("_findnext", c__findnext64i32), WEXPN("_wfindfirst", c__wfindfirst64i32),
    WEXPN("_wfindnext", c__wfindnext64i32), WEXPN("_findfirsti64", c__findfirst64), WEXPN("_findnexti64", c__findnext64),
    WEXPN("_wfindfirsti64", c__wfindfirst64), WEXPN("_wfindnexti64", c__wfindnext64),
    WEXPN("_vsnprintf_l", c__vsnprintf), WEXPN("vsnprintf", c__vsnprintf), WEXPN("snprintf", c__snprintf),
    WEXPN("_ctime32", c__ctime64), WEXPN("_mktime", c__mktime64), WEXPN("_wtoi", c__wtoi),
    WEXPN("__CxxFrameHandler", c___CxxFrameHandler), WEXPN("__CxxFrameHandler3", c___CxxFrameHandler),
    WEXPN("_CxxThrowException", c__CxxThrowException), WEXPN("_set_se_translator", c__set_se_translator),
    WEXPN("__uncaught_exception", c___uncaught_exceptions), WEXPN("_wassert", c__wassert), WEXPN("_assert", c__assert),
};

const wdll_t dll_msvcrt = { "msvcrt.dll", msvcrt_exports, sizeof(msvcrt_exports) / sizeof(msvcrt_exports[0]) };

static const wexport_t ucrt_exports[] = {
    COMMON_EXPORTS,
    C(__stdio_common_vfprintf), C(__stdio_common_vfprintf_s), C(__stdio_common_vsprintf), C(__stdio_common_vsprintf_s),
    C(__stdio_common_vsnprintf_s), C(__stdio_common_vfwprintf), C(__stdio_common_vswprintf),
    C(__stdio_common_vswprintf_s), C(__stdio_common_vsnwprintf_s), C(__stdio_common_vsscanf), C(__stdio_common_vfscanf),
    WEXPN("__stdio_common_vfprintf_p", c___stdio_common_vfprintf), WEXPN("_stat", c__stat64i32),
    WEXPN("_fstat", c__fstat64i32), WEXPN("_wstat", c__wstat64i32), WEXPN("_findfirst", c__findfirst64i32),
    WEXPN("_findnext", c__findnext64i32), WEXPN("vsnprintf", c_vsnprintf), WEXPN("snprintf", c_snprintf),
    WEXPN("_wcsicmp_l", c__wcsicmp), WEXPN("_stricmp_l", c__stricmp), WEXPN("_strnicmp_l", c__strnicmp),
    WEXPN("_get_stream_buffer_pointers", c__invalid_parameter_noinfo), WEXPN("__intrinsic_setjmp", ms_setjmp),
    WEXPN("__intrinsic_setjmpex", ms_setjmp), C(__current_exception), C(__current_exception_context),
    C(__std_type_info_name), C(__std_type_info_compare), C(__std_type_info_hash), C(__std_exception_copy),
    C(__std_exception_destroy), C(_CxxThrowException), C(__uncaught_exceptions), C(wcsrchr),
    WEXPN("_wassert", c__wassert), WEXPN("_assert", c__assert),
};

const wdll_t dll_ucrt = { "ucrtbase.dll", ucrt_exports, sizeof(ucrt_exports) / sizeof(ucrt_exports[0]) };

static const wexport_t vcrt_exports[] = {
    C(memcpy), C(memmove), C(memset), C(memcmp), C(memchr), C(strchr), C(strrchr), C(strstr), C(wcschr), C(wcsrchr),
    C(wcsstr), C(__C_specific_handler), C(__current_exception), C(__current_exception_context),
    WEXPN("__intrinsic_setjmp", ms_setjmp), WEXPN("__intrinsic_setjmpex", ms_setjmp), WEXPN("_setjmp", ms_setjmp),
    WEXPN("_setjmpex", ms_setjmp), WEXPN("longjmp", ms_longjmp), C(__std_type_info_destroy_list),
    C(__std_type_info_name), C(__std_type_info_compare), C(__std_type_info_hash), C(__std_exception_copy),
    C(__std_exception_destroy), C(_CxxThrowException), WEXPN("__CxxFrameHandler3", c___CxxFrameHandler),
    WEXPN("__CxxFrameHandler4", c___CxxFrameHandler), WEXPN("__CxxFrameHandler", c___CxxFrameHandler), C(_purecall),
    C(__uncaught_exceptions), WEXPN("__uncaught_exception", c___uncaught_exceptions), C(_set_se_translator),
    C(_is_exception_typeof), C(_set_purecall_handler), WEXPN("__vcrt_InitializeCriticalSectionEx", c__heapmin),
};

const wdll_t dll_vcruntime = { "vcruntime140.dll", vcrt_exports, sizeof(vcrt_exports) / sizeof(vcrt_exports[0]) };

/* set up what the tables point at */
__attribute__((constructor)) static void crt_init(void) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&crt_lock, &ma);
    ctype_init();
    errlist_init();
    pioinfo_init();
    env_a = environ;
}
