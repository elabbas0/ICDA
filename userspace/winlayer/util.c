/* Logging, time, UTF-16 text and paths for the Windows layer. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int wtrace;

void wlog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    if (!n || buf[n - 1] != '\n') buf[n++] = '\n';
    (void)!write(2, "[win] ", 6);
    (void)!write(2, buf, (size_t)n);
}

/* Asks through ICDA's dialogs (/dev/dialog): the request is written, then
 * the answer read until it comes.  buttons are "OK|Cancel"; the answer is
 * the button's index, -1 without one (and with no desktop to ask). */
int dialog_run(const char *kind, const char *title, const char *text, const char *buttons, uint32_t window) {
    char req[3000], ans[64];
    int fd, n, o;
    fd = open("/dev/dialog", O_RDWR);
    if (fd < 0) return -1;
    o = snprintf(req, sizeof(req), "kind=%s\ntitle=%s\n", kind, title);
    /* each line of the text is a message= line */
    for (const char *p = text; p && *p && o < (int)sizeof(req) - 64;) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len > (int)sizeof(req) - o - 64) len = (int)sizeof(req) - o - 64;
        o += snprintf(req + o, sizeof(req) - (size_t)o, "message=%.*s\n", len, p);
        p = e ? e + 1 : p + len;
    }
    o += snprintf(req + o, sizeof(req) - (size_t)o, "buttons=%s\n", buttons && *buttons ? buttons : "OK");
    if (window) o += snprintf(req + o, sizeof(req) - (size_t)o, "window=%u\n", window);
    if (o >= (int)sizeof(req) || write(fd, req, (size_t)o) != o) {
        close(fd);
        return -1;
    }
    for (int tries = 0; tries < 20 * 60 * 60; tries++) {      /* an hour */
        struct timespec ts = { 0, 50000000 };
        n = (int)pread(fd, ans, sizeof(ans) - 1, 0);
        if (n <= 0) n = (int)read(fd, ans, sizeof(ans) - 1);
        if (n <= 0) break;
        ans[n] = 0;
        if (!strncmp(ans, "answer=", 7)) {
            close(fd);
            return atoi(ans + 7);
        }
        if (strncmp(ans, "pending", 7)) break;      /* nohost, none */
        nanosleep(&ts, 0);
    }
    close(fd);
    return -1;
}

void wnotice(const char *title, const char *text) {
    wlog("%s: %s", title, text);
    (void)dialog_run("error", title, text, "OK", 0);
}

void wfatal(const char *title, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wnotice(title, buf);
    _exit(1);
}

/* ---- time ------------------------------------------------------------------------- */

#define EPOCH_DIFF_100NS 116444736000000000ULL    /* 1601 to 1970 */

uint64_t now_100ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return EPOCH_DIFF_100NS + (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100;
}

uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void unix_to_filetime(int64_t sec, long nsec, FILETIME *ft) {
    uint64_t v = EPOCH_DIFF_100NS + (uint64_t)sec * 10000000ULL + (uint64_t)nsec / 100;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

DWORD errno_to_win(int e) {
    switch (e) {
    case 0:       return ERROR_SUCCESS;
    case ENOENT:  return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EBADF:   return ERROR_INVALID_HANDLE;
    case ENOMEM:  return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST:  return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case EINVAL:  return ERROR_INVALID_PARAMETER;
    case ENOSPC:  return 112;   /* ERROR_DISK_FULL */
    default:      return ERROR_INVALID_FUNCTION;
    }
}

/* ---- UTF-16 ------------------------------------------------------------------------ */

size_t w_len(const WCHAR *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int w_cmp(const WCHAR *a, const WCHAR *b) {
    while (*a && *a == *b) a++, b++;
    return (int)*a - (int)*b;
}

static WCHAR w_lower1(WCHAR c) { return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c; }

int w_icmp(const WCHAR *a, const WCHAR *b) {
    while (*a && w_lower1(*a) == w_lower1(*b)) a++, b++;
    return (int)w_lower1(*a) - (int)w_lower1(*b);
}

WCHAR *w_dup(const WCHAR *s) {
    size_t n = w_len(s);
    WCHAR *d = malloc((n + 1) * sizeof(WCHAR));
    if (d) memcpy(d, s, (n + 1) * sizeof(WCHAR));
    return d;
}

int w_to_utf8(const WCHAR *s, int len, char *out, int cap) {
    int o = 0, i = 0;
    if (len < 0) len = (int)w_len(s);
    for (; i < len; i++) {
        uint32_t c = s[i];
        char b[4];
        int n;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) b[0] = (char)c, n = 1;
        else if (c < 0x800) b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), n = 2;
        else if (c < 0x10000) b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 63)), b[2] = (char)(0x80 | (c & 63)), n = 3;
        else b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 63)), b[2] = (char)(0x80 | ((c >> 6) & 63)),
             b[3] = (char)(0x80 | (c & 63)), n = 4;
        if (out) {
            if (o + n >= cap) break;
            memcpy(out + o, b, (size_t)n);
        }
        o += n;
    }
    if (out && cap > 0) out[o < cap ? o : cap - 1] = 0;
    return o + 1;
}

int utf8_to_w(const char *s, int len, WCHAR *out, int cap) {
    const unsigned char *p = (const unsigned char *)s;
    int o = 0, i = 0;
    if (len < 0) len = (int)strlen(s);
    while (i < len) {
        uint32_t c = p[i++];
        if (c >= 0xF0 && i + 2 < len + 1) c = ((c & 7) << 18) | ((p[i] & 63) << 12) | ((p[i + 1] & 63) << 6) | (p[i + 2] & 63), i += 3;
        else if (c >= 0xE0 && i + 1 < len + 1) c = ((c & 15) << 12) | ((p[i] & 63) << 6) | (p[i + 1] & 63), i += 2;
        else if (c >= 0xC0 && i < len + 1) c = ((c & 31) << 6) | (p[i] & 63), i += 1;
        if (c >= 0x10000) {
            if (out) {
                if (o + 2 >= cap) break;
                out[o] = (WCHAR)(0xD800 + ((c - 0x10000) >> 10));
                out[o + 1] = (WCHAR)(0xDC00 + ((c - 0x10000) & 1023));
            }
            o += 2;
        } else {
            if (out) {
                if (o + 1 >= cap) break;
                out[o] = (WCHAR)c;
            }
            o++;
        }
    }
    if (out && cap > 0) out[o < cap ? o : cap - 1] = 0;
    return o + 1;
}

char *w_to_utf8_dup(const WCHAR *s) {
    int n;
    char *d;
    if (!s) return 0;
    n = w_to_utf8(s, -1, 0, 0);
    d = malloc((size_t)n + 4);
    if (d) w_to_utf8(s, -1, d, n + 4);
    return d;
}

WCHAR *utf8_to_w_dup(const char *s) {
    int n;
    WCHAR *d;
    if (!s) return 0;
    n = utf8_to_w(s, -1, 0, 0);
    d = malloc(((size_t)n + 2) * sizeof(WCHAR));
    if (d) utf8_to_w(s, -1, d, n + 2);
    return d;
}

int a_icmp(const char *a, const char *b) {
    while (*a) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++, b++;
    }
    return -(int)(unsigned char)*b;
}

void a_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

/* ---- paths ---------------------------------------------------------------------------
 * As Wine does it: C: is a folder of its own (the prefix), Z: is the whole
 * Unix tree.  "C:\Users\me\AppData" is <prefix>/Users/me/AppData, and
 * "Z:\home\games" is /home/games.  A path without a drive is relative to the
 * current directory (a Unix one) and only has its separators changed. */

static char prefix[512];

const char *path_prefix(void) { return prefix; }

static void mkdirs(const char *p) {
    char tmp[600];
    size_t n = strlen(p);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, p, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

void path_init(void) {
    const char *e = getenv("ICDA_WIN_PREFIX"), *home = getenv("HOME");
    if (e && *e) snprintf(prefix, sizeof(prefix), "%s", e);
    else snprintf(prefix, sizeof(prefix), "%s/.windows/c", home && *home ? home : "/home");
    mkdirs(prefix);
    {
        char p[700];
        static const char *const dirs[] = { "Windows/System32", "Windows/Temp", "Users/Public", "ProgramData",
                                            "Program Files" };
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            snprintf(p, sizeof(p), "%s/%s", prefix, dirs[i]);
            mkdirs(p);
        }
    }
}

int path_to_unix(const char *wp, char *out, size_t cap) {
    size_t o = 0;
    const char *s = wp;
    if (!wp || !out || cap < 2) return -1;
    /* \\?\C:\x and \??\C:\x */
    if ((s[0] == '\\' && s[1] == '\\' && s[2] == '?' && s[3] == '\\') || (s[0] == '\\' && s[1] == '?' && s[2] == '?' && s[3] == '\\'))
        s += 4;
    if (((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && s[1] == ':') {
        char d = (char)(s[0] | 32);
        s += 2;
        if (d == 'z') {
            out[o++] = '/';
        } else {
            size_t n = strlen(prefix);
            if (n + 2 > cap) return -1;
            memcpy(out, prefix, n);
            o = n;
            out[o++] = '/';
        }
        while (*s == '\\' || *s == '/') s++;
    } else if (s[0] == '\\' || s[0] == '/') {
        /* \x: on the current drive; C: unless the current directory is outside the prefix */
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) && strncmp(cwd, prefix, strlen(prefix)) == 0) {
            size_t n = strlen(prefix);
            if (n + 2 > cap) return -1;
            memcpy(out, prefix, n);
            o = n;
        }
    }
    for (; *s && o + 1 < cap; s++) {
        char c = *s == '\\' ? '/' : *s;
        if (c == '/' && o > 0 && out[o - 1] == '/') continue;
        out[o++] = c;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return *s ? -1 : 0;
}

int path_to_win(const char *up, char *out, size_t cap) {
    size_t n = strlen(prefix), o = 0;
    const char *s = up;
    char cwd[512], full[1024];
    if (up[0] != '/') {
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        snprintf(full, sizeof(full), "%s/%s", cwd, up);
        return path_to_win(full, out, cap);
    }
    if (strncmp(up, prefix, n) == 0 && (up[n] == '/' || up[n] == 0)) {
        if (cap < 4) return -1;
        out[o++] = 'C', out[o++] = ':';
        s = up + n;
    } else {
        if (cap < 4) return -1;
        out[o++] = 'Z', out[o++] = ':';
    }
    if (!*s) out[o++] = '\\';
    for (; *s && o + 1 < cap; s++) out[o++] = *s == '/' ? '\\' : *s;
    out[o] = 0;
    return 0;
}
