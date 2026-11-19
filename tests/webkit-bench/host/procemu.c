/* LD_PRELOAD for the WebKit benchmark: a Linux host presents memory the way
 * ICDA's kernel does, so the browser and WebKit see what they see on ICDA.
 *
 *   BENCH_PROC=icda-old   ICDA 1.10.3:
 *       /proc/meminfo  rendered when opened; rewinding (fseek, pread) reads
 *                      the same text again; MemAvailable = MemFree
 *       /proc/self/statm  missing
 *   BENCH_PROC=icda-new   ICDA with live /proc:
 *       /proc/meminfo  rendered again on every rewind
 *       /proc/self/statm  the process's own (the host's, which is live)
 *   both:
 *       /proc/self/cgroup  missing (ICDA has no cgroups: WebKit goes by
 *                          /proc/meminfo alone)
 *       sysinfo(), sysconf(_SC_PHYS_PAGES / _SC_AVPHYS_PAGES): the emulated
 *                          machine's RAM and free memory
 *
 * The emulated machine has BENCH_RAM_MB of RAM, of which BENCH_BASE_MB is
 * taken by the kernel and the desktop; the rest is free unless the browser
 * uses it.  The benchmark driver writes how much the browser uses (kB, the
 * sum of its processes' PSS) to the file BENCH_MEMSTATE every 250 ms. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

static int mode(void) {                 /* 0 off, 1 icda-old, 2 icda-new */
    static int m = -1;
    if (m < 0) {
        const char *p = getenv("BENCH_PROC");
        m = !p ? 0 : !strcmp(p, "icda-old") ? 1 : !strcmp(p, "icda-new") ? 2 : 0;
    }
    return m;
}

static unsigned long long env_mb(const char *k, unsigned long long def) {
    const char *p = getenv(k);
    return p ? strtoull(p, 0, 10) : def;
}

/* the emulated machine: total and available, in kB */
static void machine(unsigned long long *total_kb, unsigned long long *avail_kb) {
    unsigned long long total = env_mb("BENCH_RAM_MB", 4096) * 1024, base = env_mb("BENCH_BASE_MB", 700) * 1024, used = 0;
    const char *p = getenv("BENCH_MEMSTATE");
    if (p) {
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char b[64];
            ssize_t n = read(fd, b, sizeof b - 1);
            close(fd);
            if (n > 0) {
                b[n] = 0;
                used = strtoull(b, 0, 10);
            }
        }
    }
    *total_kb = total;
    *avail_kb = base + used < total ? total - base - used : 0;
}

/* ICDA's /proc/meminfo, as kernel/linux/lx.c renders it */
static int render_meminfo(char *b, size_t cap) {
    unsigned long long total, avail;
    machine(&total, &avail);
    if (mode() == 1)
        return snprintf(b, cap,
                        "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:        0 kB\n"
                        "Cached:         0 kB\nSwapCached:     0 kB\nSwapTotal:      0 kB\nSwapFree:       0 kB\nShmem:          0 kB\n"
                        "SReclaimable:   0 kB\n",
                        total, avail, avail);
    return snprintf(b, cap,
                    "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:        0 kB\n"
                    "Cached:         0 kB\nSwapCached:     0 kB\nActive(file):   0 kB\nInactive(file): 0 kB\nSwapTotal:      0 kB\n"
                    "SwapFree:       0 kB\nShmem:          0 kB\nSReclaimable:   0 kB\n",
                    total, avail, avail);
}

/* a live /proc/meminfo: rendered again whenever reading starts over */
typedef struct { char buf[1024]; int len; off_t pos; } live_t;

static ssize_t live_read(void *c, char *out, size_t n) {
    live_t *l = (live_t *)c;
    if (l->pos == 0) l->len = render_meminfo(l->buf, sizeof l->buf);
    if (l->pos >= l->len) return 0;
    if (n > (size_t)(l->len - l->pos)) n = (size_t)(l->len - l->pos);
    memcpy(out, l->buf + l->pos, n);
    l->pos += (off_t)n;
    return (ssize_t)n;
}

static int live_seek(void *c, off_t *off, int whence) {
    live_t *l = (live_t *)c;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? l->pos : l->len;
    if (base + *off < 0) return -1;
    l->pos = base + *off;
    *off = l->pos;
    return 0;
}

static int live_close(void *c) {
    free(c);
    return 0;
}

static int is_self(const char *path, const char *leaf) {
    char own[64];
    if (!strncmp(path, "/proc/self/", 11) && !strcmp(path + 11, leaf)) return 1;
    snprintf(own, sizeof own, "/proc/%d/%s", (int)getpid(), leaf);
    return !strcmp(path, own);
}

FILE *fopen(const char *path, const char *how) {
    static FILE *(*real)(const char *, const char *);
    if (!real) real = (FILE * (*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    if (mode() && path) {
        if (!strcmp(path, "/proc/meminfo")) {
            if (mode() == 1) {                                   /* a copy made now */
                char *b = (char *)malloc(1024);
                int n = b ? render_meminfo(b, 1024) : 0;
                return b ? fmemopen(b, (size_t)n, "r") : 0;      /* (leaks 1 kB per open; the monitor opens it once) */
            } else {
                live_t *l = (live_t *)calloc(1, sizeof *l);
                cookie_io_functions_t io = { live_read, 0, live_seek, live_close };
                return l ? fopencookie(l, "r", io) : 0;
            }
        }
        if (is_self(path, "cgroup") || (mode() == 1 && is_self(path, "statm"))) {
            errno = ENOENT;
            return 0;
        }
    }
    return real(path, how);
}

FILE *fopen64(const char *path, const char *how) { return fopen(path, how); }

static int blocked(const char *path) {
    return mode() && path && (is_self(path, "cgroup") || (mode() == 1 && is_self(path, "statm")));
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    mode_t m = 0;
    if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        m = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (blocked(path)) {
        errno = ENOENT;
        return -1;
    }
    return real(path, flags, m);
}

int open64(const char *path, int flags, ...) {
    mode_t m = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        m = va_arg(ap, mode_t);
        va_end(ap);
    }
    return open(path, flags, m);
}

int sysinfo(struct sysinfo *si) {
    static int (*real)(struct sysinfo *);
    int r;
    if (!real) real = (int (*)(struct sysinfo *))dlsym(RTLD_NEXT, "sysinfo");
    r = real(si);
    if (r == 0 && mode()) {
        unsigned long long total, avail;
        machine(&total, &avail);
        si->mem_unit = 1;
        si->totalram = (unsigned long)(total * 1024);
        si->freeram = (unsigned long)(avail * 1024);
        si->bufferram = 0;
        si->totalswap = si->freeswap = 0;
    }
    return r;
}

long sysconf(int name) {
    static long (*real)(int);
    if (!real) real = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
    if (mode() && (name == _SC_PHYS_PAGES || name == _SC_AVPHYS_PAGES)) {
        unsigned long long total, avail;
        machine(&total, &avail);
        return (long)((name == _SC_PHYS_PAGES ? total : avail) / 4);
    }
    return real(name);
}
