/* Where a Windows program's window shows: an ICDA window (gui.c, through the
 * kernel's gateway to the window manager), or memory when there is no
 * desktop (headless: tests and benchmarks).
 *
 * A program has one ICDA window, its main one; it is drawn into directly
 * (the window manager's own buffer), since programs that come here redraw
 * all of it each frame.  Keys come as presses and releases once the window
 * asks for raw keys; until the window manager sends one, a key's character
 * is made into a press and a release, so older desktops still work.
 *
 * Headless: ICDA_WIN_SCREEN=1280x720 is the display; ICDA_WIN_SHOT=prefix
 * with ICDA_WIN_SHOT_AT=60,300 writes those frames as prefix-N.ppm;
 * ICDA_WIN_EXIT_AT=N ends the program after frame N; ICDA_WIN_FPS=1 prints
 * the frame rate every 5 seconds; ICDA_WIN_INPUT="N:kd1c,N:ku1c,N:ch a,
 * N:mm5/0,N:bd1,N:bu1,N:close" plays input at frame N. */
#define _GNU_SOURCE
#include "win.h"
#include "../gui.h"
#include "../icda_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int mode = -1;          /* 0 headless, 1 ICDA */
static int is_open;
static uint32_t *hl_pixels;
static int win_w, win_h;
static uint64_t frames;
static uint64_t fps_t0, fps_frames;
static int raw_seen;           /* the window manager sends key releases */

static void pick_mode(void) {
    if (mode >= 0) return;
    if (getenv("ICDA_WIN_HEADLESS")) {
        mode = 0;
        return;
    }
    /* the gateway answers only on ICDA (elsewhere: an error, or nothing) */
    {
        long r = (long)icda_gui_available();
        mode = (r == 1) ? 1 : 0;
    }
    if (wtrace) wlog("display: %s", mode ? "ICDA desktop" : "headless");
}

int bk_headless(void) {
    pick_mode();
    return mode == 0;
}

void bk_screen(int *w, int *h) {
    pick_mode();
    *w = 1280, *h = 720;
    if (mode == 1) {
        icda_gpu_info_t gi;
        memset(&gi, 0, sizeof(gi));
        if (icda_gpu_query(&gi) == 0 && gi.width > 0 && gi.height > 0) *w = gi.width, *h = gi.height;
    } else {
        const char *s = getenv("ICDA_WIN_SCREEN");
        int a, b;
        if (s && sscanf(s, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) *w = a, *h = b;
    }
}

int bk_open(const char *title, int w, int h) {
    pick_mode();
    if (is_open) return 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (mode == 1) {
        int scale;
        /* the window manager's sizes are in its points; ours are pixels */
        if (gui_open_window(title, w, h) != 0) {
            wlog("the desktop gave no window");
            return -1;
        }
        scale = gui_window_scale() > 0 ? gui_window_scale() : 1;
        if (scale > 1) {
            /* reopen at the size in points, so the pixels are what was asked */
            gui_close_window();
            if (gui_open_window(title, (w + scale - 1) / scale, (h + scale - 1) / scale) != 0) return -1;
            scale = gui_window_scale() > 0 ? gui_window_scale() : 1;
        }
        gui_use_shared_buffer(1);
        win_w = gui_window_width() * scale;
        win_h = gui_window_height() * scale;
    } else {
        hl_pixels = calloc((size_t)w * (size_t)h, 4);
        if (!hl_pixels) return -1;
        win_w = w, win_h = h;
    }
    is_open = 1;
    fps_t0 = mono_ns();
    if (wtrace) wlog("window \"%s\" %dx%d", title, win_w, win_h);
    return 0;
}

int bk_is_open(void) { return is_open; }

void bk_close(void) {
    if (!is_open) return;
    if (mode == 1) gui_close_window();
    else free(hl_pixels), hl_pixels = 0;
    is_open = 0;
}

uint32_t *bk_pixels(int *w, int *h) {
    if (!is_open) return 0;
    if (w) *w = win_w;
    if (h) *h = win_h;
    return mode == 1 ? gui_pixel_buffer() : hl_pixels;
}

void bk_title(const char *title) {
    (void)title;      /* the window manager takes the title at opening */
}

uint32_t bk_window_id(void) { return mode == 1 ? gui_window_id() : 0; }
uint64_t bk_frames(void) { return frames; }

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
    for (int i = 0; i < win_w * win_h; i++) {
        uint32_t p = hl_pixels[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int listed(const char *list, uint64_t n) {
    for (const char *p = list; p && *p;) {
        char *e;
        unsigned long long v = strtoull(p, &e, 10);
        if (e == p) break;
        if (v == n) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

void bk_present(int x, int y, int w, int h) {
    if (!is_open) return;
    frames++;
    if (mode == 1) {
        if (w > 0) gui_flush_rect(x, y, w, h);
        else gui_flush();
    } else {
        const char *shot = getenv("ICDA_WIN_SHOT"), *at = getenv("ICDA_WIN_SHOT_AT"), *exit_at = getenv("ICDA_WIN_EXIT_AT");
        if (shot && listed(at ? at : "1", frames)) {
            char path[600];
            snprintf(path, sizeof(path), "%s-%llu.ppm", shot, (unsigned long long)frames);
            write_ppm(path);
            wlog("frame %llu written to %s", (unsigned long long)frames, path);
        }
        if (exit_at && frames >= strtoull(exit_at, 0, 10)) {
            double s = (double)(mono_ns() - fps_t0) / 1e9;
            wlog("%llu frames in %.2f s: %.1f frames a second", (unsigned long long)frames, s, (double)frames / s);
            fflush(0);
            _exit(0);
        }
    }
    fps_frames++;
    if (getenv("ICDA_WIN_FPS")) {
        uint64_t now = mono_ns();
        if (now - fps_t0 >= 5000000000ull) {
            wlog("%.1f frames a second", (double)fps_frames * 1e9 / (double)(now - fps_t0));
            fps_t0 = now;
            fps_frames = 0;
        }
    }
}

void bk_cursor(int shape) {
    if (mode == 1 && is_open) gui_set_cursor(shape < 0 ? 0 : shape);
}

void bk_input_mode(int raw_keys, int pointer_lock) {
    if (mode == 1 && is_open) gui_set_input_mode(raw_keys, pointer_lock);
}

/* ---- events -------------------------------------------------------------------------------- */

#define QMAX 256
static bevent_t q[QMAX];
static int qh, qt;
static int mouse_x, mouse_y, mouse_b;

static void push(const bevent_t *e) {
    int n = (qt + 1) % QMAX;
    if (n == qh) return;
    q[qt] = *e;
    qt = n;
}

static void push_key(int sc, int down) {
    bevent_t e;
    memset(&e, 0, sizeof(e));
    e.type = BEV_KEY;
    e.scancode = sc;
    e.down = down;
    push(&e);
}

static void push_char(int ch) {
    bevent_t e;
    memset(&e, 0, sizeof(e));
    e.type = BEV_CHAR;
    e.ch = ch;
    push(&e);
}

/* US layout: the set-1 code for a character */
static int char_scancode(int c) {
    static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`", row4[] = "zxcvbnm,./";
    static const char srow1[] = "!@#$%^&*()_+", srow2[] = "QWERTYUIOP{}", srow3[] = "ASDFGHJKL:\"~", srow4[] = "ZXCVBNM<>?";
    const char *p;
    if (c == 27) return 0x01;
    if (c == 8 || c == 127) return 0x0E;
    if (c == 9) return 0x0F;
    if (c == 13 || c == 10) return 0x1C;
    if (c == ' ') return 0x39;
    if (c == '\\' || c == '|') return 0x2B;
    if (c > 0 && c < 128) {
        if ((p = strchr(row1, c))) return 0x02 + (int)(p - row1);
        if ((p = strchr(srow1, c))) return 0x02 + (int)(p - srow1);
        if ((p = strchr(row2, c))) return 0x10 + (int)(p - row2);
        if ((p = strchr(srow2, c))) return 0x10 + (int)(p - srow2);
        if ((p = strchr(row3, c))) return 0x1E + (int)(p - row3);
        if ((p = strchr(srow3, c))) return 0x1E + (int)(p - srow3);
        if ((p = strchr(row4, c))) return 0x2C + (int)(p - row4);
        if ((p = strchr(srow4, c))) return 0x2C + (int)(p - srow4);
        if (c >= 1 && c <= 26) return char_scancode('a' + c - 1);      /* Ctrl+letter */
    }
    return 0;
}

/* the window manager's keys as characters and VT sequences: decoded into
 * characters and (until raw keys come) presses and releases */
static int esc_state, esc_p1, esc_p2;

static void tap(int sc) {
    if (raw_seen || !sc) return;
    push_key(sc, 1);
    push_key(sc, 0);
}

static void char_key(uint32_t c) {
    if (esc_state == 0) {
        if (c == 27) {
            esc_state = 1;
            return;
        }
        if (c >= 0xE1 && c <= 0xE5) {         /* chord keys: arrows, delete */
            static const int sc[] = { 0x148, 0x150, 0x14B, 0x14D, 0x153 };
            tap(sc[c - 0xE1]);
            return;
        }
        if (c >= 0xF1 && c <= 0xFC) {         /* F1..F12 */
            int f = (int)(c - 0xF1);
            tap(f < 10 ? 0x3B + f : 0x57 + (f - 10));
            return;
        }
        if (c == 13 || c == 10) {
            tap(0x1C);
            push_char(13);
            return;
        }
        if (c == 8 || c == 127) {
            tap(0x0E);
            push_char(8);
            return;
        }
        tap(char_scancode((int)c));
        if (c >= 32 || c == 9) push_char((int)c);
        return;
    }
    if (esc_state == 1) {
        if (c == '[' || c == 'O') {
            esc_state = 2;
            esc_p1 = esc_p2 = 0;
            return;
        }
        esc_state = 0;
        tap(0x01);              /* Escape, then the key */
        push_char(27);
        char_key(c);
        return;
    }
    if (c >= '0' && c <= '9') {
        if (esc_state == 3) esc_p2 = esc_p2 * 10 + (int)(c - '0');
        else esc_p1 = esc_p1 * 10 + (int)(c - '0');
        return;
    }
    if (c == ';') {
        esc_state = 3;
        return;
    }
    esc_state = 0;
    switch (c) {
    case 'A': tap(0x148); break;
    case 'B': tap(0x150); break;
    case 'C': tap(0x14D); break;
    case 'D': tap(0x14B); break;
    case 'H': tap(0x147); break;
    case 'F': tap(0x14F); break;
    case 'P': tap(0x3B); break;
    case 'Q': tap(0x3C); break;
    case 'R': tap(0x3D); break;
    case 'S': tap(0x3E); break;
    case '~':
        switch (esc_p1) {
        case 1: case 7: tap(0x147); break;
        case 2: tap(0x152); break;
        case 3: tap(0x153); break;
        case 4: case 8: tap(0x14F); break;
        case 5: tap(0x149); break;
        case 6: tap(0x151); break;
        case 15: tap(0x3F); break;
        case 17: tap(0x40); break;
        case 18: tap(0x41); break;
        case 19: tap(0x42); break;
        case 20: tap(0x43); break;
        case 21: tap(0x44); break;
        case 23: tap(0x57); break;
        case 24: tap(0x58); break;
        default: break;
        }
        break;
    default:
        break;
    }
}

static void icda_events(void) {
    gui_msg_t m;
    while (gui_poll_event(&m)) {
        bevent_t e;
        memset(&e, 0, sizeof(e));
        switch (m.type) {
        case GUI_MSG_KEY_EVENT:
            if (m.key.keycode & GUI_KEY_RAW) {
                raw_seen = 1;
                push_key((int)(m.key.keycode & 0x1FF), m.key.pressed);
            } else if (m.key.pressed) {
                if (m.key.keycode & 0x1000000u) {     /* a chord: its key, as a tap */
                    char_key(m.key.keycode & 0xFF);
                } else {
                    char_key(m.key.keycode);
                }
            }
            break;
        case GUI_MSG_MOUSE_EVENT: {
            int scale = gui_window_scale() > 0 ? gui_window_scale() : 1;
            e.type = BEV_MOUSE;
            if (m.mouse.relative) {
                e.dx = m.mouse.x;
                e.dy = m.mouse.y;
                e.x = mouse_x, e.y = mouse_y;
            } else {
                int nx = m.mouse.x * scale, ny = m.mouse.y * scale;
                e.dx = nx - mouse_x, e.dy = ny - mouse_y;
                e.x = mouse_x = nx, e.y = mouse_y = ny;
            }
            e.buttons = mouse_b = m.mouse.buttons & 7;
            e.wheel = m.mouse.wheel;
            push(&e);
            break;
        }
        case GUI_MSG_RESIZE: {
            int scale = gui_window_scale() > 0 ? gui_window_scale() : 1;
            gui_use_shared_buffer(1);
            win_w = gui_window_width() * scale;
            win_h = gui_window_height() * scale;
            e.type = BEV_RESIZE;
            e.w = win_w, e.h = win_h;
            push(&e);
            break;
        }
        case GUI_MSG_FOCUS:
            e.type = BEV_FOCUS;
            e.down = m.focus.focused;
            push(&e);
            break;
        case GUI_MSG_CLOSE_WINDOW:
            e.type = BEV_CLOSE;
            push(&e);
            break;
        default:
            break;
        }
    }
    /* a lone Escape: nothing followed it */
    if (esc_state == 1) {
        esc_state = 0;
        tap(0x01);
        push_char(27);
    }
}

/* the headless script: "frame:action,frame:action" */
static void script_events(void) {
    static const char *pos;
    static int started;
    const char *s;
    if (!started) {
        pos = getenv("ICDA_WIN_INPUT");
        started = 1;
    }
    while (pos && *pos) {
        char act[64];
        unsigned long long at;
        int n = 0;
        bevent_t e;
        if (sscanf(pos, "%llu:%63[^,]%n", &at, act, &n) < 2) {
            pos = 0;
            break;
        }
        if (at > frames) break;
        s = act;
        memset(&e, 0, sizeof(e));
        if (!strncmp(s, "kd", 2) || !strncmp(s, "ku", 2)) {
            push_key((int)strtol(s + 2, 0, 16), s[1] == 'd');
            raw_seen = 1;
        } else if (!strncmp(s, "ch", 2)) {
            push_char((unsigned char)s[3 - (s[2] != ' ')]);
        } else if (!strncmp(s, "mm", 2)) {
            int dx = 0, dy = 0;
            sscanf(s + 2, "%d/%d", &dx, &dy);
            e.type = BEV_MOUSE;
            mouse_x += dx, mouse_y += dy;
            e.x = mouse_x, e.y = mouse_y, e.dx = dx, e.dy = dy, e.buttons = mouse_b;
            push(&e);
        } else if (!strncmp(s, "bd", 2) || !strncmp(s, "bu", 2)) {
            int b = atoi(s + 2);
            if (s[1] == 'd') mouse_b |= b;
            else mouse_b &= ~b;
            e.type = BEV_MOUSE;
            e.x = mouse_x, e.y = mouse_y, e.buttons = mouse_b;
            push(&e);
        } else if (!strcmp(s, "close")) {
            e.type = BEV_CLOSE;
            push(&e);
        }
        pos += n;
        if (*pos == ',') pos++;
    }
}

int bk_poll(bevent_t *ev) {
    pick_mode();
    if (qh == qt && is_open) {
        if (mode == 1) icda_events();
        else script_events();
    }
    if (qh == qt) return 0;
    *ev = q[qh];
    qh = (qh + 1) % QMAX;
    return 1;
}

void bk_wait(int ms) {
    struct timespec ts;
    if (ms <= 0) return;
    if (ms > 5) ms = 5;     /* the window manager is polled: often enough to keep input prompt */
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    nanosleep(&ts, 0);
}
