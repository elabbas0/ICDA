/* printf and scanf for Windows programs.
 *
 * Windows passes variable arguments in 8-byte stack slots (the four in
 * registers are spilled next to them), so a Windows va_list is a plain
 * pointer that steps by 8.  The format is walked here with the Windows
 * sizes (long is 32 bits; %I64d, %ls, %S...) and each conversion is
 * printed by the C library with a value of the right type.
 *
 * scanf only ever takes pointers: the format is rewritten to the C
 * library's sizes and the pointers are handed on as they are. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t len, cap;    /* cap 0: counting only */
} sink_t;

static void put(sink_t *s, const char *p, size_t n) {
    if (s->cap && s->len < s->cap) {
        size_t room = s->cap - 1 - s->len;
        if (s->len + 1 <= s->cap) memcpy(s->buf + s->len, p, n < room ? n : room);
    }
    s->len += n;
}

#define NEXT(ap, T) (*(T *)(((ap) += 8) - 8))

/* a wide string's first n characters (n < 0: all) as UTF-8 */
static char *wide_utf8(const WCHAR *w, int n) {
    size_t len = 0;
    char *out;
    if (!w) return strdup("(null)");
    while (w[len] && (n < 0 || (int)len < n)) len++;
    out = malloc(len * 3 + 4);
    w_to_utf8(w, (int)len, out, (int)len * 3 + 4);
    return out;
}

int ms_vformat(char *outbuf, size_t cap, const char *fmt, ms_va_list ap0, int wide_args) {
    sink_t s = { outbuf, 0, cap };
    char *ap = (char *)ap0;
    const char *p = fmt;
    while (*p) {
        char spec[32], tmp[512], conv;
        int si = 0, width = -1, prec = -1, size = 4;   /* size: 1 2 4 8, or 'w' 'n' for strings */
        int wide = 0, narrow = 0;
        const char *start;
        if (*p != '%') {
            const char *q = strchr(p, '%');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            put(&s, p, n);
            p += n;
            continue;
        }
        start = p++;
        if (*p == '%') {
            put(&s, "%", 1);
            p++;
            continue;
        }
        spec[si++] = '%';
        while (*p && strchr("-+ #0", *p)) {
            if (si < 8) spec[si++] = *p;
            p++;
        }
        if (*p == '*') {
            width = NEXT(ap, int);
            if (width < 0) {
                spec[si++] = '-';
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
        }
        spec[si] = 0;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = NEXT(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            }
        }
        /* length */
        for (;;) {
            if (*p == 'h') {
                if (p[1] == 'h') size = 1, p += 2;
                else size = 2, p++;
                narrow = 1;
            } else if (*p == 'l') {
                if (p[1] == 'l') size = 8, p += 2;
                else size = 4, wide = 1, p++;
            } else if (*p == 'L') {
                size = 8, p++;
            } else if (*p == 'w') {
                wide = 1, p++;
            } else if (*p == 'I') {
                if (p[1] == '6' && p[2] == '4') size = 8, p += 3;
                else if (p[1] == '3' && p[2] == '2') size = 4, p += 3;
                else size = 8, p++;
            } else if (*p == 'z' || *p == 't' || *p == 'j') {
                size = 8, p++;
            } else if (*p == 'q') {
                size = 8, p++;
            } else {
                break;
            }
        }
        conv = *p;
        if (!conv) break;
        p++;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (size == 8) v = NEXT(ap, long long);
            else if (size == 2) v = (short)NEXT(ap, int);
            else if (size == 1) v = (signed char)NEXT(ap, int);
            else v = NEXT(ap, int);
            if (width >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, "%d", width);
            if (prec >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, ".%d", prec);
            spec[si++] = 'l', spec[si++] = 'l', spec[si++] = conv, spec[si] = 0;
            put(&s, tmp, (size_t)snprintf(tmp, sizeof(tmp), spec, v));
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v;
            if (size == 8) v = NEXT(ap, unsigned long long);
            else if (size == 2) v = (unsigned short)NEXT(ap, unsigned);
            else if (size == 1) v = (unsigned char)NEXT(ap, unsigned);
            else v = NEXT(ap, unsigned);
            if (width >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, "%d", width);
            if (prec >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, ".%d", prec);
            spec[si++] = 'l', spec[si++] = 'l', spec[si++] = conv, spec[si] = 0;
            put(&s, tmp, (size_t)snprintf(tmp, sizeof(tmp), spec, v));
            break;
        }
        case 'p': {
            void *v = NEXT(ap, void *);
            int n = snprintf(tmp, sizeof(tmp), "%0*llX", width > 16 ? width : 16, (unsigned long long)(uintptr_t)v);
            put(&s, tmp, (size_t)n);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v = NEXT(ap, double);
            int n;
            char *big;
            if (width >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, "%d", width);
            if (prec >= 0) si += snprintf(spec + si, sizeof(spec) - (size_t)si, ".%d", prec);
            spec[si++] = conv, spec[si] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, v);
            if (n < (int)sizeof(tmp)) {
                put(&s, tmp, (size_t)n);
            } else {
                big = malloc((size_t)n + 1);
                snprintf(big, (size_t)n + 1, spec, v);
                put(&s, big, (size_t)n);
                free(big);
            }
            break;
        }
        case 'c': case 'C': {
            int w = (conv == 'C') ? !narrow : (wide || (wide_args && !narrow));
            char ch[8];
            int n;
            if (w) {
                WCHAR c = (WCHAR)NEXT(ap, int);
                n = w_to_utf8(&c, 1, ch, sizeof(ch)) - 1;
            } else {
                ch[0] = (char)NEXT(ap, int);
                n = 1;
            }
            if (width > n && !strchr(spec, '-'))
                for (int i = n; i < width; i++) put(&s, " ", 1);
            put(&s, ch, (size_t)n);
            if (width > n && strchr(spec, '-'))
                for (int i = n; i < width; i++) put(&s, " ", 1);
            break;
        }
        case 's': case 'S': case 'Z': {
            int w = (conv == 'S') ? !narrow : (wide || (wide_args && !narrow));
            char *str, *owned = 0;
            size_t n;
            if (conv == 'Z') {
                /* ANSI_STRING / UNICODE_STRING */
                uint16_t *us = NEXT(ap, uint16_t *);
                if (!us) str = "(null)";
                else if (w) str = owned = wide_utf8(*(WCHAR **)(us + 4), us[0] / 2);
                else str = owned = strndup(*(char **)(us + 4), us[0]);
            } else if (w) {
                str = owned = wide_utf8(NEXT(ap, WCHAR *), prec);
                prec = -1;
            } else {
                str = NEXT(ap, char *);
                if (!str) str = "(null)";
            }
            n = strlen(str);
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            if (width > (int)n && !strchr(spec, '-'))
                for (int i = (int)n; i < width; i++) put(&s, " ", 1);
            put(&s, str, n);
            if (width > (int)n && strchr(spec, '-'))
                for (int i = (int)n; i < width; i++) put(&s, " ", 1);
            free(owned);
            break;
        }
        case 'n': {
            void *v = NEXT(ap, void *);
            if (v) {
                if (size == 8) *(long long *)v = (long long)s.len;
                else if (size == 2) *(short *)v = (short)s.len;
                else if (size == 1) *(signed char *)v = (signed char)s.len;
                else *(int *)v = (int)s.len;
            }
            break;
        }
        default:
            /* not a conversion: printed as it was written */
            put(&s, start, (size_t)(p - start));
            break;
        }
    }
    if (s.cap) s.buf[s.len < s.cap ? s.len : s.cap - 1] = 0;
    return (int)s.len;
}

/* wide printf: the format and output in UTF-16; %s means a wide string */
int ms_vformatw(WCHAR *out, size_t cap, const WCHAR *fmt, ms_va_list ap) {
    char *f = w_to_utf8_dup(fmt), small[1024], *buf = small;
    int n = ms_vformat(small, sizeof(small), f, ap, 1), wn;
    if (n >= (int)sizeof(small)) {
        buf = malloc((size_t)n + 1);
        ms_vformat(buf, (size_t)n + 1, f, ap, 1);
    }
    free(f);
    wn = utf8_to_w(buf, n, 0, 0) - 1;
    if (out && cap) {
        WCHAR *tmp = malloc(((size_t)wn + 1) * sizeof(WCHAR));
        utf8_to_w(buf, n, tmp, wn + 1);
        memcpy(out, tmp, (size_t)((size_t)wn < cap ? (size_t)wn : cap - 1) * sizeof(WCHAR));
        out[(size_t)wn < cap ? (size_t)wn : cap - 1] = 0;
        free(tmp);
    }
    if (buf != small) free(buf);
    return wn;
}

/* ---- scanf -------------------------------------------------------------------------------- */

/* the format with the C library's sizes, and how many pointers it takes;
 * -1 if it has what cannot be handed on (wide strings) */
static int scan_format(const char *in, char *out, size_t cap, int secure, int *sizes_after) {
    size_t o = 0;
    int nptr = 0;
    *sizes_after = 0;
    for (const char *p = in; *p && o + 8 < cap;) {
        int suppress = 0;
        if (*p != '%') {
            out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
        if (*p == '%') {
            out[o++] = *p++;
            continue;
        }
        if (*p == '*') suppress = 1, out[o++] = *p++;
        while (*p >= '0' && *p <= '9') out[o++] = *p++;
        /* length: Windows to C library */
        if (*p == 'I' && p[1] == '6' && p[2] == '4') {
            out[o++] = 'l', out[o++] = 'l';
            p += 3;
        } else if (*p == 'I' && p[1] == '3' && p[2] == '2') {
            p += 3;
        } else if (*p == 'l' && p[1] == 'l') {
            out[o++] = 'l', out[o++] = 'l';
            p += 2;
        } else if (*p == 'l' || *p == 'w') {
            char c = p[1];
            p++;
            if (c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'a') out[o++] = 'l';  /* double */
            else if (c == 's' || c == 'c' || c == '[') return -1;                                     /* wide */
            /* %ld is an int on Windows: no length */
        } else if (*p == 'L') {
            out[o++] = 'l';    /* long double is double */
            p++;
        } else if (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't') {
            while (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't') out[o++] = *p++;
        }
        if (*p == 'S' || *p == 'C') return -1;
        if (*p == '[') {
            out[o++] = *p++;
            if (*p == '^') out[o++] = *p++;
            if (*p == ']') out[o++] = *p++;
            while (*p && *p != ']' && o + 2 < cap) out[o++] = *p++;
            if (*p == ']') out[o++] = *p++;
            if (!suppress) {
                nptr++;
                if (secure) (*sizes_after)++;
            }
            continue;
        }
        if (!*p) break;
        if (!suppress && *p != 'n') {
            nptr++;
            if (secure && (*p == 's' || *p == 'c')) (*sizes_after)++;
        } else if (!suppress && *p == 'n') {
            nptr++;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return nptr;
}

/* the pointers in order; with the secure (_s) forms, the buffer sizes
 * after %s, %c and %[ are skipped */
static int scan_args(const char *fmt, char *ap, int secure, void **ptrs, int max) {
    int n = 0;
    for (const char *p = fmt; *p && n < max;) {
        int suppress = 0;
        char c;
        if (*p++ != '%') continue;
        if (*p == '%') {
            p++;
            continue;
        }
        if (*p == '*') suppress = 1, p++;
        while ((*p >= '0' && *p <= '9') || strchr("hlLIwzjt36", *p)) {
            if (*p == 'I' && p[1] == '6' && p[2] == '4') p += 3;
            else if (*p == 'I' && p[1] == '3' && p[2] == '2') p += 3;
            else p++;
        }
        c = *p;
        if (c == '[') {
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']') p++;
            if (*p) p++;
        } else if (c) {
            p++;
        }
        if (suppress) continue;
        ptrs[n++] = NEXT(ap, void *);
        if (secure && (c == 's' || c == 'c' || c == '[')) (void)NEXT(ap, unsigned);
    }
    return n;
}

#define P32(a) a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15], \
    a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23], a[24], a[25], a[26], a[27], a[28], a[29], a[30], a[31]

int ms_vscan_ex(const char *in, FILE *f, const char *fmt, ms_va_list ap, int secure) {
    char nf[1024];
    void *ptrs[32];
    int sz;
    memset(ptrs, 0, sizeof(ptrs));
    if (scan_format(fmt, nf, sizeof(nf), secure, &sz) < 0) {
        wlog("scanf: wide conversions in \"%s\" are not supported", fmt);
        return 0;
    }
    scan_args(fmt, (char *)ap, secure, ptrs, 32);
    if (f) return fscanf(f, nf, P32(ptrs));
    return sscanf(in, nf, P32(ptrs));
}

int ms_vscan(const char *in, const char *fmt, ms_va_list ap) { return ms_vscan_ex(in, 0, fmt, ap, 0); }
