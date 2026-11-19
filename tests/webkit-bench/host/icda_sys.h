/* ICDA's system calls as a Linux host provides them, for the WebKit
 * benchmark: the window manager is emulated inside the browser process, so
 * userspace/gui.c and userspace/webkit/browser.c build and run unchanged.
 *
 * Only the calls gui.c makes exist here (queues, shared memory, pid).  The
 * emulated window manager:
 *   - answers GUI_MSG_OPEN_WINDOW with a BENCH_WIN-sized window (default: the
 *     size asked for) in ordinary memory;
 *   - on every GUI_MSG_FLUSH copies the damaged rectangle into a frame of its
 *     own, as ICDA's compositor does (the whole window when the flush names
 *     no rectangle, or always with BENCH_MAXIMIZED=1, as ICDA's wm.c treats a
 *     maximized window), and counts flushes, pixels and the time spent;
 *   - feeds input from the commands appended to the file BENCH_CTL:
 *       scroll <seconds> <notches per second>   wheel over the page
 *       click <x> <y>                           left button press + release
 *       move <x> <y>                            pointer motion
 *       type <text>                             characters as key presses
 *       key <byte>                              one key byte (13 Enter, 20 Ctrl+T ...)
 *       open <url>                              Ctrl+T, the address, Enter
 *   - prints "benchwm:" statistics lines to stderr every 500 ms. */
#ifndef BENCH_HOST_ICDA_SYS_H
#define BENCH_HOST_ICDA_SYS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui_proto.h"

#define BENCH_Q_WM    1
#define BENCH_Q_APP   2
#define BENCH_Q_REPLY 3
#define BENCH_SHM_WIN 100

static struct {
    int w, h, scale, maximized;
    uint32_t *win;                 /* the window's shared pixels */
    uint32_t *frame;               /* the compositor's copy */
    gui_msg_t q[4096];             /* the app's event queue */
    unsigned head, tail;
    /* statistics */
    unsigned long long flushes, pixels, comp_ns, events;
    long long last_report_ms;
    /* input script */
    FILE *ctl;
    long ctl_off;
    long long last_ctl_ms;
    long long scroll_until, scroll_next, scroll_start, scroll_period;
    int mouse_x, mouse_y;
} bwm;

static long long bwm_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long long bwm_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void bwm_push(const gui_msg_t *m) {
    if (bwm.tail - bwm.head >= sizeof bwm.q / sizeof bwm.q[0]) return;
    bwm.q[bwm.tail++ % (sizeof bwm.q / sizeof bwm.q[0])] = *m;
    bwm.events++;
}

static void bwm_key(uint32_t code) {
    gui_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = GUI_MSG_KEY_EVENT;
    m.key.keycode = code;
    m.key.pressed = 1;
    bwm_push(&m);
}

static void bwm_mouse(int x, int y, int buttons, int wheel) {
    gui_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = GUI_MSG_MOUSE_EVENT;
    m.mouse.x = x;
    m.mouse.y = y;
    m.mouse.buttons = (uint8_t)buttons;
    m.mouse.wheel = (int8_t)wheel;
    bwm.mouse_x = x;
    bwm.mouse_y = y;
    bwm_push(&m);
}

static void bwm_command(char *line) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    else arg = line + strlen(line);
    if (!strcmp(line, "scroll")) {
        double secs = 0, rate = 20;
        sscanf(arg, "%lf %lf", &secs, &rate);
        if (rate < 1) rate = 1;
        bwm.scroll_start = bwm_ms();
        bwm.scroll_until = bwm.scroll_start + (long long)(secs * 1000);
        bwm.scroll_period = (long long)(1000 / rate);
        bwm.scroll_next = bwm.scroll_start;
        if (!bwm.mouse_x) bwm_mouse(bwm.w / 2, bwm.h / 2 + 40, 0, 0);
    } else if (!strcmp(line, "click") || !strcmp(line, "move")) {
        int x = 0, y = 0;
        sscanf(arg, "%d %d", &x, &y);
        bwm_mouse(x, y, 0, 0);
        if (line[0] == 'c') {
            bwm_mouse(x, y, GUI_BTN_LEFT, 0);
            bwm_mouse(x, y, 0, 0);
        }
    } else if (!strcmp(line, "type")) {
        for (const char *s = arg; *s; s++) bwm_key((unsigned char)*s);
    } else if (!strcmp(line, "key")) {
        bwm_key((uint32_t)atoi(arg));
    } else if (!strcmp(line, "open")) {
        bwm_key(20);                                    /* Ctrl+T: a new tab, the address bar focused */
        for (const char *s = arg; *s; s++) bwm_key((unsigned char)*s);
        bwm_key(13);
    } else if (!strcmp(line, "shot")) {                 /* what the compositor shows, as a PPM picture */
        FILE *f = fopen(arg, "wb");
        if (f && bwm.frame) {
            fprintf(f, "P6\n%d %d\n255\n", bwm.w, bwm.h);
            for (size_t i = 0; i < (size_t)bwm.w * (size_t)bwm.h; i++) {
                uint32_t p = bwm.frame[i];
                unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
                fwrite(rgb, 1, 3, f);
            }
        }
        if (f) fclose(f);
    }
}

/* new lines of the control file become events; a scroll in progress sends its next notch */
static void bwm_input(void) {
    long long now = bwm_ms();
    if (bwm.scroll_until && now >= bwm.scroll_next) {
        if (now >= bwm.scroll_until) {
            bwm.scroll_until = 0;
        } else {
            /* down for the first 60 %, then back up: long pages stay scrollable */
            int up = now - bwm.scroll_start > (bwm.scroll_until - bwm.scroll_start) * 6 / 10;
            bwm_mouse(bwm.mouse_x, bwm.mouse_y, 0, up ? 1 : -1);
            bwm.scroll_next += bwm.scroll_period;
        }
    }
    if (now - bwm.last_ctl_ms < 20) return;
    bwm.last_ctl_ms = now;
    if (!bwm.ctl) {
        const char *p = getenv("BENCH_CTL");
        if (!p || !(bwm.ctl = fopen(p, "r"))) return;
    }
    {
        char line[4096];
        clearerr(bwm.ctl);
        fseek(bwm.ctl, bwm.ctl_off, SEEK_SET);
        while (fgets(line, sizeof line, bwm.ctl)) {
            size_t n = strlen(line);
            if (!n || line[n - 1] != '\n') break;       /* a line still being written */
            bwm.ctl_off += (long)n;
            line[n - 1] = 0;
            bwm_command(line);
        }
    }
}

static void bwm_report(void) {
    long long now = bwm_ms();
    if (now - bwm.last_report_ms < 500) return;
    bwm.last_report_ms = now;
    fprintf(stderr, "benchwm: ms=%lld flushes=%llu pixels=%llu comp_us=%llu events=%llu\n", now, bwm.flushes, bwm.pixels,
            bwm.comp_ns / 1000, bwm.events);
}

static void bwm_flush(const gui_msg_t *m) {
    long long t0 = bwm_ns();
    int x = 0, y = 0, w = bwm.w, h = bwm.h;
    if (m->damage.w > 0 && m->damage.h > 0 && !bwm.maximized) {
        x = m->damage.x;
        y = m->damage.y;
        w = m->damage.w;
        h = m->damage.h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > bwm.w) w = bwm.w - x;
        if (y + h > bwm.h) h = bwm.h - y;
    }
    for (int r = 0; r < h; r++)
        memcpy(bwm.frame + (size_t)(y + r) * (size_t)bwm.w + x, bwm.win + (size_t)(y + r) * (size_t)bwm.w + x, (size_t)w * 4);
    bwm.flushes++;
    bwm.pixels += (unsigned long long)(w > 0 && h > 0 ? w * h : 0);
    bwm.comp_ns += (unsigned long long)(bwm_ns() - t0);
    bwm_report();
}

static inline uint64_t icda_get_pid(void) { return (uint64_t)getpid(); }

static inline uint64_t icda_msg_open(const char *name) {
    return !strcmp(name, WM_QUEUE_NAME) ? BENCH_Q_WM : BENCH_Q_APP;
}

static inline int icda_msg_send(uint64_t handle, const void *msg) {
    const gui_msg_t *m = (const gui_msg_t *)msg;
    if (handle == BENCH_Q_WM && m->type == GUI_MSG_OPEN_WINDOW) {
        gui_msg_t r;
        const char *size = getenv("BENCH_WIN");
        bwm.w = m->open_req.w;
        bwm.h = m->open_req.h;
        if (size) sscanf(size, "%dx%d", &bwm.w, &bwm.h);
        bwm.scale = 1;
        bwm.maximized = getenv("BENCH_MAXIMIZED") && atoi(getenv("BENCH_MAXIMIZED"));
        bwm.win = (uint32_t *)calloc((size_t)bwm.w * (size_t)bwm.h, 4);
        bwm.frame = (uint32_t *)calloc((size_t)bwm.w * (size_t)bwm.h, 4);
        memset(&r, 0, sizeof r);
        r.type = GUI_MSG_OPEN_OK;
        r.window_id = 1;
        r.open_ok.shm_handle = BENCH_SHM_WIN;
        r.open_ok.w = bwm.w;
        r.open_ok.h = bwm.h;
        r.open_ok.reply_queue = BENCH_Q_REPLY;
        r.open_ok.scale = 1;
        bwm_push(&r);
        bwm.events--;
        return 0;
    }
    if (handle == BENCH_Q_REPLY && m->type == GUI_MSG_FLUSH) bwm_flush(m);
    return 0;
}

static inline int icda_msg_poll(uint64_t handle) {
    if (handle != BENCH_Q_APP) return 0;
    bwm_input();
    bwm_report();
    return (int)(bwm.tail - bwm.head);
}

static inline int icda_msg_recv(uint64_t handle, void *out, int block) {
    if (handle != BENCH_Q_APP) return -1;
    while (bwm.tail == bwm.head) {
        if (!block) return -1;
        bwm_input();
        usleep(1000);
    }
    *(gui_msg_t *)out = bwm.q[bwm.head++ % (sizeof bwm.q / sizeof bwm.q[0])];
    return 0;
}

static inline uint64_t icda_shm_map(uint64_t handle) {
    return handle == BENCH_SHM_WIN ? (uint64_t)(uintptr_t)bwm.win : 0;
}
static inline int icda_shm_unmap(uint64_t handle) { (void)handle; return 0; }
static inline int icda_shm_close(uint64_t handle) { (void)handle; return 0; }

#endif
