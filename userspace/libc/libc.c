#include <stdint.h>
#include "include/string.h"
#include "include/stdlib.h"
#include "include/ctype.h"
#include "include/stdio.h"
#include "../icda_sys.h"
#include "../ic_mem.h"

#define LIBC_PATH_CAP 256
#define LIBC_ATEXIT_MAX 16

enum { F_STDIN = 0, F_STDOUT, F_STDERR, F_FILE };

struct ic_file {
    int    kind;
    int    readable;
    int    writable;
    int    dirty;
    int    eof;
    int    err;
    int    unget;
    char   path[LIBC_PATH_CAP];
    char  *buf;
    size_t len;
    size_t cap;
    size_t pos;
    size_t disk_len;
    size_t lo;
    size_t hi;
    int    whole;
};

static struct ic_file libc_stdin = { F_STDIN, 1, 0, 0, 0, 0, -1, "", 0, 0, 0, 0, 0, 0, 0, 0 };
static struct ic_file libc_stdout = { F_STDOUT, 0, 1, 0, 0, 0, -1, "", 0, 0, 0, 0, 0, 0, 0, 0 };
static struct ic_file libc_stderr = { F_STDERR, 0, 1, 0, 0, 0, -1, "", 0, 0, 0, 0, 0, 0, 0, 0 };
FILE *stdin = &libc_stdin;
FILE *stdout = &libc_stdout;
FILE *stderr = &libc_stderr;

static char   out_buf[BUFSIZ + 1];
static size_t out_len;
static char   in_line[BUFSIZ + 2];
static size_t in_len, in_pos;

static void (*atexit_fns[LIBC_ATEXIT_MAX])(void);
static int atexit_count;

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    size_t i = 0;
    while ((dst[i] = src[i]) != 0) i++;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    size_t d = strlen(dst), i = 0;
    for (; i < n && src[i]; i++) dst[d + i] = src[i];
    dst[d + i] = 0;
    return dst;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++) {
        if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char *)hay;
    }
    return 0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}

char *strtok(char *s, const char *delim) {
    static char *next;
    char *tok;
    if (s) next = s;
    if (!next) return 0;
    next += strspn(next, delim);
    if (!*next) {
        next = 0;
        return 0;
    }
    tok = next;
    next += strcspn(next, delim);
    if (*next) *next++ = 0;
    else next = 0;
    return tok;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *strerror(int err) {
    return err ? (char *)"error" : (char *)"success";
}

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

void *malloc(size_t size) { return ic_malloc(size); }
void *calloc(size_t count, size_t size) { return ic_calloc(count, size); }
void *realloc(void *ptr, size_t size) { return ic_realloc(ptr, size); }
void  free(void *ptr) { ic_free(ptr); }

int atexit(void (*fn)(void)) {
    if (atexit_count >= LIBC_ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int status) {
    while (atexit_count > 0) atexit_fns[--atexit_count]();
    fflush(stdout);
    fflush(stderr);
    icda_exit((uint64_t)(int64_t)status);
    __builtin_unreachable();
}

void abort(void) {
    fflush(stdout);
    icda_exit(134);
    __builtin_unreachable();
}

static int digit_value(int c) {
    if (isdigit(c)) return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    unsigned long long v = 0;
    int any = 0;
    while (isspace(*p)) p++;
    if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit(p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    while (digit_value(*p) < base) {
        v = v * (unsigned)base + (unsigned)digit_value(*p);
        p++;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    return v;
}

long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    int neg = 0;
    unsigned long long v;
    char *e;
    while (isspace(*p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    v = strtoull(p, &e, base);
    if (end) *end = e == p ? (char *)s : e;
    return neg ? -(long long)v : (long long)v;
}

long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

static unsigned long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void srand(unsigned seed) { rand_state = seed; }

void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *b = (uint8_t *)base;
    for (size_t gap = count / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < count; i++) {
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap) {
                uint8_t *x = b + (j - gap) * size, *y = b + j * size;
                for (size_t k = 0; k < size; k++) {
                    uint8_t t = x[k];
                    x[k] = y[k];
                    y[k] = t;
                }
            }
        }
    }
}

char *getenv(const char *name) {
    (void)name;
    return 0;
}

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    FILE  *file;
} sink_t;

static void sink_put(sink_t *s, char c) {
    if (s->file) {
        fputc(c, s->file);
    } else if (s->len + 1 < s->cap) {
        s->buf[s->len] = c;
    }
    s->len++;
}

static void sink_pad(sink_t *s, char c, int n) {
    while (n-- > 0) sink_put(s, c);
}

static int fmt_core(sink_t *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        char tmp[32];
        const char *str;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;
        int neg = 0, n = 0;
        unsigned long long u = 0;
        if (*fmt != '%') {
            sink_put(s, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            if (!left) sink_pad(s, ' ', width - 1);
            sink_put(s, tmp[0]);
            if (left) sink_pad(s, ' ', width - 1);
            continue;
        case 's':
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            n = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
            if (!left) sink_pad(s, ' ', width - n);
            for (int i = 0; i < n; i++) sink_put(s, str[i]);
            if (left) sink_pad(s, ' ', width - n);
            continue;
        case '%':
            sink_put(s, '%');
            continue;
        case 'd':
        case 'i': {
            long long v = lng ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            neg = v < 0;
            u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            u = lng ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned);
            break;
        case 'p':
            u = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            alt = 1;
            break;
        default:
            sink_put(s, '%');
            if (*fmt) sink_put(s, *fmt);
            else fmt--;
            continue;
        }
        {
            int base = (*fmt == 'x' || *fmt == 'X' || *fmt == 'p') ? 16 : (*fmt == 'o' ? 8 : 10);
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            const char *prefix = "";
            int len = 0, zeros, total;
            do {
                tmp[len++] = digits[u % (unsigned)base];
                u /= (unsigned)base;
            } while (u);
            if (prec == 0 && len == 1 && tmp[0] == '0') len = 0;
            if (neg) prefix = "-";
            else if (plus && (*fmt == 'd' || *fmt == 'i')) prefix = "+";
            else if (space && (*fmt == 'd' || *fmt == 'i')) prefix = " ";
            else if (alt && base == 16) prefix = *fmt == 'X' ? "0X" : "0x";
            else if (alt && base == 8 && len) prefix = "0";
            zeros = prec > len ? prec - len : 0;
            total = (int)strlen(prefix) + zeros + len;
            if (zero && !left && prec < 0 && width > total) {
                zeros += width - total;
                total = width;
            }
            if (!left) sink_pad(s, ' ', width - total);
            for (const char *p = prefix; *p; p++) sink_put(s, *p);
            sink_pad(s, '0', zeros);
            while (len > 0) sink_put(s, tmp[--len]);
            if (left) sink_pad(s, ' ', width - total);
        }
    }
    return (int)s->len;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    sink_t s = { buf, cap, 0, 0 };
    int n = fmt_core(&s, fmt, ap);
    if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    sink_t s = { 0, 0, 0, f };
    return fmt_core(&s, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

static void console_flush(void) {
    if (!out_len) return;
    out_buf[out_len] = 0;
    icda_write(out_buf);
    out_len = 0;
}

static void file_touch(FILE *f, size_t from, size_t to) {
    if (from < f->lo) f->lo = from;
    if (to > f->hi) f->hi = to;
    f->dirty = 1;
}

static int file_reserve(FILE *f, size_t need) {
    size_t cap = f->cap ? f->cap : 256;
    char *grown;
    if (need <= f->cap) return 0;
    while (cap < need) cap *= 2;
    grown = (char *)realloc(f->buf, cap);
    if (!grown) return -1;
    f->buf = grown;
    f->cap = cap;
    return 0;
}

int fputc(int c, FILE *f) {
    if (!f || !f->writable) return EOF;
    if (f->kind == F_STDOUT || f->kind == F_STDERR) {
        if (f->kind == F_STDERR) console_flush();
        out_buf[out_len++] = (char)c;
        if (c == '\n' || out_len >= BUFSIZ || f->kind == F_STDERR) console_flush();
        return (unsigned char)c;
    }
    if (file_reserve(f, f->pos + 1) != 0) {
        f->err = 1;
        return EOF;
    }
    file_touch(f, f->pos, f->pos + 1);
    f->buf[f->pos++] = (char)c;
    if (f->pos > f->len) f->len = f->pos;
    return (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    while (*s) {
        if (fputc(*s++, f) == EOF) return EOF;
    }
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

static int stdin_fill(void) {
    long n;
    console_flush();
    n = (long)icda_read_line(in_line, BUFSIZ);
    if (n < 0) return -1;
    if (n > 0 && in_line[n - 1] == '\n') n--;
    in_line[n] = '\n';
    in_len = (size_t)n + 1;
    in_pos = 0;
    return 0;
}

int fgetc(FILE *f) {
    if (!f || !f->readable) return EOF;
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        return c;
    }
    if (f->kind == F_STDIN) {
        if (in_pos >= in_len && stdin_fill() != 0) {
            f->eof = 1;
            return EOF;
        }
        return (unsigned char)in_line[in_pos++];
    }
    if (f->pos >= f->len) {
        f->eof = 1;
        return EOF;
    }
    return (unsigned char)f->buf[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    f->unget = (unsigned char)c;
    f->eof = 0;
    return c;
}

char *fgets(char *buf, int cap, FILE *f) {
    int i = 0;
    if (cap <= 0) return 0;
    while (i < cap - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return i ? buf : 0;
}

FILE *fopen(const char *path, const char *mode) {
    FILE *f;
    icda_stat_t st;
    int exists;
    if (!path || !mode || strlen(path) >= LIBC_PATH_CAP) return 0;
    exists = (long)icda_stat(path, &st) >= 0;
    if (mode[0] == 'r' && !exists) return 0;
    f = (FILE *)calloc(1, sizeof(*f));
    if (!f) return 0;
    f->kind = F_FILE;
    f->unget = -1;
    strcpy(f->path, path);
    f->readable = mode[0] == 'r' || strchr(mode, '+') != 0;
    f->writable = mode[0] != 'r' || strchr(mode, '+') != 0;
    if (exists && mode[0] != 'w' && st.size) {
        long n;
        if (file_reserve(f, st.size + 1) != 0) {
            free(f);
            return 0;
        }
        n = (long)icda_read_file(path, f->buf, st.size + 1);
        f->len = n > 0 ? (size_t)n : 0;
    }
    f->disk_len = f->len;
    f->lo = (size_t)-1;
    f->whole = mode[0] == 'w' || !exists;
    if (f->whole) f->dirty = 1;
    if (mode[0] == 'a') f->pos = f->len;
    return f;
}

int fflush(FILE *f) {
    if (!f) {
        console_flush();
        return 0;
    }
    if (f->kind != F_FILE) {
        console_flush();
        return 0;
    }
    if (f->writable && f->dirty) {
        int failed;
        if (f->whole) {
            failed = icda_write_file(f->path, f->buf ? f->buf : "", f->len) == (uint64_t)-1;
        } else {
            failed = f->lo < f->hi && icda_write_file_at(f->path, f->lo, f->buf + f->lo, f->hi - f->lo) < 0;
            if (!failed && f->len < f->disk_len) failed = icda_truncate(f->path, f->len) < 0;
        }
        if (failed) {
            f->err = 1;
            return EOF;
        }
        f->dirty = 0;
        f->whole = 0;
        f->disk_len = f->len;
        f->lo = (size_t)-1;
        f->hi = 0;
    }
    return 0;
}

int fclose(FILE *f) {
    int rc;
    if (!f || f->kind != F_FILE) return f ? fflush(f) : EOF;
    rc = fflush(f);
    free(f->buf);
    free(f);
    return rc;
}

size_t fread(void *dst, size_t size, size_t count, FILE *f) {
    uint8_t *d = (uint8_t *)dst;
    size_t total = size * count, got = 0;
    if (!size || !count) return 0;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF) break;
        d[got++] = (uint8_t)c;
    }
    return got / size;
}

size_t fwrite(const void *src, size_t size, size_t count, FILE *f) {
    const uint8_t *s = (const uint8_t *)src;
    size_t total = size * count, put = 0;
    if (!size || !count) return 0;
    while (put < total && fputc(s[put], f) != EOF) put++;
    return put / size;
}

int fseek(FILE *f, long offset, int whence) {
    long base;
    if (!f || f->kind != F_FILE) return -1;
    base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->pos : (long)f->len;
    if (base + offset < 0) return -1;
    f->pos = (size_t)(base + offset);
    if (f->pos > f->len) {
        if (file_reserve(f, f->pos) != 0) return -1;
        memset(f->buf + f->len, 0, f->pos - f->len);
        file_touch(f, f->len, f->pos);
        f->len = f->pos;
    }
    f->eof = 0;
    f->unget = -1;
    return 0;
}

long ftell(FILE *f) { return f && f->kind == F_FILE ? (long)f->pos : -1; }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); clearerr(f); }
int  feof(FILE *f) { return f ? f->eof : 1; }
int  ferror(FILE *f) { return f ? f->err : 1; }

void clearerr(FILE *f) {
    if (f) f->eof = f->err = 0;
}

int remove(const char *path) {
    return icda_remove(path) < 0 ? -1 : 0;
}

int rename(const char *from, const char *to) {
    FILE *src = fopen(from, "r");
    long n;
    if (!src) return -1;
    n = (long)icda_write_file(to, src->buf ? src->buf : "", src->len);
    fclose(src);
    if (n < 0) return -1;
    return remove(from);
}

int __libc_start(int argc, char **argv);
extern int main(int argc, char **argv);

int __libc_start(int argc, char **argv) {
    exit(main(argc, argv));
}
