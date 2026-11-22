/* user32: window classes, windows, message queues and input.
 *
 * A program's main window (the first top-level one it shows) is its ICDA
 * window; other windows exist for the program (hidden helpers, the windows
 * SDL makes to ask OpenGL what it has) but are not shown.  Windows have no
 * frame of their own: the desktop draws the frame, so a window is all
 * client area and AdjustWindowRect adds nothing.
 *
 * Each thread has a message queue.  Input from the desktop is read by the
 * thread that owns the main window, while it looks for messages, and
 * becomes WM_KEYDOWN, WM_CHAR, WM_MOUSEMOVE... with the codes Windows uses
 * (virtual keys, and set-1 scan codes in lParam, which is what SDL reads).
 * Raw mouse input (WM_INPUT) is what games use for mouselook: when a
 * program asks for it and hides the pointer, the ICDA window locks the
 * pointer and the mouse's motion comes as it is. */
#define _GNU_SOURCE
#include "wingui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* messages */
#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_SETTEXT          0x000C
#define WM_GETTEXT          0x000D
#define WM_GETTEXTLENGTH    0x000E
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_ERASEBKGND       0x0014
#define WM_SHOWWINDOW       0x0018
#define WM_ACTIVATEAPP      0x001C
#define WM_SETCURSOR        0x0020
#define WM_MOUSEACTIVATE    0x0021
#define WM_GETMINMAXINFO    0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_GETICON          0x007F
#define WM_SETICON          0x0080
#define WM_NCCREATE         0x0081
#define WM_NCDESTROY        0x0082
#define WM_NCCALCSIZE       0x0083
#define WM_NCHITTEST        0x0084
#define WM_NCACTIVATE       0x0086
#define WM_INPUT            0x00FF
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_SYSCHAR          0x0106
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A
#define WM_ENTERSIZEMOVE    0x0231
#define WM_EXITSIZEMOVE     0x0232
#define WM_MOUSELEAVE       0x02A3
#define WM_USER             0x0400

#define GWL_WNDPROC   (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_STYLE     (-16)
#define GWL_EXSTYLE   (-20)
#define GWL_USERDATA  (-21)
#define GWL_ID        (-12)

#define WS_POPUP      0x80000000U
#define WS_CHILD      0x40000000U
#define WS_MINIMIZE   0x20000000U
#define WS_VISIBLE    0x10000000U
#define WS_MAXIMIZE   0x01000000U
#define WS_CAPTION    0x00C00000U
#define CW_USEDEFAULT ((int)0x80000000)
#define HWND_MESSAGE  ((HWND)(intptr_t)-3)

#define SWP_NOSIZE         0x0001
#define SWP_NOMOVE         0x0002
#define SWP_NOZORDER       0x0004
#define SWP_NOACTIVATE     0x0010
#define SWP_FRAMECHANGED   0x0020
#define SWP_SHOWWINDOW     0x0040
#define SWP_HIDEWINDOW     0x0080

#define PM_REMOVE 0x1
#define PM_NOYIELD 0x2

#define MK_LBUTTON 0x1
#define MK_RBUTTON 0x2
#define MK_SHIFT   0x4
#define MK_CONTROL 0x8
#define MK_MBUTTON 0x10

typedef struct {
    void     *lpCreateParams;
    HINSTANCE hInstance;
    HMENU     hMenu;
    HWND      hwndParent;
    int       cy, cx, y, x;
    LONG      style;
    const WCHAR *lpszName, *lpszClass;
    DWORD     dwExStyle;
} CREATESTRUCTW;

typedef struct {
    HWND hwnd, hwndInsertAfter;
    int  x, y, cx, cy;
    UINT flags;
} WINDOWPOS;

typedef struct {
    POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} MINMAXINFO;

static pthread_mutex_t ulock;      /* windows and classes (recursive) */

__attribute__((constructor)) static void ulock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ulock, &a);
}

static DWORD msg_time(void) { return (DWORD)(mono_ns() / 1000000ULL); }

/* ---- classes ----------------------------------------------------------------------------- */

static wclass_t *classes;
static ATOM next_atom = 0xC000;

typedef struct {
    UINT      cbSize, style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    const void *lpszMenuName, *lpszClassName;
    HICON     hIconSm;
} WNDCLASSEXW;

typedef struct {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    const void *lpszMenuName, *lpszClassName;
} WNDCLASSW;

static int is_atom(const void *p) { return (uintptr_t)p < 0x10000; }

static wclass_t *class_find(const WCHAR *name) {
    for (wclass_t *c = classes; c; c = c->next) {
        if (is_atom(name)) {
            if (c->atom == (ATOM)(uintptr_t)name) return c;
        } else if (!w_icmp(c->name, name)) {
            return c;
        }
    }
    return 0;
}

static wclass_t *class_find_a(const char *name) {
    WCHAR w[128];
    if (is_atom(name)) return class_find((const WCHAR *)name);
    utf8_to_w(name, -1, w, 128);
    return class_find(w);
}

LRESULT WINAPI DefWindowProcW(HWND h, UINT m, WPARAM wp, LPARAM lp);

static ATOM register_class(const WNDCLASSEXW *wc, int ansi) {
    wclass_t *c;
    pthread_mutex_lock(&ulock);
    if (ansi ? (class_find_a(wc->lpszClassName) != 0) : (class_find(wc->lpszClassName) != 0)) {
        pthread_mutex_unlock(&ulock);
        wset_last_error(ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    c = calloc(1, sizeof(*c));
    if (is_atom(wc->lpszClassName)) c->name[0] = 0;
    else if (ansi) utf8_to_w(wc->lpszClassName, -1, c->name, 128);
    else {
        size_t n = w_len(wc->lpszClassName);
        if (n > 127) n = 127;
        memcpy(c->name, wc->lpszClassName, n * 2);
        c->name[n] = 0;
    }
    c->atom = is_atom(wc->lpszClassName) ? (ATOM)(uintptr_t)wc->lpszClassName : next_atom++;
    c->style = wc->style;
    c->proc = wc->lpfnWndProc;
    c->cls_extra = wc->cbClsExtra;
    c->wnd_extra = wc->cbWndExtra;
    c->inst = wc->hInstance;
    c->icon = wc->hIcon;
    c->icon_sm = wc->cbSize >= sizeof(WNDCLASSEXW) ? wc->hIconSm : 0;
    c->cursor = wc->hCursor;
    c->bg = wc->hbrBackground;
    c->ansi = ansi;
    c->next = classes;
    classes = c;
    pthread_mutex_unlock(&ulock);
    if (wtrace) {
        char n[128];
        w_to_utf8(c->name, -1, n, sizeof(n));
        wlog("RegisterClass(%s)", n);
    }
    return c->atom;
}

ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *wc) { return register_class(wc, 0); }
ATOM WINAPI RegisterClassExA(const WNDCLASSEXW *wc) { return register_class(wc, 1); }

static ATOM register_plain(const WNDCLASSW *w, int ansi) {
    WNDCLASSEXW x;
    memset(&x, 0, sizeof(x));
    x.cbSize = sizeof(x);
    x.style = w->style;
    x.lpfnWndProc = w->lpfnWndProc;
    x.cbClsExtra = w->cbClsExtra;
    x.cbWndExtra = w->cbWndExtra;
    x.hInstance = w->hInstance;
    x.hIcon = w->hIcon;
    x.hCursor = w->hCursor;
    x.hbrBackground = w->hbrBackground;
    x.lpszMenuName = w->lpszMenuName;
    x.lpszClassName = w->lpszClassName;
    return register_class(&x, ansi);
}

ATOM WINAPI RegisterClassW(const WNDCLASSW *w) { return register_plain(w, 0); }
ATOM WINAPI RegisterClassA(const WNDCLASSW *w) { return register_plain(w, 1); }

static BOOL unregister(wclass_t *c) {
    wclass_t **pp;
    if (!c) {
        wset_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    pthread_mutex_lock(&ulock);
    for (pp = &classes; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    pthread_mutex_unlock(&ulock);
    return TRUE;
}

BOOL WINAPI UnregisterClassW(const WCHAR *name, HINSTANCE inst) {
    (void)inst;
    return unregister(class_find(name));
}
BOOL WINAPI UnregisterClassA(const char *name, HINSTANCE inst) {
    (void)inst;
    return unregister(class_find_a(name));
}

static BOOL class_info(wclass_t *c, WNDCLASSEXW *wc, int ex) {
    if (!c) {
        wset_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    if (ex) {
        UINT sz = wc->cbSize;
        memset(wc, 0, sizeof(*wc));
        wc->cbSize = sz;
        wc->hIconSm = c->icon_sm;
    }
    wc->style = c->style;
    wc->lpfnWndProc = c->proc;
    wc->cbClsExtra = c->cls_extra;
    wc->cbWndExtra = c->wnd_extra;
    wc->hInstance = c->inst;
    wc->hIcon = c->icon;
    wc->hCursor = c->cursor;
    wc->hbrBackground = c->bg;
    return TRUE;
}

BOOL WINAPI GetClassInfoExW(HINSTANCE i, const WCHAR *n, WNDCLASSEXW *wc) {
    (void)i;
    return class_info(class_find(n), wc, 1) ? (BOOL)class_find(n)->atom : FALSE;
}
BOOL WINAPI GetClassInfoExA(HINSTANCE i, const char *n, WNDCLASSEXW *wc) {
    (void)i;
    return class_info(class_find_a(n), wc, 1) ? (BOOL)class_find_a(n)->atom : FALSE;
}
BOOL WINAPI GetClassInfoW(HINSTANCE i, const WCHAR *n, WNDCLASSEXW *wc) {
    WNDCLASSEXW x;
    (void)i;
    x.cbSize = sizeof(x);
    if (!class_info(class_find(n), &x, 1)) return FALSE;
    memcpy(wc, &x.style, sizeof(WNDCLASSW));
    return TRUE;
}

/* ---- windows ------------------------------------------------------------------------------- */

#define MAX_WND 512
static wwnd_t wnds[MAX_WND];
static HWND main_hwnd;
static HWND focus_hwnd, active_hwnd, capture_hwnd;

static HWND hwnd_of(int i) { return (HWND)(uintptr_t)(0x10010 + i * 4); }

wwnd_t *wnd_get(HWND h) {
    uintptr_t v = (uintptr_t)h;
    int i;
    if (v < 0x10010 || (v - 0x10010) % 4) return 0;
    i = (int)((v - 0x10010) / 4);
    if (i >= MAX_WND || !wnds[i].used || wnds[i].destroyed) return 0;
    return &wnds[i];
}

wwnd_t *wnd_main(void) { return wnd_get(main_hwnd); }

/* the desktop window: the whole display */
static HWND desktop_hwnd(void) { return (HWND)(uintptr_t)0x10008; }

static int width_of(const RECT *r) { return r->right - r->left; }
static int height_of(const RECT *r) { return r->bottom - r->top; }

void wnd_surface(wwnd_t *w) {
    int cw = width_of(&w->rect), ch = height_of(&w->rect);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (w->top && bk_is_open()) {
        int bw, bh;
        uint32_t *p = bk_pixels(&bw, &bh);
        if (w->own_surface) free(w->surface);
        w->surface = p;
        w->sw = bw, w->sh = bh;
        w->own_surface = 0;
        return;
    }
    if (w->surface && w->own_surface && w->sw == cw && w->sh == ch) return;
    if (w->own_surface) free(w->surface);
    w->surface = calloc((size_t)cw * (size_t)ch, 4);
    w->sw = cw, w->sh = ch;
    w->own_surface = 1;
}

void wnd_present(wwnd_t *w) {
    if (!w) return;
    w->gdi_drawn = 0;
    if (w->top && bk_is_open()) bk_present(0, 0, 0, 0);
}

static LRESULT send_msg(HWND h, UINT m, WPARAM wp, LPARAM lp);

/* shows the main window on the desktop */
static void make_main(wwnd_t *w) {
    char title[256];
    if (main_hwnd || (w->style & WS_CHILD) || w->parent == HWND_MESSAGE) return;
    w_to_utf8(w->title, -1, title, sizeof(title));
    if (!title[0]) {
        const char *b = strrchr(g_exe_win, '\\');
        snprintf(title, sizeof(title), "%s", b ? b + 1 : g_exe_win);
    }
    if (bk_open(title, width_of(&w->rect), height_of(&w->rect)) != 0) return;
    main_hwnd = w->h;
    w->top = 1;
    {
        int bw, bh;
        bk_pixels(&bw, &bh);
        if (bw != width_of(&w->rect) || bh != height_of(&w->rect)) {
            w->rect.right = w->rect.left + bw;
            w->rect.bottom = w->rect.top + bh;
            send_msg(w->h, WM_SIZE, 0, (LPARAM)((uint32_t)bw | ((uint32_t)bh << 16)));
        }
    }
    wnd_surface(w);
}

static void activate(wwnd_t *w) {
    HWND old = focus_hwnd;
    if (active_hwnd != w->h) {
        HWND prev = active_hwnd;
        active_hwnd = w->h;
        send_msg(w->h, WM_ACTIVATEAPP, TRUE, 0);
        send_msg(w->h, WM_NCACTIVATE, TRUE, 0);
        send_msg(w->h, WM_ACTIVATE, 1, (LPARAM)prev);       /* WA_ACTIVE */
    }
    if (focus_hwnd != w->h) {
        focus_hwnd = w->h;
        if (old && wnd_get(old)) send_msg(old, WM_KILLFOCUS, (WPARAM)w->h, 0);
        send_msg(w->h, WM_SETFOCUS, (WPARAM)old, 0);
    }
}

static void deactivate(wwnd_t *w) {
    if (focus_hwnd == w->h) {
        focus_hwnd = 0;
        send_msg(w->h, WM_KILLFOCUS, 0, 0);
    }
    if (active_hwnd == w->h) {
        active_hwnd = 0;
        send_msg(w->h, WM_ACTIVATE, 0, 0);
        send_msg(w->h, WM_NCACTIVATE, FALSE, 0);
        send_msg(w->h, WM_ACTIVATEAPP, FALSE, 0);
    }
}

static void pos_changed(wwnd_t *w, UINT flags) {
    WINDOWPOS wp;
    memset(&wp, 0, sizeof(wp));
    wp.hwnd = w->h;
    wp.x = w->rect.left;
    wp.y = w->rect.top;
    wp.cx = width_of(&w->rect);
    wp.cy = height_of(&w->rect);
    wp.flags = flags;
    send_msg(w->h, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp);
}

static void show(wwnd_t *w, int on) {
    if (on == w->visible) return;
    w->visible = on;
    if (on) w->style |= WS_VISIBLE;
    else w->style &= ~WS_VISIBLE;
    send_msg(w->h, WM_SHOWWINDOW, (WPARAM)on, 0);
    if (on) {
        make_main(w);
        pos_changed(w, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
        if (!(w->style & WS_CHILD)) activate(w);
        w->invalid = 1;
    } else {
        pos_changed(w, SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE);
        deactivate(w);
    }
}

static HWND create_window(DWORD exstyle, wclass_t *c, const WCHAR *title, DWORD style, int x, int y, int cw, int ch,
                          HWND parent, HMENU menu, HINSTANCE inst, void *param, int ansi) {
    wwnd_t *w = 0;
    int i;
    CREATESTRUCTW cs;
    int sw, shh;
    if (!c) {
        wset_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    pthread_mutex_lock(&ulock);
    for (i = 0; i < MAX_WND; i++)
        if (!wnds[i].used) {
            w = &wnds[i];
            break;
        }
    if (!w) {
        pthread_mutex_unlock(&ulock);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->h = hwnd_of(i);
    pthread_mutex_unlock(&ulock);
    bk_screen(&sw, &shh);
    if (cw == CW_USEDEFAULT || cw <= 0) cw = cw == CW_USEDEFAULT ? (sw * 2 / 3) : 1;
    if (ch == CW_USEDEFAULT || ch <= 0) ch = ch == CW_USEDEFAULT ? (shh * 2 / 3) : 1;
    if (x == CW_USEDEFAULT) x = (sw - cw) / 2, y = (shh - ch) / 2;
    w->cls = c;
    w->proc = c->proc;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->parent = (style & WS_CHILD) || parent == HWND_MESSAGE ? parent : 0;
    w->owner = (style & WS_CHILD) ? 0 : parent;
    w->id = (LONG_PTR)menu;
    w->inst = inst;
    w->rect.left = x, w->rect.top = y, w->rect.right = x + cw, w->rect.bottom = y + ch;
    w->enabled = 1;
    w->tid = thread_id();
    w->nextra = c->wnd_extra;
    w->extra = calloc(1, (size_t)(c->wnd_extra > 0 ? c->wnd_extra : 1) + 8);
    if (title) {
        size_t n = w_len(title);
        if (n > 255) n = 255;
        memcpy(w->title, title, n * 2);
        w->title[n] = 0;
    }
    (void)ansi;
    if (wtrace) {
        char t[128], cn[128];
        w_to_utf8(w->title, -1, t, sizeof(t));
        w_to_utf8(c->name, -1, cn, sizeof(cn));
        wlog("CreateWindow(%s, \"%s\", %dx%d, style %x) = %p", cn, t, cw, ch, style, (void *)w->h);
    }
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param;
    cs.hInstance = inst;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.cx = cw, cs.cy = ch, cs.x = x, cs.y = y;
    cs.style = (LONG)style;
    cs.lpszName = w->title;
    cs.lpszClass = c->name;
    cs.dwExStyle = exstyle;
    {
        MINMAXINFO mm;
        memset(&mm, 0, sizeof(mm));
        mm.ptMaxSize.x = sw, mm.ptMaxSize.y = shh;
        mm.ptMaxTrackSize.x = sw * 2, mm.ptMaxTrackSize.y = shh * 2;
        send_msg(w->h, WM_GETMINMAXINFO, 0, (LPARAM)&mm);
    }
    if (!send_msg(w->h, WM_NCCREATE, 0, (LPARAM)&cs)) {
        w->used = 0;
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    send_msg(w->h, WM_NCCALCSIZE, FALSE, (LPARAM)&w->rect);
    if (send_msg(w->h, WM_CREATE, 0, (LPARAM)&cs) == -1) {
        w->used = 0;
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!wnd_get(w->h)) return 0;     /* destroyed while being made */
    send_msg(w->h, WM_SIZE, 0, (LPARAM)((uint32_t)cw | ((uint32_t)ch << 16)));
    send_msg(w->h, WM_MOVE, 0, (LPARAM)((uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16)));
    if (style & WS_VISIBLE) show(w, 1);
    return w->h;
}

HWND WINAPI CreateWindowExW(DWORD ex, const WCHAR *cls, const WCHAR *title, DWORD style, int x, int y, int w, int h,
                            HWND parent, HMENU menu, HINSTANCE inst, void *param) {
    return create_window(ex, class_find(cls), title, style, x, y, w, h, parent, menu, inst, param, 0);
}

HWND WINAPI CreateWindowExA(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int w, int h,
                            HWND parent, HMENU menu, HINSTANCE inst, void *param) {
    WCHAR t[256];
    if (title) utf8_to_w(title, -1, t, 256);
    return create_window(ex, class_find_a(cls), title ? t : 0, style, x, y, w, h, parent, menu, inst, param, 1);
}

BOOL WINAPI DestroyWindow(HWND h) {
    wwnd_t *w = wnd_get(h);
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (w->visible) show(w, 0);
    send_msg(h, WM_DESTROY, 0, 0);
    /* its children go too */
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroyed && wnds[i].parent == h) DestroyWindow(wnds[i].h);
    send_msg(h, WM_NCDESTROY, 0, 0);
    w->destroyed = 1;
    if (h == main_hwnd) {
        bk_close();
        main_hwnd = 0;
        w->surface = 0;
        w->own_surface = 0;
    }
    if (focus_hwnd == h) focus_hwnd = 0;
    if (active_hwnd == h) active_hwnd = 0;
    if (capture_hwnd == h) capture_hwnd = 0;
    if (w->own_surface) free(w->surface);
    free(w->extra);
    w->used = 0;
    return TRUE;
}

BOOL WINAPI IsWindow(HWND h) { return wnd_get(h) != 0; }
BOOL WINAPI IsWindowVisible(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w && w->visible;
}
BOOL WINAPI IsWindowEnabled(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w && w->enabled;
}
BOOL WINAPI EnableWindow(HWND h, BOOL e) {
    wwnd_t *w = wnd_get(h);
    BOOL was;
    if (!w) return FALSE;
    was = !w->enabled;
    w->enabled = e != 0;
    return was;
}
BOOL WINAPI IsIconic(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w && w->minimized;
}
BOOL WINAPI IsZoomed(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w && w->maximized;
}
BOOL WINAPI IsChild(HWND p, HWND c) {
    wwnd_t *w = wnd_get(c);
    while (w) {
        if (w->parent == p) return TRUE;
        w = wnd_get(w->parent);
    }
    return FALSE;
}
HWND WINAPI GetParent(HWND h) {
    wwnd_t *w = wnd_get(h);
    if (!w) return 0;
    return (w->style & WS_CHILD) ? w->parent : w->owner;
}
HWND WINAPI GetAncestor(HWND h, UINT f) {
    wwnd_t *w = wnd_get(h);
    (void)f;
    while (w && (w->style & WS_CHILD) && wnd_get(w->parent)) w = wnd_get(w->parent);
    return w ? w->h : 0;
}
HWND WINAPI SetParent(HWND c, HWND p) {
    wwnd_t *w = wnd_get(c);
    HWND old;
    if (!w) return 0;
    old = w->parent;
    w->parent = p;
    return old;
}
HWND WINAPI GetDesktopWindow(void) { return desktop_hwnd(); }
HWND WINAPI GetShellWindow(void) { return desktop_hwnd(); }
HWND WINAPI GetWindow(HWND h, UINT cmd) {
    (void)h, (void)cmd;
    return 0;
}
HWND WINAPI GetTopWindow(HWND h) {
    (void)h;
    return main_hwnd;
}

DWORD WINAPI GetWindowThreadProcessId(HWND h, DWORD *pid) {
    wwnd_t *w = wnd_get(h);
    if (pid) *pid = (DWORD)getpid();
    return w ? w->tid : 0;
}

typedef BOOL (WCALLBACK *WNDENUMPROC)(HWND, LPARAM);
BOOL WINAPI EnumWindows(WNDENUMPROC fn, LPARAM lp) {
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroyed && !(wnds[i].style & WS_CHILD))
            if (!fn(wnds[i].h, lp)) break;
    return TRUE;
}
BOOL WINAPI EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp) {
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroyed && wnds[i].tid == tid && !(wnds[i].style & WS_CHILD))
            if (!fn(wnds[i].h, lp)) break;
    return TRUE;
}
BOOL WINAPI EnumChildWindows(HWND p, WNDENUMPROC fn, LPARAM lp) {
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroyed && wnds[i].parent == p)
            if (!fn(wnds[i].h, lp)) break;
    return TRUE;
}

HWND WINAPI FindWindowW(const WCHAR *cls, const WCHAR *title) {
    for (int i = 0; i < MAX_WND; i++) {
        wwnd_t *w = &wnds[i];
        if (!w->used || w->destroyed) continue;
        if (cls && !is_atom(cls) && w_icmp(w->cls->name, cls)) continue;
        if (title && w_cmp(w->title, title)) continue;
        return w->h;
    }
    return 0;
}
HWND WINAPI FindWindowA(const char *cls, const char *title) {
    WCHAR c[128], t[256];
    if (cls && !is_atom(cls)) utf8_to_w(cls, -1, c, 128);
    if (title) utf8_to_w(title, -1, t, 256);
    return FindWindowW(cls ? (is_atom(cls) ? (const WCHAR *)cls : c) : 0, title ? t : 0);
}

/* ---- window properties and long values ------------------------------------------------------ */

static int prop_match(wwnd_t *w, int i, const WCHAR *name) {
    if (is_atom(name)) return w->props[i].atom == (ATOM)(uintptr_t)name;
    return !w->props[i].atom && !w_icmp(w->props[i].name, name);
}

BOOL WINAPI SetPropW(HWND h, const WCHAR *name, HANDLE v) {
    wwnd_t *w = wnd_get(h);
    int i;
    if (!w) return FALSE;
    for (i = 0; i < w->nprops; i++)
        if (prop_match(w, i, name)) {
            w->props[i].val = v;
            return TRUE;
        }
    if (w->nprops >= 24) return FALSE;
    i = w->nprops++;
    w->props[i].atom = is_atom(name) ? (ATOM)(uintptr_t)name : 0;
    if (!is_atom(name)) {
        size_t n = w_len(name);
        if (n > 63) n = 63;
        memcpy(w->props[i].name, name, n * 2);
        w->props[i].name[n] = 0;
    }
    w->props[i].val = v;
    return TRUE;
}

HANDLE WINAPI GetPropW(HWND h, const WCHAR *name) {
    wwnd_t *w = wnd_get(h);
    if (!w) return 0;
    for (int i = 0; i < w->nprops; i++)
        if (prop_match(w, i, name)) return w->props[i].val;
    return 0;
}

HANDLE WINAPI RemovePropW(HWND h, const WCHAR *name) {
    wwnd_t *w = wnd_get(h);
    if (!w) return 0;
    for (int i = 0; i < w->nprops; i++)
        if (prop_match(w, i, name)) {
            HANDLE v = w->props[i].val;
            w->props[i] = w->props[--w->nprops];
            return v;
        }
    return 0;
}

BOOL WINAPI SetPropA(HWND h, const char *name, HANDLE v) {
    WCHAR n[64];
    if (is_atom(name)) return SetPropW(h, (const WCHAR *)name, v);
    utf8_to_w(name, -1, n, 64);
    return SetPropW(h, n, v);
}
HANDLE WINAPI GetPropA(HWND h, const char *name) {
    WCHAR n[64];
    if (is_atom(name)) return GetPropW(h, (const WCHAR *)name);
    utf8_to_w(name, -1, n, 64);
    return GetPropW(h, n);
}
HANDLE WINAPI RemovePropA(HWND h, const char *name) {
    WCHAR n[64];
    if (is_atom(name)) return RemovePropW(h, (const WCHAR *)name);
    utf8_to_w(name, -1, n, 64);
    return RemovePropW(h, n);
}

static void set_style(wwnd_t *w, DWORD style) {
    int vis = (style & WS_VISIBLE) != 0;
    w->style = style;
    if (vis != w->visible) {
        w->style = style & ~WS_VISIBLE;
        if (vis) w->style |= WS_VISIBLE;
        show(w, vis);
    }
}

LONG_PTR WINAPI GetWindowLongPtrW(HWND h, int i) {
    wwnd_t *w = wnd_get(h);
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    switch (i) {
    case GWL_WNDPROC: return (LONG_PTR)w->proc;
    case GWL_HINSTANCE: return (LONG_PTR)w->inst;
    case GWL_HWNDPARENT: return (LONG_PTR)GetParent(h);
    case GWL_STYLE: return (LONG)w->style;
    case GWL_EXSTYLE: return (LONG)w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
    default:
        if (i >= 0 && i + 8 <= w->nextra + 8 && i < w->nextra) {
            LONG_PTR v = 0;
            memcpy(&v, w->extra + i, (size_t)(w->nextra - i >= 8 ? 8 : w->nextra - i));
            return v;
        }
        wset_last_error(1413);   /* ERROR_INVALID_INDEX */
        return 0;
    }
}

LONG_PTR WINAPI SetWindowLongPtrW(HWND h, int i, LONG_PTR v) {
    wwnd_t *w = wnd_get(h);
    LONG_PTR old;
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    old = GetWindowLongPtrW(h, i);
    switch (i) {
    case GWL_WNDPROC: w->proc = (WNDPROC)v; break;
    case GWL_HINSTANCE: w->inst = (HINSTANCE)v; break;
    case GWL_HWNDPARENT: w->owner = (HWND)v; break;
    case GWL_STYLE: set_style(w, (DWORD)v); break;
    case GWL_EXSTYLE: w->exstyle = (DWORD)v; break;
    case GWL_USERDATA: w->userdata = v; break;
    case GWL_ID: w->id = v; break;
    default:
        if (i >= 0 && i < w->nextra) memcpy(w->extra + i, &v, (size_t)(w->nextra - i >= 8 ? 8 : w->nextra - i));
        break;
    }
    wset_last_error(0);
    return old;
}

LONG WINAPI GetWindowLongW(HWND h, int i) { return (LONG)GetWindowLongPtrW(h, i); }
LONG WINAPI SetWindowLongW(HWND h, int i, LONG v) {
    if (i >= 0) {
        wwnd_t *w = wnd_get(h);
        LONG old = 0;
        if (!w || i + 4 > w->nextra) return 0;
        memcpy(&old, w->extra + i, 4);
        memcpy(w->extra + i, &v, 4);
        return old;
    }
    return (LONG)SetWindowLongPtrW(h, i, (LONG_PTR)v);
}

ULONG_PTR WINAPI GetClassLongPtrW(HWND h, int i) {
    wwnd_t *w = wnd_get(h);
    if (!w) return 0;
    switch (i) {
    case -12: return (ULONG_PTR)w->cls->cursor;     /* GCLP_HCURSOR */
    case -14: return (ULONG_PTR)w->cls->icon;       /* GCLP_HICON */
    case -34: return (ULONG_PTR)w->cls->icon_sm;    /* GCLP_HICONSM */
    case -10: return (ULONG_PTR)w->cls->bg;         /* GCLP_HBRBACKGROUND */
    case -24: return (ULONG_PTR)w->cls->proc;       /* GCLP_WNDPROC */
    case -26: return w->cls->style;                 /* GCL_STYLE */
    case -32: return w->cls->atom;                  /* GCW_ATOM */
    default: return 0;
    }
}

ULONG_PTR WINAPI SetClassLongPtrW(HWND h, int i, LONG_PTR v) {
    wwnd_t *w = wnd_get(h);
    ULONG_PTR old;
    if (!w) return 0;
    old = GetClassLongPtrW(h, i);
    switch (i) {
    case -12: w->cls->cursor = (HCURSOR)v; break;
    case -14: w->cls->icon = (HICON)v; break;
    case -34: w->cls->icon_sm = (HICON)v; break;
    case -10: w->cls->bg = (HBRUSH)v; break;
    case -24: w->cls->proc = (WNDPROC)v; break;
    case -26: w->cls->style = (UINT)v; break;
    default: break;
    }
    return old;
}

DWORD WINAPI GetClassLongW(HWND h, int i) { return (DWORD)GetClassLongPtrW(h, i); }
DWORD WINAPI SetClassLongW(HWND h, int i, LONG v) { return (DWORD)SetClassLongPtrW(h, i, v); }

int WINAPI GetClassNameW(HWND h, WCHAR *buf, int n) {
    wwnd_t *w = wnd_get(h);
    size_t l;
    if (!w || n <= 0) return 0;
    l = w_len(w->cls->name);
    if ((int)l >= n) l = (size_t)n - 1;
    memcpy(buf, w->cls->name, l * 2);
    buf[l] = 0;
    return (int)l;
}

int WINAPI GetClassNameA(HWND h, char *buf, int n) {
    wwnd_t *w = wnd_get(h);
    if (!w || n <= 0) return 0;
    w_to_utf8(w->cls->name, -1, buf, n);
    return (int)strlen(buf);
}

/* ---- text ---------------------------------------------------------------------------------- */

BOOL WINAPI SetWindowTextW(HWND h, const WCHAR *t) {
    wwnd_t *w = wnd_get(h);
    size_t n;
    if (!w) return FALSE;
    n = t ? w_len(t) : 0;
    if (n > 255) n = 255;
    if (t) memcpy(w->title, t, n * 2);
    w->title[n] = 0;
    if (h == main_hwnd) {
        char a[256];
        w_to_utf8(w->title, -1, a, sizeof(a));
        bk_title(a);
    }
    return TRUE;
}

BOOL WINAPI SetWindowTextA(HWND h, const char *t) {
    WCHAR w[256];
    utf8_to_w(t ? t : "", -1, w, 256);
    return SetWindowTextW(h, w);
}

int WINAPI GetWindowTextW(HWND h, WCHAR *buf, int n) {
    wwnd_t *w = wnd_get(h);
    size_t l;
    if (!w || n <= 0) {
        if (n > 0) buf[0] = 0;
        return 0;
    }
    l = w_len(w->title);
    if ((int)l >= n) l = (size_t)n - 1;
    memcpy(buf, w->title, l * 2);
    buf[l] = 0;
    return (int)l;
}

int WINAPI GetWindowTextA(HWND h, char *buf, int n) {
    wwnd_t *w = wnd_get(h);
    if (!w || n <= 0) return 0;
    w_to_utf8(w->title, -1, buf, n);
    return (int)strlen(buf);
}

int WINAPI GetWindowTextLengthW(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w ? (int)w_len(w->title) : 0;
}
int WINAPI GetWindowTextLengthA(HWND h) {
    wwnd_t *w = wnd_get(h);
    return w ? w_to_utf8(w->title, -1, 0, 0) - 1 : 0;
}

/* ---- position and size -------------------------------------------------------------------- */

BOOL WINAPI GetWindowRect(HWND h, RECT *r) {
    wwnd_t *w = wnd_get(h);
    if (h == desktop_hwnd()) {
        int sw, sh;
        bk_screen(&sw, &sh);
        r->left = r->top = 0, r->right = sw, r->bottom = sh;
        return TRUE;
    }
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    *r = w->rect;
    return TRUE;
}

BOOL WINAPI GetClientRect(HWND h, RECT *r) {
    RECT wr;
    if (!GetWindowRect(h, &wr)) return FALSE;
    r->left = r->top = 0;
    r->right = width_of(&wr);
    r->bottom = height_of(&wr);
    return TRUE;
}

BOOL WINAPI ClientToScreen(HWND h, POINT *p) {
    wwnd_t *w = wnd_get(h);
    if (!w) return FALSE;
    p->x += w->rect.left, p->y += w->rect.top;
    return TRUE;
}

BOOL WINAPI ScreenToClient(HWND h, POINT *p) {
    wwnd_t *w = wnd_get(h);
    if (!w) return FALSE;
    p->x -= w->rect.left, p->y -= w->rect.top;
    return TRUE;
}

int WINAPI MapWindowPoints(HWND from, HWND to, POINT *pts, UINT n) {
    wwnd_t *a = wnd_get(from), *b = wnd_get(to);
    int dx = (a ? a->rect.left : 0) - (b ? b->rect.left : 0), dy = (a ? a->rect.top : 0) - (b ? b->rect.top : 0);
    for (UINT i = 0; i < n; i++) pts[i].x += dx, pts[i].y += dy;
    return (int)((uint32_t)(dx & 0xFFFF) | ((uint32_t)dy << 16));
}

/* no frame: a window is its client area */
BOOL WINAPI AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex) {
    (void)r, (void)style, (void)menu, (void)ex;
    return TRUE;
}
BOOL WINAPI AdjustWindowRect(RECT *r, DWORD style, BOOL menu) {
    (void)r, (void)style, (void)menu;
    return TRUE;
}
BOOL WINAPI AdjustWindowRectExForDpi(RECT *r, DWORD style, BOOL menu, DWORD ex, UINT dpi) {
    (void)r, (void)style, (void)menu, (void)ex, (void)dpi;
    return TRUE;
}

/* resizing the main window: the ICDA window is opened again at the new size */
static void resize_main(wwnd_t *w) {
    char title[256];
    int bw, bh;
    w_to_utf8(w->title, -1, title, sizeof(title));
    bk_close();
    if (bk_open(title[0] ? title : "Windows program", width_of(&w->rect), height_of(&w->rect)) != 0) return;
    bk_pixels(&bw, &bh);
    w->rect.right = w->rect.left + bw;
    w->rect.bottom = w->rect.top + bh;
    wnd_surface(w);
    gl_window_changed(w);
}

BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    wwnd_t *w = wnd_get(h);
    RECT old;
    int sized = 0, moved = 0;
    (void)after;
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    old = w->rect;
    if (!(flags & SWP_NOMOVE)) {
        w->rect.right += x - w->rect.left;
        w->rect.bottom += y - w->rect.top;
        w->rect.left = x;
        w->rect.top = y;
        moved = old.left != x || old.top != y;
    }
    if (!(flags & SWP_NOSIZE)) {
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;
        w->rect.right = w->rect.left + cx;
        w->rect.bottom = w->rect.top + cy;
        sized = width_of(&old) != cx || height_of(&old) != cy;
    }
    if (sized) {
        if (w->top) resize_main(w);
        else if (w->own_surface) wnd_surface(w);
    }
    if (flags & SWP_SHOWWINDOW) show(w, 1);
    if (flags & SWP_HIDEWINDOW) show(w, 0);
    if (sized || moved || (flags & SWP_FRAMECHANGED)) pos_changed(w, flags);
    if (!(flags & SWP_NOACTIVATE) && w->visible && !(w->style & WS_CHILD)) activate(w);
    return TRUE;
}

BOOL WINAPI MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint) {
    (void)repaint;
    return SetWindowPos(h, 0, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

BOOL WINAPI ShowWindow(HWND h, int cmd) {
    wwnd_t *w = wnd_get(h);
    BOOL was;
    if (!w) return FALSE;
    was = w->visible;
    switch (cmd) {
    case 0: show(w, 0); break;                     /* SW_HIDE */
    case 6: case 2: case 7:                        /* SW_MINIMIZE, SHOWMINIMIZED, SHOWMINNOACTIVE */
        w->minimized = 1;
        show(w, 1);
        send_msg(h, WM_SIZE, 1, 0);                /* SIZE_MINIMIZED */
        break;
    case 3:                                        /* SW_MAXIMIZE */
        w->maximized = 1, w->minimized = 0;
        show(w, 1);
        break;
    case 9:                                        /* SW_RESTORE */
        w->maximized = 0;
        if (w->minimized) {
            w->minimized = 0;
            send_msg(h, WM_SIZE, 0, (LPARAM)((uint32_t)width_of(&w->rect) | ((uint32_t)height_of(&w->rect) << 16)));
        }
        show(w, 1);
        break;
    default:
        w->minimized = 0;
        show(w, 1);
        break;
    }
    return was;
}

BOOL WINAPI ShowWindowAsync(HWND h, int cmd) { return ShowWindow(h, cmd); }
BOOL WINAPI UpdateWindow(HWND h) {
    wwnd_t *w = wnd_get(h);
    if (w && w->invalid) {
        w->invalid = 0;
        send_msg(h, WM_PAINT, 0, 0);
    }
    return w != 0;
}
BOOL WINAPI BringWindowToTop(HWND h) { return wnd_get(h) != 0; }
BOOL WINAPI SetForegroundWindow(HWND h) {
    wwnd_t *w = wnd_get(h);
    if (w && w->visible) activate(w);
    return w != 0;
}
HWND WINAPI GetForegroundWindow(void) { return active_hwnd; }
HWND WINAPI GetActiveWindow(void) { return active_hwnd; }
HWND WINAPI SetActiveWindow(HWND h) {
    HWND old = active_hwnd;
    wwnd_t *w = wnd_get(h);
    if (w && w->visible) activate(w);
    return old;
}
HWND WINAPI GetFocus(void) { return focus_hwnd; }
HWND WINAPI SetFocus(HWND h) {
    HWND old = focus_hwnd;
    wwnd_t *w = wnd_get(h);
    if (w && focus_hwnd != h) {
        focus_hwnd = h;
        if (old && wnd_get(old)) send_msg(old, WM_KILLFOCUS, (WPARAM)h, 0);
        send_msg(h, WM_SETFOCUS, (WPARAM)old, 0);
    }
    return old;
}
HWND WINAPI GetCapture(void) { return capture_hwnd; }
HWND WINAPI SetCapture(HWND h) {
    HWND old = capture_hwnd;
    capture_hwnd = h;
    return old;
}
BOOL WINAPI ReleaseCapture(void) {
    capture_hwnd = 0;
    return TRUE;
}
HWND WINAPI WindowFromPoint(POINT p) {
    wwnd_t *w = wnd_main();
    if (w && p.x >= w->rect.left && p.x < w->rect.right && p.y >= w->rect.top && p.y < w->rect.bottom) return w->h;
    return desktop_hwnd();
}
HWND WINAPI ChildWindowFromPoint(HWND h, POINT p) {
    (void)p;
    return h;
}
BOOL WINAPI FlashWindowEx(void *info) {
    (void)info;
    return TRUE;
}
BOOL WINAPI FlashWindow(HWND h, BOOL inv) {
    (void)h, (void)inv;
    return TRUE;
}
BOOL WINAPI SetLayeredWindowAttributes(HWND h, uint32_t key, BYTE alpha, DWORD flags) {
    (void)h, (void)key, (void)alpha, (void)flags;
    return TRUE;
}
int WINAPI SetWindowRgn(HWND h, HRGN r, BOOL redraw) {
    (void)h, (void)r, (void)redraw;
    return 1;
}
BOOL WINAPI GetWindowPlacement(HWND h, void *wp) {
    wwnd_t *w = wnd_get(h);
    uint32_t *p = wp;
    if (!w) return FALSE;
    memset((uint8_t *)wp + 4, 0, 40);
    p[1] = 0;
    p[2] = w->minimized ? 2 : w->maximized ? 3 : 1;
    memcpy((uint8_t *)wp + 28, &w->rect, sizeof(RECT));
    return TRUE;
}
BOOL WINAPI SetWindowPlacement(HWND h, const void *wp) {
    (void)h, (void)wp;
    return TRUE;
}
HMENU WINAPI GetMenu(HWND h) {
    (void)h;
    return 0;
}
HMENU WINAPI GetSystemMenu(HWND h, BOOL r) {
    (void)h, (void)r;
    return 0;
}
BOOL WINAPI EnableMenuItem(HMENU m, UINT i, UINT e) {
    (void)m, (void)i, (void)e;
    return TRUE;
}
BOOL WINAPI DrawMenuBar(HWND h) {
    (void)h;
    return TRUE;
}
HWND WINAPI GetDlgItem(HWND h, int id) {
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && !wnds[i].destroyed && wnds[i].parent == h && wnds[i].id == id) return wnds[i].h;
    return 0;
}

/* ---- painting -------------------------------------------------------------------------------- */

typedef struct {
    HDC  hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore, fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT;

HDC WINAPI GetDC(HWND h);
int WINAPI ReleaseDC(HWND h, HDC dc);

HDC WINAPI BeginPaint(HWND h, PAINTSTRUCT *ps) {
    wwnd_t *w = wnd_get(h);
    memset(ps, 0, sizeof(*ps));
    if (!w) return 0;
    w->invalid = 0;
    ps->hdc = GetDC(h);
    ps->rcPaint.right = width_of(&w->rect);
    ps->rcPaint.bottom = height_of(&w->rect);
    return ps->hdc;
}

/* what WM_PAINT drew (FillRect, TextOut ...) is shown when it ends */
BOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT *ps) {
    wwnd_t *w = wnd_get(h);
    (void)ps;
    if (w && w->gdi_drawn) wnd_present(w);
    return TRUE;
}

BOOL WINAPI InvalidateRect(HWND h, const RECT *r, BOOL erase) {
    wwnd_t *w = wnd_get(h);
    (void)r, (void)erase;
    if (w) w->invalid = 1;
    return TRUE;
}
BOOL WINAPI ValidateRect(HWND h, const RECT *r) {
    wwnd_t *w = wnd_get(h);
    (void)r;
    if (w) w->invalid = 0;
    return TRUE;
}
BOOL WINAPI GetUpdateRect(HWND h, RECT *r, BOOL erase) {
    wwnd_t *w = wnd_get(h);
    (void)erase;
    if (!w || !w->invalid) {
        if (r) memset(r, 0, sizeof(*r));
        return FALSE;
    }
    if (r) {
        r->left = r->top = 0;
        r->right = width_of(&w->rect);
        r->bottom = height_of(&w->rect);
    }
    return TRUE;
}
BOOL WINAPI RedrawWindow(HWND h, const RECT *r, HRGN rg, UINT f) {
    (void)rg, (void)f;
    return InvalidateRect(h, r, TRUE);
}
BOOL WINAPI InvalidateRgn(HWND h, HRGN r, BOOL e) {
    (void)r;
    return InvalidateRect(h, 0, e);
}

/* ---- message queues ----------------------------------------------------------------------- */

#define QCAP 2048

typedef struct {
    DWORD tid;
    MSG   ring[QCAP];
    int   head, tail;
    int   quit;
    int   quit_code;
} mqueue_t;

static mqueue_t *queues[64];
static int nqueues;

/* under sync_lock */
static mqueue_t *queue_of(DWORD tid, int make) {
    for (int i = 0; i < nqueues; i++)
        if (queues[i]->tid == tid) return queues[i];
    if (!make || nqueues >= 64) return 0;
    queues[nqueues] = calloc(1, sizeof(mqueue_t));
    queues[nqueues]->tid = tid;
    return queues[nqueues++];
}

static int post_to(DWORD tid, HWND h, UINT m, WPARAM wp, LPARAM lp) {
    mqueue_t *q;
    int n;
    sync_lock();
    q = queue_of(tid, 1);
    if (!q) {
        sync_unlock();
        return 0;
    }
    n = (q->tail + 1) % QCAP;
    if (n == q->head) {
        sync_unlock();
        wset_last_error(1816);    /* ERROR_NOT_ENOUGH_QUOTA */
        return 0;
    }
    q->ring[q->tail].hwnd = h;
    q->ring[q->tail].message = m;
    q->ring[q->tail].wParam = wp;
    q->ring[q->tail].lParam = lp;
    q->ring[q->tail].time = msg_time();
    q->ring[q->tail].pt.x = 0;
    q->ring[q->tail].pt.y = 0;
    q->tail = n;
    sync_broadcast();
    sync_unlock();
    return 1;
}

BOOL WINAPI PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wwnd_t *w = wnd_get(h);
    if (!h) return post_to(thread_id(), 0, m, wp, lp);
    if (h == (HWND)(uintptr_t)0xFFFF) {          /* HWND_BROADCAST */
        for (int i = 0; i < MAX_WND; i++)
            if (wnds[i].used && !wnds[i].destroyed && !(wnds[i].style & WS_CHILD)) post_to(wnds[i].tid, wnds[i].h, m, wp, lp);
        return TRUE;
    }
    if (!w) {
        wset_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    return post_to(w->tid, h, m, wp, lp);
}

BOOL WINAPI PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { return PostMessageW(h, m, wp, lp); }
BOOL WINAPI PostThreadMessageW(DWORD tid, UINT m, WPARAM wp, LPARAM lp) { return post_to(tid, 0, m, wp, lp); }
BOOL WINAPI PostThreadMessageA(DWORD tid, UINT m, WPARAM wp, LPARAM lp) { return post_to(tid, 0, m, wp, lp); }

void WINAPI PostQuitMessage(int code) {
    mqueue_t *q;
    sync_lock();
    q = queue_of(thread_id(), 1);
    if (q) {
        q->quit = 1;
        q->quit_code = code;
    }
    sync_broadcast();
    sync_unlock();
}

static LRESULT send_msg(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wwnd_t *w = wnd_get(h);
    WNDPROC p;
    if (!w) return 0;
    p = w->proc ? w->proc : DefWindowProcW;
    return p(h, m, wp, lp);
}

LRESULT WINAPI SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (h == (HWND)(uintptr_t)0xFFFF) {
        for (int i = 0; i < MAX_WND; i++)
            if (wnds[i].used && !wnds[i].destroyed && !(wnds[i].style & WS_CHILD)) send_msg(wnds[i].h, m, wp, lp);
        return 0;
    }
    return send_msg(h, m, wp, lp);
}

LRESULT WINAPI SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_SETTEXT && lp) {
        WCHAR t[256];
        utf8_to_w((const char *)lp, -1, t, 256);
        return SendMessageW(h, m, wp, (LPARAM)t);
    }
    return SendMessageW(h, m, wp, lp);
}

LRESULT WINAPI SendMessageTimeoutW(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT f, UINT t, DWORD_PTR *res) {
    LRESULT r = SendMessageW(h, m, wp, lp);
    (void)f, (void)t;
    if (res) *res = (DWORD_PTR)r;
    return 1;
}
BOOL WINAPI SendNotifyMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    SendMessageW(h, m, wp, lp);
    return TRUE;
}

LRESULT WINAPI CallWindowProcW(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p ? p(h, m, wp, lp) : 0; }
LRESULT WINAPI CallWindowProcA(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p ? p(h, m, wp, lp) : 0; }

static UINT next_msg_id = 0xC100;
static struct { char name[64]; UINT id; } reg_msgs[64];
static int nreg;

UINT WINAPI RegisterWindowMessageA(const char *name) {
    for (int i = 0; i < nreg; i++)
        if (!a_icmp(reg_msgs[i].name, name)) return reg_msgs[i].id;
    if (nreg < 64) {
        snprintf(reg_msgs[nreg].name, sizeof(reg_msgs[0].name), "%s", name);
        reg_msgs[nreg].id = next_msg_id++;
        return reg_msgs[nreg++].id;
    }
    return next_msg_id++;
}

UINT WINAPI RegisterWindowMessageW(const WCHAR *name) {
    char a[64];
    w_to_utf8(name, -1, a, sizeof(a));
    return RegisterWindowMessageA(a);
}

/* ---- timers ---------------------------------------------------------------------------------- */

typedef LRESULT (WCALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);

typedef struct {
    int       used;
    HWND      h;
    UINT_PTR  id;
    UINT      ms;
    uint64_t  due;
    TIMERPROC fn;
    DWORD     tid;
} wtimer_t;

static wtimer_t timers[64];
static UINT_PTR next_timer_id = 1;

UINT_PTR WINAPI SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn) {
    int i, slot = -1;
    if (ms < 10) ms = 10;
    sync_lock();
    for (i = 0; i < 64; i++) {
        if (timers[i].used && timers[i].h == h && h && timers[i].id == id) {
            slot = i;
            break;
        }
        if (!timers[i].used && slot < 0) slot = i;
    }
    if (slot < 0) {
        sync_unlock();
        return 0;
    }
    timers[slot].used = 1;
    timers[slot].h = h;
    timers[slot].id = h ? id : next_timer_id++;
    timers[slot].ms = ms;
    timers[slot].due = mono_ns() + (uint64_t)ms * 1000000ull;
    timers[slot].fn = fn;
    timers[slot].tid = thread_id();
    sync_unlock();
    return h ? (id ? id : 1) : timers[slot].id;
}

BOOL WINAPI KillTimer(HWND h, UINT_PTR id) {
    BOOL r = FALSE;
    sync_lock();
    for (int i = 0; i < 64; i++)
        if (timers[i].used && timers[i].h == h && timers[i].id == id) {
            timers[i].used = 0;
            r = TRUE;
        }
    sync_unlock();
    return r;
}

/* a timer that is due, as WM_TIMER (under sync_lock) */
static int due_timer(DWORD tid, MSG *m, uint64_t *next) {
    uint64_t now = mono_ns();
    for (int i = 0; i < 64; i++) {
        if (!timers[i].used || timers[i].tid != tid) continue;
        if (timers[i].due <= now) {
            m->hwnd = timers[i].h;
            m->message = WM_TIMER;
            m->wParam = timers[i].id;
            m->lParam = (LPARAM)timers[i].fn;
            m->time = msg_time();
            timers[i].due = now + (uint64_t)timers[i].ms * 1000000ull;
            return 1;
        }
        if (next && timers[i].due < *next) *next = timers[i].due;
    }
    return 0;
}

/* ---- input ------------------------------------------------------------------------------------- */

static uint8_t key_state[256];        /* 0x80 down, 0x01 toggled */
static int mouse_x, mouse_y, mouse_buttons;
static int raw_mouse;                 /* RegisterRawInputDevices: mouse */
static int raw_keyboard;
static int cursor_hidden;             /* SetCursor(NULL) / ShowCursor counter below 0 */
static int show_count;
static int cursor_clipped;
static int pointer_locked;

/* set-1 scan code (0x100: E0) to virtual key */
static UINT vk_of(int sc) {
    static const uint8_t base[0x59] = {
        0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0xA2, 'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0xA0, 0xDC, 'Z', 'X', 'C', 'V',
        'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0xA1, 0x6A, 0xA4, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6D, 0x64, 0x65, 0x66, 0x6B, 0x61,
        0x62, 0x63, 0x60, 0x6E, 0, 0, 0xE2, 0x7A, 0x7B,
    };
    if (sc & 0x100) {
        switch (sc & 0xFF) {
        case 0x1C: return 0x0D;       /* keypad Enter */
        case 0x1D: return 0xA3;       /* right Ctrl */
        case 0x35: return 0x6F;       /* keypad / */
        case 0x38: return 0xA5;       /* right Alt */
        case 0x47: return 0x24;       /* Home */
        case 0x48: return 0x26;       /* Up */
        case 0x49: return 0x21;       /* Page Up */
        case 0x4B: return 0x25;       /* Left */
        case 0x4D: return 0x27;       /* Right */
        case 0x4F: return 0x23;       /* End */
        case 0x50: return 0x28;       /* Down */
        case 0x51: return 0x22;       /* Page Down */
        case 0x52: return 0x2D;       /* Insert */
        case 0x53: return 0x2E;       /* Delete */
        case 0x5B: return 0x5B;       /* left Windows */
        case 0x5C: return 0x5C;       /* right Windows */
        case 0x5D: return 0x5D;       /* Menu */
        case 0x37: return 0x2C;       /* Print Screen */
        default: return 0;
        }
    }
    if (sc == 0x45) return 0x90;
    return sc < 0x59 ? base[sc] : 0;
}

/* the generic key of a left/right one, as WM_KEYDOWN has it */
static UINT vk_generic(UINT vk) {
    switch (vk) {
    case 0xA0: case 0xA1: return 0x10;   /* VK_SHIFT */
    case 0xA2: case 0xA3: return 0x11;   /* VK_CONTROL */
    case 0xA4: case 0xA5: return 0x12;   /* VK_MENU */
    default: return vk;
    }
}

static int key_down(UINT vk) { return (key_state[vk & 0xFF] & 0x80) != 0; }

static WPARAM mk_flags(void) {
    WPARAM f = 0;
    if (mouse_buttons & 1) f |= MK_LBUTTON;
    if (mouse_buttons & 2) f |= MK_RBUTTON;
    if (mouse_buttons & 4) f |= MK_MBUTTON;
    if (key_down(0x10)) f |= MK_SHIFT;
    if (key_down(0x11)) f |= MK_CONTROL;
    return f;
}

/* raw input records: a WM_INPUT's lParam is one of these */
typedef struct {
    DWORD  dwType, dwSize;
    HANDLE hDevice;
    WPARAM wParam;
    union {
        struct {
            USHORT usFlags;
            USHORT pad;
            USHORT usButtonFlags, usButtonData;
            ULONG  ulRawButtons;
            LONG   lLastX, lLastY;
            ULONG  ulExtraInformation;
        } mouse;
        struct {
            USHORT MakeCode, Flags, Reserved, VKey;
            UINT   Message;
            ULONG  ExtraInformation;
        } keyboard;
    } data;
} RAWINPUT;

#define RAW_RING 512
static RAWINPUT raw_ring[RAW_RING];
static int raw_next;
#define RAW_MOUSE_DEV    ((HANDLE)(uintptr_t)0x5A10)
#define RAW_KEYBOARD_DEV ((HANDLE)(uintptr_t)0x5A20)

static void post_raw(wwnd_t *w, RAWINPUT *r) {
    RAWINPUT *slot = &raw_ring[raw_next];
    raw_next = (raw_next + 1) % RAW_RING;
    *slot = *r;
    post_to(w->tid, w->h, WM_INPUT, 0, (LPARAM)slot);
}

static void raw_mouse_event(wwnd_t *w, int dx, int dy, USHORT bflags, short wheel) {
    RAWINPUT r;
    memset(&r, 0, sizeof(r));
    r.dwType = 0;                     /* RIM_TYPEMOUSE */
    r.dwSize = 48;
    r.hDevice = RAW_MOUSE_DEV;
    r.data.mouse.usFlags = 0;         /* MOUSE_MOVE_RELATIVE */
    r.data.mouse.usButtonFlags = bflags;
    r.data.mouse.usButtonData = (USHORT)wheel;
    r.data.mouse.lLastX = dx;
    r.data.mouse.lLastY = dy;
    post_raw(w, &r);
}

/* the pointer is locked to a game that reads raw mouse input and hides the pointer */
static void update_pointer_lock(void) {
    int want = raw_mouse && (cursor_hidden || show_count < 0 || cursor_clipped);
    if (want != pointer_locked) {
        pointer_locked = want;
        bk_input_mode(1, want);
        if (wtrace) wlog("pointer %s", want ? "locked" : "free");
    }
}

static void post_key(wwnd_t *w, int sc, int down) {
    UINT vk = vk_of(sc), gvk;
    LPARAM lp;
    int was = 0, alt;
    if (!vk) return;
    was = key_down(vk);
    if (down) {
        key_state[vk] |= 0x80;
        if (!was) key_state[vk] ^= 0x01;
    } else {
        key_state[vk] &= (uint8_t)~0x80;
    }
    gvk = vk_generic(vk);
    if (gvk != vk) {
        int any = key_down(gvk == 0x10 ? 0xA0 : gvk == 0x11 ? 0xA2 : 0xA4) || key_down(gvk == 0x10 ? 0xA1 : gvk == 0x11 ? 0xA3 : 0xA5);
        if (any) key_state[gvk] |= 0x80;
        else key_state[gvk] &= (uint8_t)~0x80;
        if (down && !was) key_state[gvk] ^= 0x01;
    }
    alt = key_down(0x12);
    lp = 1 | ((LPARAM)(sc & 0xFF) << 16) | ((sc & 0x100) ? (1L << 24) : 0) | (alt ? (1L << 29) : 0) |
         (was ? (1L << 30) : 0) | (down ? 0 : (1L << 30) | (1L << 31));
    if (raw_keyboard) {
        RAWINPUT r;
        memset(&r, 0, sizeof(r));
        r.dwType = 1;                 /* RIM_TYPEKEYBOARD */
        r.dwSize = 40;
        r.hDevice = RAW_KEYBOARD_DEV;
        r.data.keyboard.MakeCode = (USHORT)(sc & 0xFF);
        r.data.keyboard.Flags = (USHORT)((down ? 0 : 1) | ((sc & 0x100) ? 2 : 0));
        r.data.keyboard.VKey = (USHORT)gvk;
        r.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
        post_raw(w, &r);
    }
    /* Alt and F10 make system keys */
    if (alt || gvk == 0x12 || gvk == 0x79)
        post_to(w->tid, w->h, down ? WM_SYSKEYDOWN : WM_SYSKEYUP, gvk, lp);
    else
        post_to(w->tid, w->h, down ? WM_KEYDOWN : WM_KEYUP, gvk, lp);
}

static LPARAM pos_lp(int x, int y) { return (LPARAM)((uint32_t)(x & 0xFFFF) | ((uint32_t)(y & 0xFFFF) << 16)); }

static void handle_event(wwnd_t *w, const bevent_t *e) {
    switch (e->type) {
    case BEV_KEY:
        post_key(w, e->scancode, e->down);
        break;
    case BEV_CHAR: {
        WCHAR c = (WCHAR)e->ch;
        if (key_down(0x12) && !key_down(0x11)) post_to(w->tid, w->h, WM_SYSCHAR, c, 1);
        else post_to(w->tid, w->h, WM_CHAR, c, 1);
        break;
    }
    case BEV_MOUSE: {
        int changed = (e->buttons ^ mouse_buttons) & 7;
        static const UINT downs[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
        static const UINT ups[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
        static const USHORT rdown[3] = { 0x1, 0x4, 0x10 }, rup[3] = { 0x2, 0x8, 0x20 };
        USHORT bflags = 0;
        if (!pointer_locked) {
            mouse_x = e->x, mouse_y = e->y;
        }
        if (e->dx || e->dy || !changed) {
            if (!pointer_locked) {
                post_to(w->tid, w->h, WM_SETCURSOR, (WPARAM)w->h, 1 | ((LPARAM)WM_MOUSEMOVE << 16));
                post_to(w->tid, w->h, WM_MOUSEMOVE, mk_flags(), pos_lp(mouse_x, mouse_y));
            }
        }
        for (int b = 0; b < 3; b++) {
            if (!(changed & (1 << b))) continue;
            if (e->buttons & (1 << b)) {
                mouse_buttons |= 1 << b;
                key_state[b == 0 ? 0x01 : b == 1 ? 0x02 : 0x04] |= 0x80;
                bflags |= rdown[b];
                post_to(w->tid, w->h, downs[b], mk_flags(), pos_lp(mouse_x, mouse_y));
            } else {
                mouse_buttons &= ~(1 << b);
                key_state[b == 0 ? 0x01 : b == 1 ? 0x02 : 0x04] &= (uint8_t)~0x80;
                bflags |= rup[b];
                post_to(w->tid, w->h, ups[b], mk_flags(), pos_lp(mouse_x, mouse_y));
            }
        }
        if (e->wheel) {
            int16_t d = (int16_t)(e->wheel * 120);
            bflags |= 0x400;          /* RI_MOUSE_WHEEL */
            post_to(w->tid, w->h, WM_MOUSEWHEEL, mk_flags() | ((WPARAM)(uint16_t)d << 16),
                    pos_lp(mouse_x + w->rect.left, mouse_y + w->rect.top));
        }
        if (raw_mouse && (e->dx || e->dy || bflags))
            raw_mouse_event(w, e->dx, e->dy, bflags, (short)(e->wheel * 120));
        break;
    }
    case BEV_FOCUS:
        if (e->down) activate(w);
        else {
            /* keys held when the focus goes are let go */
            for (int vk = 0; vk < 256; vk++) key_state[vk] &= 0x01;
            deactivate(w);
        }
        break;
    case BEV_RESIZE: {
        int bw = e->w, bh = e->h;
        w->rect.right = w->rect.left + bw;
        w->rect.bottom = w->rect.top + bh;
        wnd_surface(w);
        gl_window_changed(w);
        post_to(w->tid, w->h, WM_SIZE, 0, (LPARAM)((uint32_t)bw | ((uint32_t)bh << 16)));
        w->invalid = 1;
        break;
    }
    case BEV_CLOSE:
        post_to(w->tid, w->h, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

/* input from the desktop, read by the main window's thread */
static void pump_input(void) {
    wwnd_t *w = wnd_main();
    bevent_t e;
    if (!w || w->tid != thread_id()) return;
    while (bk_poll(&e)) handle_event(w, &e);
}

/* ---- getting messages --------------------------------------------------------------------------- */

static int msg_match(const MSG *m, HWND h, UINT lo, UINT hi) {
    if (h && h != (HWND)(intptr_t)-1 && m->hwnd != h && !IsChild(h, m->hwnd)) return 0;
    if (h == (HWND)(intptr_t)-1 && m->hwnd) return 0;
    if (lo || hi) {
        if (m->message < lo || m->message > hi) return 0;
    }
    return 1;
}

/* one message for this thread: posted ones, then WM_QUIT, WM_PAINT, WM_TIMER */
static int take_message(MSG *out, HWND h, UINT lo, UINT hi, int remove, uint64_t *next_timer) {
    mqueue_t *q;
    DWORD tid = thread_id();
    int found = 0;
    pump_input();
    sync_lock();
    q = queue_of(tid, 1);
    if (q) {
        for (int i = q->head; i != q->tail; i = (i + 1) % QCAP) {
            if (!msg_match(&q->ring[i], h, lo, hi)) continue;
            *out = q->ring[i];
            if (remove) {
                /* closes the gap: messages stay in order */
                for (int j = i; j != q->head; j = (j - 1 + QCAP) % QCAP) q->ring[j] = q->ring[(j - 1 + QCAP) % QCAP];
                q->head = (q->head + 1) % QCAP;
            }
            found = 1;
            break;
        }
        if (!found && q->quit && (!lo && !hi)) {
            memset(out, 0, sizeof(*out));
            out->message = WM_QUIT;
            out->wParam = (WPARAM)q->quit_code;
            if (remove) q->quit = 0;
            found = 1;
        }
    }
    if (!found && (!lo || (lo <= WM_TIMER && hi >= WM_TIMER))) found = due_timer(tid, out, next_timer);
    sync_unlock();
    if (!found && (!lo || (lo <= WM_PAINT && hi >= WM_PAINT))) {
        for (int i = 0; i < MAX_WND; i++) {
            wwnd_t *w = &wnds[i];
            if (w->used && !w->destroyed && w->invalid && w->visible && w->tid == tid && (!h || h == w->h)) {
                memset(out, 0, sizeof(*out));
                out->hwnd = w->h;
                out->message = WM_PAINT;
                out->time = msg_time();
                if (remove) w->invalid = 0;
                found = 1;
                break;
            }
        }
    }
    if (found) {
        out->pt.x = mouse_x + (wnd_main() ? wnd_main()->rect.left : 0);
        out->pt.y = mouse_y + (wnd_main() ? wnd_main()->rect.top : 0);
    }
    return found;
}

static __thread DWORD last_msg_time;

BOOL WINAPI PeekMessageW(MSG *m, HWND h, UINT lo, UINT hi, UINT flags) {
    MSG tmp;
    if (!take_message(m ? m : &tmp, h, lo, hi, flags & PM_REMOVE, 0)) return FALSE;
    last_msg_time = (m ? m : &tmp)->time;
    return TRUE;
}

BOOL WINAPI PeekMessageA(MSG *m, HWND h, UINT lo, UINT hi, UINT flags) { return PeekMessageW(m, h, lo, hi, flags); }

/* waits for a message or for ms (INFINITE: for ever) */
static void wait_message(DWORD ms) {
    uint64_t end = ms == INFINITE ? ~0ull : mono_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        MSG m;
        uint64_t next = ~0ull, now;
        if (take_message(&m, 0, 0, 0, 0, &next)) return;
        now = mono_ns();
        if (now >= end) return;
        if (wnd_main() && wnd_main()->tid == thread_id()) {
            /* the desktop is polled: a few milliseconds at a time */
            bk_wait(5);
            continue;
        }
        sync_lock();
        {
            mqueue_t *q = queue_of(thread_id(), 1);
            if (q && q->head == q->tail && !q->quit) {
                uint64_t until = end < next ? end : next;
                DWORD wait = until == ~0ull ? 50 : (DWORD)((until - now) / 1000000ull + 1);
                if (wait > 50) wait = 50;
                sync_deadline(wait);
                (void)sync_wait_ms(wait);
            }
        }
        sync_unlock();
    }
}

BOOL WINAPI GetMessageW(MSG *m, HWND h, UINT lo, UINT hi) {
    for (;;) {
        if (take_message(m, h, lo, hi, 1, 0)) {
            last_msg_time = m->time;
            return m->message != WM_QUIT;
        }
        wait_message(INFINITE);
    }
}

BOOL WINAPI GetMessageA(MSG *m, HWND h, UINT lo, UINT hi) { return GetMessageW(m, h, lo, hi); }

BOOL WINAPI WaitMessage(void) {
    wait_message(INFINITE);
    return TRUE;
}

/* the characters come from the desktop as they are typed: nothing to translate */
BOOL WINAPI TranslateMessage(const MSG *m) {
    (void)m;
    return FALSE;
}

LRESULT WINAPI DispatchMessageW(const MSG *m) {
    if (m->message == WM_TIMER && m->lParam) return ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, m->time);
    if (!m->hwnd) return 0;
    return send_msg(m->hwnd, m->message, m->wParam, m->lParam);
}

LRESULT WINAPI DispatchMessageA(const MSG *m) { return DispatchMessageW(m); }

DWORD WINAPI GetMessageTime(void) { return last_msg_time; }
LPARAM WINAPI GetMessageExtraInfo(void) { return 0; }
DWORD WINAPI GetMessagePos(void) { return (DWORD)pos_lp(mouse_x, mouse_y); }
DWORD WINAPI GetQueueStatus(UINT flags) {
    mqueue_t *q;
    DWORD r = 0;
    (void)flags;
    pump_input();
    sync_lock();
    q = queue_of(thread_id(), 1);
    if (q && q->head != q->tail) r = 0x00FF00FF;
    sync_unlock();
    return r;
}
BOOL WINAPI GetInputState(void) { return GetQueueStatus(0) != 0; }
BOOL WINAPI InSendMessage(void) { return FALSE; }
BOOL WINAPI ReplyMessage(LRESULT r) {
    (void)r;
    return FALSE;
}

/* waits for objects or for a message */
DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD flags) {
    uint64_t end = ms == INFINITE ? ~0ull : mono_ns() + (uint64_t)ms * 1000000ull;
    DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
    (void)mask;
    for (;;) {
        MSG m;
        if (n) {
            DWORD r = WaitForMultipleObjects(n, hs, (flags & 1) != 0, 0);    /* MWMO_WAITALL */
            if (r != WAIT_TIMEOUT) return r;
        }
        if (take_message(&m, 0, 0, 0, 0, 0)) return WAIT_OBJECT_0 + n;
        if (mono_ns() >= end) return WAIT_TIMEOUT;
        if (n) {
            DWORD r = WaitForMultipleObjects(n, hs, (flags & 1) != 0, 2);
            if (r != WAIT_TIMEOUT) return r;
        } else {
            bk_wait(2);
        }
    }
}

DWORD WINAPI MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD mask) {
    return MsgWaitForMultipleObjectsEx(n, hs, ms, mask, all ? 1 : 0);
}

/* ---- the default window procedure --------------------------------------------------------- */

HCURSOR WINAPI SetCursor(HCURSOR c);

LRESULT WINAPI DefWindowProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wwnd_t *w = wnd_get(h);
    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        if (w && cs && cs->lpszName && !is_atom(cs->lpszName)) {
            size_t n = w_len(cs->lpszName);
            if (n > 255) n = 255;
            memmove(w->title, cs->lpszName, n * 2);
            w->title[n] = 0;
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_SETTEXT:
        SetWindowTextW(h, (const WCHAR *)lp);
        return TRUE;
    case WM_GETTEXT:
        return GetWindowTextW(h, (WCHAR *)lp, (int)wp);
    case WM_GETTEXTLENGTH:
        return GetWindowTextLengthW(h);
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return 1;              /* HTCLIENT */
    case WM_SETCURSOR:
        if (w && (lp & 0xFFFF) == 1) {
            SetCursor(w->cls->cursor);
            return TRUE;
        }
        return FALSE;
    case WM_MOUSEACTIVATE:
        return 1;              /* MA_ACTIVATE */
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS *p = (const WINDOWPOS *)lp;
        if (!(p->flags & SWP_NOMOVE)) send_msg(h, WM_MOVE, 0, pos_lp(p->x, p->y));
        if (!(p->flags & SWP_NOSIZE) && w)
            send_msg(h, WM_SIZE, w->maximized ? 2 : 0, (LPARAM)((uint32_t)p->cx | ((uint32_t)p->cy << 16)));
        return 0;
    }
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case 0xF060: send_msg(h, WM_CLOSE, 0, 0); return 0;       /* SC_CLOSE */
        case 0xF020: ShowWindow(h, 6); return 0;                  /* SC_MINIMIZE */
        case 0xF030: ShowWindow(h, 3); return 0;                  /* SC_MAXIMIZE */
        case 0xF120: ShowWindow(h, 9); return 0;                  /* SC_RESTORE */
        default: return 0;
        }
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && (lp & (1L << 29))) send_msg(h, WM_SYSCOMMAND, 0xF060, 0);    /* Alt+F4 */
        return 0;
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) && w) SetFocus(h);
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_GETICON:
        return 0;
    case 0x0011:               /* WM_QUERYENDSESSION */
    case 0x0013:               /* WM_QUERYOPEN */
        return TRUE;
    default:
        return 0;
    }
}

LRESULT WINAPI DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_SETTEXT && lp) {
        WCHAR t[256];
        utf8_to_w((const char *)lp, -1, t, 256);
        return DefWindowProcW(h, m, wp, (LPARAM)t);
    }
    if (m == WM_GETTEXT) return GetWindowTextA(h, (char *)lp, (int)wp);
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT WINAPI DefRawInputProc(void *in, int n, UINT hs) {
    (void)in, (void)n, (void)hs;
    return 0;
}

/* ---- raw input ------------------------------------------------------------------------------- */

typedef struct {
    USHORT usUsagePage, usUsage;
    DWORD  dwFlags;
    HWND   hwndTarget;
} RAWINPUTDEVICE;

BOOL WINAPI RegisterRawInputDevices(const RAWINPUTDEVICE *d, UINT n, UINT size) {
    (void)size;
    for (UINT i = 0; i < n; i++) {
        int on = !(d[i].dwFlags & 0x1);       /* RIDEV_REMOVE */
        if (d[i].usUsagePage != 1) continue;
        if (d[i].usUsage == 2) raw_mouse = on;
        else if (d[i].usUsage == 6) raw_keyboard = on;
    }
    if (wtrace) wlog("raw input: mouse %d, keyboard %d", raw_mouse, raw_keyboard);
    update_pointer_lock();
    return TRUE;
}

UINT WINAPI GetRegisteredRawInputDevices(RAWINPUTDEVICE *d, UINT *n, UINT size) {
    UINT k = 0;
    (void)size;
    if (!d) {
        *n = (UINT)(raw_mouse + raw_keyboard);
        return 0;
    }
    if (raw_mouse && k < *n) d[k].usUsagePage = 1, d[k].usUsage = 2, d[k].dwFlags = 0, d[k].hwndTarget = 0, k++;
    if (raw_keyboard && k < *n) d[k].usUsagePage = 1, d[k].usUsage = 6, d[k].dwFlags = 0, d[k].hwndTarget = 0, k++;
    return k;
}

#define RID_INPUT  0x10000003
#define RID_HEADER 0x10000005

UINT WINAPI GetRawInputData(HRAWINPUT h, UINT cmd, void *data, UINT *size, UINT hdr) {
    RAWINPUT *r = (RAWINPUT *)h;
    UINT need;
    (void)hdr;
    if (!r || r < raw_ring || r >= raw_ring + RAW_RING) return (UINT)-1;
    need = cmd == RID_HEADER ? 24 : r->dwSize;
    if (!data) {
        *size = need;
        return 0;
    }
    if (*size < need) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    memcpy(data, r, need);
    return need;
}

UINT WINAPI GetRawInputBuffer(void *data, UINT *size, UINT hdr) {
    (void)data, (void)hdr;
    *size = 0;
    return 0;
}

typedef struct {
    HANDLE hDevice;
    DWORD  dwType;
} RAWINPUTDEVICELIST;

UINT WINAPI GetRawInputDeviceList(RAWINPUTDEVICELIST *l, UINT *n, UINT size) {
    (void)size;
    if (!l) {
        *n = 2;
        return 0;
    }
    if (*n < 2) {
        *n = 2;
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    l[0].hDevice = RAW_MOUSE_DEV, l[0].dwType = 0;
    l[1].hDevice = RAW_KEYBOARD_DEV, l[1].dwType = 1;
    return 2;
}

UINT WINAPI GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, void *data, UINT *size) {
    (void)dev, (void)cmd, (void)data;
    if (size) *size = 0;
    return (UINT)-1;
}
UINT WINAPI GetRawInputDeviceInfoW(HANDLE dev, UINT cmd, void *data, UINT *size) {
    return GetRawInputDeviceInfoA(dev, cmd, data, size);
}

/* ---- keyboard ------------------------------------------------------------------------------- */

SHORT WINAPI GetKeyState(int vk) {
    uint8_t s;
    pump_input();
    s = key_state[vk & 0xFF];
    return (SHORT)(((s & 0x80) ? 0x8000 : 0) | (s & 0x01));
}

SHORT WINAPI GetAsyncKeyState(int vk) {
    pump_input();
    return (SHORT)((key_state[vk & 0xFF] & 0x80) ? 0x8000 : 0);
}

BOOL WINAPI GetKeyboardState(BYTE *s) {
    pump_input();
    memcpy(s, key_state, 256);
    return TRUE;
}

BOOL WINAPI SetKeyboardState(BYTE *s) {
    memcpy(key_state, s, 256);
    return TRUE;
}

/* the character a key gives, US layout */
static WCHAR vk_char(UINT vk, int shift) {
    static const char shifted_digits[] = ")!@#$%^&*(";
    if (vk >= 'A' && vk <= 'Z') return (WCHAR)(shift ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return (WCHAR)(shift ? (UINT)(uint8_t)shifted_digits[vk - '0'] : vk);
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x1B: return 27;
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    default:
        if (vk >= 0x60 && vk <= 0x69) return (WCHAR)('0' + vk - 0x60);
        return 0;
    }
}

UINT WINAPI MapVirtualKeyW(UINT code, UINT type) {
    switch (type) {
    case 0: case 4: {                     /* MAPVK_VK_TO_VSC(_EX) */
        for (int sc = 1; sc < 0x200; sc++) {
            UINT v = vk_of(sc);
            if (v == code || vk_generic(v) == code) return (sc & 0x100) ? (UINT)(type == 4 ? 0xE000 | (sc & 0xFF) : (sc & 0xFF)) : (UINT)sc;
        }
        return 0;
    }
    case 1: return vk_generic(vk_of((int)(code & 0xFF) | ((code & 0xE000) == 0xE000 ? 0x100 : 0)));   /* VSC_TO_VK */
    case 3: return vk_of((int)(code & 0xFF) | ((code & 0xE000) == 0xE000 ? 0x100 : 0));               /* VSC_TO_VK_EX */
    case 2: {                             /* VK_TO_CHAR: upper case for letters */
        WCHAR c = vk_char(code, 0);
        if (c >= 'a' && c <= 'z') c = (WCHAR)(c - 32);
        return c;
    }
    default: return 0;
    }
}

UINT WINAPI MapVirtualKeyA(UINT code, UINT type) { return MapVirtualKeyW(code, type); }
UINT WINAPI MapVirtualKeyExW(UINT code, UINT type, HKL l) {
    (void)l;
    return MapVirtualKeyW(code, type);
}

int WINAPI ToUnicodeEx(UINT vk, UINT sc, const BYTE *state, WCHAR *buf, int n, UINT flags, HKL l) {
    int shift = state && ((state[0x10] | state[0xA0] | state[0xA1]) & 0x80);
    WCHAR c;
    (void)sc, (void)flags, (void)l;
    if (state && (state[0x14] & 1) && vk >= 'A' && vk <= 'Z') shift = !shift;
    c = vk_char(vk, shift);
    if (!c || n < 1) return 0;
    buf[0] = c;
    if (n > 1) buf[1] = 0;
    return 1;
}

int WINAPI ToUnicode(UINT vk, UINT sc, const BYTE *state, WCHAR *buf, int n, UINT flags) {
    return ToUnicodeEx(vk, sc, state, buf, n, flags, 0);
}

int WINAPI ToAscii(UINT vk, UINT sc, const BYTE *state, WORD *buf, UINT flags) {
    WCHAR w[2];
    int r = ToUnicodeEx(vk, sc, state, w, 2, flags, 0);
    if (r > 0) *buf = w[0];
    return r;
}

int WINAPI GetKeyNameTextW(LONG lp, WCHAR *buf, int n) {
    int sc = (int)((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0x100 : 0);
    UINT vk = vk_of(sc);
    char name[32];
    WCHAR c = vk_char(vk, 1);
    if (c > 32 && c < 127) snprintf(name, sizeof(name), "%c", (char)c);
    else if (vk >= 0x70 && vk <= 0x7B) snprintf(name, sizeof(name), "F%u", vk - 0x6F);
    else switch (vk) {
        case 0x20: snprintf(name, sizeof(name), "Space"); break;
        case 0x0D: snprintf(name, sizeof(name), "Enter"); break;
        case 0x1B: snprintf(name, sizeof(name), "Esc"); break;
        case 0x09: snprintf(name, sizeof(name), "Tab"); break;
        case 0x08: snprintf(name, sizeof(name), "Backspace"); break;
        case 0xA0: case 0xA1: snprintf(name, sizeof(name), "Shift"); break;
        case 0xA2: case 0xA3: snprintf(name, sizeof(name), "Ctrl"); break;
        case 0xA4: case 0xA5: snprintf(name, sizeof(name), "Alt"); break;
        default: snprintf(name, sizeof(name), "Key %d", sc); break;
        }
    return (int)utf8_to_w(name, -1, buf, n) - 1;
}

SHORT WINAPI VkKeyScanW(WCHAR c) {
    for (UINT vk = 0; vk < 256; vk++) {
        if (vk_char(vk, 0) == c) return (SHORT)vk;
        if (vk_char(vk, 1) == c) return (SHORT)(vk | 0x100);
    }
    return -1;
}
SHORT WINAPI VkKeyScanExW(WCHAR c, HKL l) {
    (void)l;
    return VkKeyScanW(c);
}

HKL WINAPI GetKeyboardLayout(DWORD tid) {
    (void)tid;
    return (HKL)(uintptr_t)0x04090409;
}
UINT WINAPI GetKeyboardLayoutList(int n, HKL *l) {
    if (n >= 1 && l) l[0] = (HKL)(uintptr_t)0x04090409;
    return 1;
}
HKL WINAPI ActivateKeyboardLayout(HKL l, UINT f) {
    (void)f;
    return l;
}
BOOL WINAPI GetKeyboardLayoutNameW(WCHAR *n) {
    utf8_to_w("00000409", -1, n, 9);
    return TRUE;
}
int WINAPI GetKeyboardType(int what) {
    return what == 0 ? 4 : what == 2 ? 12 : 0;
}

BOOL WINAPI AttachThreadInput(DWORD a, DWORD b, BOOL f) {
    (void)a, (void)b, (void)f;
    return TRUE;
}
BOOL WINAPI BlockInput(BOOL b) {
    (void)b;
    return TRUE;
}

typedef struct {
    DWORD cbSize, dwFlags;
    HWND  hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT;

BOOL WINAPI TrackMouseEvent(TRACKMOUSEEVENT *t) {
    (void)t;
    return TRUE;
}

UINT WINAPI SendInput(UINT n, void *in, int size) {
    (void)in, (void)size;
    return n;
}

/* ---- the pointer ------------------------------------------------------------------------------ */

static HCURSOR cur_cursor;

HCURSOR WINAPI LoadCursorW(HINSTANCE inst, const WCHAR *name) {
    (void)inst;
    /* system cursors: a handle that says which */
    return (HCURSOR)(uintptr_t)(0xC0000 + ((uintptr_t)name & 0xFFFF));
}
HCURSOR WINAPI LoadCursorA(HINSTANCE inst, const char *name) { return LoadCursorW(inst, (const WCHAR *)name); }
HICON WINAPI LoadIconW(HINSTANCE inst, const WCHAR *name) {
    (void)inst, (void)name;
    return (HICON)(uintptr_t)0xC1001;
}
HICON WINAPI LoadIconA(HINSTANCE inst, const char *name) { return LoadIconW(inst, (const WCHAR *)name); }
HANDLE WINAPI LoadImageW(HINSTANCE inst, const WCHAR *name, UINT type, int cx, int cy, UINT f) {
    (void)cx, (void)cy, (void)f;
    if (type == 2) return LoadCursorW(inst, name);   /* IMAGE_CURSOR */
    return LoadIconW(inst, name);
}
HANDLE WINAPI LoadImageA(HINSTANCE inst, const char *name, UINT type, int cx, int cy, UINT f) {
    return LoadImageW(inst, (const WCHAR *)name, type, cx, cy, f);
}
HICON WINAPI CreateIconIndirect(void *info) {
    (void)info;
    return (HICON)(uintptr_t)0xC2000;
}
HICON WINAPI CreateIconFromResource(BYTE *b, DWORD n, BOOL icon, DWORD ver) {
    (void)b, (void)n, (void)icon, (void)ver;
    return (HICON)(uintptr_t)0xC2001;
}
HICON WINAPI CreateIconFromResourceEx(BYTE *b, DWORD n, BOOL icon, DWORD ver, int cx, int cy, UINT f) {
    (void)b, (void)n, (void)icon, (void)ver, (void)cx, (void)cy, (void)f;
    return (HICON)(uintptr_t)0xC2001;
}
HANDLE WINAPI CopyImage(HANDLE h, UINT type, int cx, int cy, UINT f) {
    (void)type, (void)cx, (void)cy, (void)f;
    return h;
}
BOOL WINAPI DestroyIcon(HICON h) {
    (void)h;
    return TRUE;
}
BOOL WINAPI DestroyCursor(HCURSOR h) {
    (void)h;
    return TRUE;
}
HCURSOR WINAPI CreateCursor(HINSTANCE i, int x, int y, int w, int h, const void *a, const void *b) {
    (void)i, (void)x, (void)y, (void)w, (void)h, (void)a, (void)b;
    return (HCURSOR)(uintptr_t)0xC2002;
}
BOOL WINAPI GetIconInfo(HICON h, void *info) {
    (void)h;
    memset(info, 0, 32);
    return TRUE;
}
BOOL WINAPI DrawIconEx(HDC dc, int x, int y, HICON i, int w, int h, UINT s, HBRUSH b, UINT f) {
    (void)dc, (void)x, (void)y, (void)i, (void)w, (void)h, (void)s, (void)b, (void)f;
    return TRUE;
}

HCURSOR WINAPI SetCursor(HCURSOR c) {
    HCURSOR old = cur_cursor;
    int hidden = c == 0;
    cur_cursor = c;
    if (hidden != cursor_hidden) {
        cursor_hidden = hidden;
        update_pointer_lock();
    }
    if (!hidden) bk_cursor(((uintptr_t)c & 0xFFFF) == 32513 ? 1 : 0);    /* IDC_IBEAM: text */
    return old;
}
HCURSOR WINAPI GetCursor(void) { return cur_cursor; }

int WINAPI ShowCursor(BOOL show) {
    show_count += show ? 1 : -1;
    update_pointer_lock();
    return show_count;
}

BOOL WINAPI GetCursorPos(POINT *p) {
    wwnd_t *w = wnd_main();
    pump_input();
    p->x = mouse_x + (w ? w->rect.left : 0);
    p->y = mouse_y + (w ? w->rect.top : 0);
    return TRUE;
}

BOOL WINAPI GetPhysicalCursorPos(POINT *p) { return GetCursorPos(p); }

BOOL WINAPI SetCursorPos(int x, int y) {
    wwnd_t *w = wnd_main();
    /* the pointer is the desktop's: only where the program thinks it is changes */
    mouse_x = x - (w ? w->rect.left : 0);
    mouse_y = y - (w ? w->rect.top : 0);
    return TRUE;
}

static RECT clip_rect;
BOOL WINAPI ClipCursor(const RECT *r) {
    int was = cursor_clipped;
    cursor_clipped = r != 0;
    if (r) clip_rect = *r;
    if (was != cursor_clipped) update_pointer_lock();
    return TRUE;
}
BOOL WINAPI GetClipCursor(RECT *r) {
    if (cursor_clipped) *r = clip_rect;
    else {
        int sw, sh;
        bk_screen(&sw, &sh);
        r->left = r->top = 0, r->right = sw, r->bottom = sh;
    }
    return TRUE;
}

typedef struct {
    DWORD   cbSize, flags;
    HCURSOR hCursor;
    POINT   ptScreenPos;
} CURSORINFO;

BOOL WINAPI GetCursorInfo(CURSORINFO *ci) {
    ci->flags = (cursor_hidden || show_count < 0) ? 0 : 1;
    ci->hCursor = cur_cursor;
    GetCursorPos(&ci->ptScreenPos);
    return TRUE;
}

UINT WINAPI GetDoubleClickTime(void) { return 500; }

/* ---- displays ------------------------------------------------------------------------------- */

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

int WINAPI GetSystemMetrics(int i) {
    int sw, sh;
    bk_screen(&sw, &sh);
    switch (i) {
    case SM_CXSCREEN: case 78: case 16: case 61: return sw;    /* +VIRTUALSCREEN, FULLSCREEN, MAXIMIZED */
    case SM_CYSCREEN: case 79: case 17: case 62: return sh;
    case 76: case 77: return 0;                                /* SM_X/YVIRTUALSCREEN */
    case 80: return 1;                                         /* SM_CMONITORS */
    case 2: case 3: return 17;                                 /* scroll bars */
    case 4: return 0;                                          /* SM_CYCAPTION: no frame */
    case 5: case 6: case 7: case 8: case 32: case 33: return 0; /* borders and frames */
    case 11: case 12: return 32;                               /* SM_CXICON */
    case 13: case 14: return 32;                               /* SM_CXCURSOR */
    case 19: return 1;                                         /* SM_MOUSEPRESENT */
    case 23: return 0;                                         /* SM_SWAPBUTTON */
    case 36: case 37: return 4;                                /* double click area */
    case 43: return 3;                                         /* SM_CMOUSEBUTTONS */
    case 49: case 50: return 16;                               /* SM_CXSMICON */
    case 75: return 1;                                         /* SM_MOUSEWHEELPRESENT */
    case 86: return 0;                                         /* SM_TABLETPC */
    case 0x1000: return 0;                                     /* SM_REMOTESESSION */
    default: return 0;
    }
}

int WINAPI GetSystemMetricsForDpi(int i, UINT dpi) {
    (void)dpi;
    return GetSystemMetrics(i);
}

typedef struct {
    DWORD cbSize;
    RECT  rcMonitor, rcWork;
    DWORD dwFlags;
    WCHAR szDevice[32];
} MONITORINFOEXW;

#define MONITOR_HANDLE ((HMONITOR)(uintptr_t)0x10001)

HMONITOR WINAPI MonitorFromWindow(HWND h, DWORD f) {
    (void)h, (void)f;
    return MONITOR_HANDLE;
}
HMONITOR WINAPI MonitorFromPoint(POINT p, DWORD f) {
    (void)p, (void)f;
    return MONITOR_HANDLE;
}
HMONITOR WINAPI MonitorFromRect(const RECT *r, DWORD f) {
    (void)r, (void)f;
    return MONITOR_HANDLE;
}

BOOL WINAPI GetMonitorInfoW(HMONITOR m, MONITORINFOEXW *mi) {
    int sw, sh;
    (void)m;
    bk_screen(&sw, &sh);
    mi->rcMonitor.left = mi->rcMonitor.top = 0;
    mi->rcMonitor.right = sw, mi->rcMonitor.bottom = sh;
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = 1;          /* MONITORINFOF_PRIMARY */
    if (mi->cbSize >= sizeof(MONITORINFOEXW)) utf8_to_w("\\\\.\\DISPLAY1", -1, mi->szDevice, 32);
    return TRUE;
}

BOOL WINAPI GetMonitorInfoA(HMONITOR m, void *mi) {
    MONITORINFOEXW w;
    DWORD sz = *(DWORD *)mi;
    w.cbSize = sizeof(w);
    GetMonitorInfoW(m, &w);
    memcpy((uint8_t *)mi + 4, (uint8_t *)&w + 4, 36);
    if (sz >= 72) snprintf((char *)mi + 40, 32, "\\\\.\\DISPLAY1");
    return TRUE;
}

typedef BOOL (WCALLBACK *MONITORENUMPROC)(HMONITOR, HDC, RECT *, LPARAM);
BOOL WINAPI EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp) {
    RECT r;
    int sw, sh;
    (void)clip;
    bk_screen(&sw, &sh);
    r.left = r.top = 0, r.right = sw, r.bottom = sh;
    fn(MONITOR_HANDLE, dc, &r, lp);
    return TRUE;
}

typedef struct {
    DWORD cb;
    WCHAR DeviceName[32];
    WCHAR DeviceString[128];
    DWORD StateFlags;
    WCHAR DeviceID[128];
    WCHAR DeviceKey[128];
} DISPLAY_DEVICEW;

BOOL WINAPI EnumDisplayDevicesW(const WCHAR *dev, DWORD i, DISPLAY_DEVICEW *d, DWORD f) {
    (void)f;
    if (i > 0) return FALSE;
    memset((uint8_t *)d + 4, 0, sizeof(*d) - 4);
    if (!dev) {      /* the adapter */
        utf8_to_w("\\\\.\\DISPLAY1", -1, d->DeviceName, 32);
        utf8_to_w("ICDA Display", -1, d->DeviceString, 128);
        d->StateFlags = 0x1 | 0x4;        /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    } else {         /* its monitor */
        utf8_to_w("\\\\.\\DISPLAY1\\Monitor0", -1, d->DeviceName, 32);
        utf8_to_w("ICDA Monitor", -1, d->DeviceString, 128);
        d->StateFlags = 0x1 | 0x2;        /* ACTIVE | ATTACHED */
    }
    return TRUE;
}

BOOL WINAPI EnumDisplayDevicesA(const char *dev, DWORD i, void *d, DWORD f) {
    DISPLAY_DEVICEW w;
    uint8_t *o = d;
    (void)f;
    w.cb = sizeof(w);
    if (!EnumDisplayDevicesW(dev ? (const WCHAR *)u"x" : 0, i, &w, 0)) return FALSE;
    memset(o + 4, 0, 32 + 128 + 4 + 128 + 128);
    w_to_utf8(w.DeviceName, -1, (char *)o + 4, 32);
    w_to_utf8(w.DeviceString, -1, (char *)o + 36, 128);
    *(DWORD *)(o + 164) = w.StateFlags;
    return TRUE;
}

/* display modes: the desktop's, and common smaller ones (a program that
 * picks one gets a window of that size) */
static const int modes[][2] = { { 0, 0 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 1024 }, { 1366, 768 }, { 1280, 800 },
                                { 1280, 720 }, { 1024, 768 }, { 800, 600 }, { 640, 480 } };

static int mode_at(int i, int *w, int *h) {
    int sw, sh, k = 0;
    bk_screen(&sw, &sh);
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        int mw = m ? modes[m][0] : sw, mh = m ? modes[m][1] : sh;
        if (m && (mw > sw || mh > sh || (mw == sw && mh == sh))) continue;
        if (k == i) {
            *w = mw, *h = mh;
            return 1;
        }
        k++;
    }
    return 0;
}

static void fill_devmode(uint8_t *dm, int w, int h, int wide) {
    /* the offsets after the device name differ by the name's width (32 WCHARs or chars) */
    int o = wide ? 64 : 32, form = wide ? 64 : 32;
    *(WORD *)(dm + o + 4) = (WORD)(wide ? 220 : 156);          /* dmSize */
    *(DWORD *)(dm + o + 8) = 0x40000 | 0x80000 | 0x100000 | 0x400000 | 0x20 | 0x200000;   /* dmFields */
    *(int32_t *)(dm + o + 12) = 0;                             /* dmPosition */
    *(int32_t *)(dm + o + 16) = 0;
    *(DWORD *)(dm + o + 20) = 0;                               /* dmDisplayOrientation */
    {
        int b = o + 38 + form;                                  /* after dmFormName */
        *(WORD *)(dm + b) = 96;                                 /* dmLogPixels */
        *(DWORD *)(dm + b + 2) = 32;                            /* dmBitsPerPel */
        *(DWORD *)(dm + b + 6) = (DWORD)w;                      /* dmPelsWidth */
        *(DWORD *)(dm + b + 10) = (DWORD)h;                     /* dmPelsHeight */
        *(DWORD *)(dm + b + 14) = 0;                            /* dmDisplayFlags */
        *(DWORD *)(dm + b + 18) = 60;                           /* dmDisplayFrequency */
    }
}

BOOL WINAPI EnumDisplaySettingsExW(const WCHAR *dev, DWORD i, void *dm, DWORD f) {
    int w, h;
    (void)dev, (void)f;
    if (i == (DWORD)-1 || i == (DWORD)-2) i = 0;             /* ENUM_CURRENT / REGISTRY_SETTINGS */
    if (!mode_at((int)i, &w, &h)) return FALSE;
    memset((uint8_t *)dm + 64, 0, 220 - 64);
    utf8_to_w("ICDA", -1, (WCHAR *)dm, 32);
    fill_devmode(dm, w, h, 1);
    return TRUE;
}

BOOL WINAPI EnumDisplaySettingsW(const WCHAR *dev, DWORD i, void *dm) { return EnumDisplaySettingsExW(dev, i, dm, 0); }

BOOL WINAPI EnumDisplaySettingsA(const char *dev, DWORD i, void *dm) {
    int w, h;
    (void)dev;
    if (i == (DWORD)-1 || i == (DWORD)-2) i = 0;
    if (!mode_at((int)i, &w, &h)) return FALSE;
    memset((uint8_t *)dm + 32, 0, 156 - 32);
    snprintf(dm, 32, "ICDA");
    fill_devmode(dm, w, h, 0);
    return TRUE;
}

LONG WINAPI ChangeDisplaySettingsExW(const WCHAR *dev, void *dm, HWND h, DWORD f, void *p) {
    (void)dev, (void)dm, (void)h, (void)f, (void)p;
    return 0;     /* DISP_CHANGE_SUCCESSFUL: the window takes the size instead */
}
LONG WINAPI ChangeDisplaySettingsW(void *dm, DWORD f) { return ChangeDisplaySettingsExW(0, dm, 0, f, 0); }
LONG WINAPI ChangeDisplaySettingsA(void *dm, DWORD f) { return ChangeDisplaySettingsExW(0, dm, 0, f, 0); }
LONG WINAPI ChangeDisplaySettingsExA(const char *dev, void *dm, HWND h, DWORD f, void *p) {
    return ChangeDisplaySettingsExW(0, dm, h, f, p);
}

LONG WINAPI GetDisplayConfigBufferSizes(uint32_t f, uint32_t *np, uint32_t *nm) {
    (void)f;
    *np = 0, *nm = 0;
    return ERROR_NOT_SUPPORTED;
}

/* DPI: 96, not scaled */
BOOL WINAPI SetProcessDPIAware(void) { return TRUE; }
BOOL WINAPI IsProcessDPIAware(void) { return TRUE; }
UINT WINAPI GetDpiForWindow(HWND h) {
    (void)h;
    return 96;
}
UINT WINAPI GetDpiForSystem(void) { return 96; }

/* SystemParametersInfo: the work area and a few settings */
BOOL WINAPI SystemParametersInfoW(UINT a, UINT u, void *p, UINT w) {
    (void)w;
    switch (a) {
    case 0x30: {                       /* SPI_GETWORKAREA */
        int sw, sh;
        RECT *r = p;
        bk_screen(&sw, &sh);
        r->left = r->top = 0, r->right = sw, r->bottom = sh;
        return TRUE;
    }
    case 0x70:                         /* SPI_GETMOUSESPEED */
        *(int *)p = 10;
        return TRUE;
    case 0x03: {                       /* SPI_GETMOUSE: thresholds and acceleration */
        int *v = p;
        v[0] = 6, v[1] = 10, v[2] = 1;
        return TRUE;
    }
    case 0x68:                         /* SPI_GETWHEELSCROLLLINES */
    case 0x6C:                         /* SPI_GETWHEELSCROLLCHARS */
        *(UINT *)p = 3;
        return TRUE;
    case 0x10: case 0x0E: case 0x1E:   /* screen saver */
        if (p) *(BOOL *)p = FALSE;
        return TRUE;
    case 0x2029:                       /* SPI_GETMOUSESONAR etc. */
    default:
        if (p && u <= 4096 && a < 0x1000 && (a & 1) == 0 && a != 0x30) {
            /* a GET: zeros are a safe answer */
        }
        return TRUE;
    }
}

BOOL WINAPI SystemParametersInfoA(UINT a, UINT u, void *p, UINT w) { return SystemParametersInfoW(a, u, p, w); }
BOOL WINAPI SystemParametersInfoForDpi(UINT a, UINT u, void *p, UINT w, UINT dpi) {
    (void)dpi;
    return SystemParametersInfoW(a, u, p, w);
}

DWORD WINAPI GetSysColor(int i) {
    static const DWORD c[31] = { 0xC8C8C8, 0, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0, 0, 0, 0xB4B4B4,
                                 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0, 0,
                                 0xFFFFFF, 0x696969, 0xE3E3E3, 0, 0xE1FFFF, 0, 0xCC6600, 0xEAD1B9, 0xF2E4D7,
                                 0xFF9933, 0xF0F0F0 };
    return i >= 0 && i < 31 ? c[i] : 0;
}
HBRUSH WINAPI GetSysColorBrush(int i) { return (HBRUSH)(uintptr_t)(0xB0000 + (i & 0xFF)); }

/* ---- the clipboard: the program's own ---------------------------------------------------------- */

static void *clip_data;
static UINT clip_format;
static DWORD clip_seq = 1;
static int clip_open;

BOOL WINAPI OpenClipboard(HWND h) {
    (void)h;
    if (clip_open) return FALSE;
    clip_open = 1;
    return TRUE;
}
BOOL WINAPI CloseClipboard(void) {
    clip_open = 0;
    return TRUE;
}
BOOL WINAPI EmptyClipboard(void) {
    free(clip_data);
    clip_data = 0;
    clip_format = 0;
    clip_seq++;
    return TRUE;
}
HANDLE WINAPI SetClipboardData(UINT f, HANDLE d) {
    free(clip_data);
    clip_data = d;          /* GlobalAlloc'd: ours now */
    clip_format = f;
    clip_seq++;
    return d;
}
HANDLE WINAPI GetClipboardData(UINT f) { return (f == clip_format) ? clip_data : 0; }
BOOL WINAPI IsClipboardFormatAvailable(UINT f) { return clip_data && f == clip_format; }
DWORD WINAPI GetClipboardSequenceNumber(void) { return clip_seq; }
UINT WINAPI EnumClipboardFormats(UINT f) { return f == 0 && clip_data ? clip_format : 0; }
int WINAPI CountClipboardFormats(void) { return clip_data != 0; }
HWND WINAPI GetClipboardOwner(void) { return 0; }
BOOL WINAPI AddClipboardFormatListener(HWND h) {
    (void)h;
    return TRUE;
}
BOOL WINAPI RemoveClipboardFormatListener(HWND h) {
    (void)h;
    return TRUE;
}
UINT WINAPI RegisterClipboardFormatW(const WCHAR *n) { return RegisterWindowMessageW(n); }
UINT WINAPI RegisterClipboardFormatA(const char *n) { return RegisterWindowMessageA(n); }

/* ---- message boxes: ICDA's dialogs ---------------------------------------------------------------- */

static int dialog_ask(const char *title, const char *text, UINT type) {
    const char *kind, *buttons;
    int answer;
    switch (type & 0xF0) {
    case 0x10: kind = "error"; break;          /* MB_ICONERROR */
    case 0x20: kind = "question"; break;
    case 0x30: kind = "warning"; break;
    default: kind = "info"; break;
    }
    switch (type & 0xF) {
    case 1: buttons = "OK|Cancel"; break;
    case 3: buttons = "Yes|No|Cancel"; break;
    case 4: buttons = "Yes|No"; break;
    case 5: buttons = "Retry|Cancel"; break;
    default: buttons = "OK"; break;
    }
    wlog("message box \"%s\": %s", title, text);
    answer = dialog_run(kind, title, text, buttons, bk_window_id());
    /* the button's index to Windows's answer; no answer is the last button */
    switch (type & 0xF) {
    case 1: return answer == 0 ? 1 : 2;                       /* IDOK, IDCANCEL */
    case 3: return answer == 0 ? 6 : answer == 1 ? 7 : 2;     /* IDYES, IDNO, IDCANCEL */
    case 4: return answer == 0 ? 6 : 7;
    case 5: return answer == 0 ? 4 : 2;                       /* IDRETRY */
    default: return 1;
    }
}

int WINAPI MessageBoxW(HWND h, const WCHAR *text, const WCHAR *caption, UINT type) {
    char t[2048], c[256];
    (void)h;
    w_to_utf8(text ? text : (const WCHAR *)u"", -1, t, sizeof(t));
    w_to_utf8(caption ? caption : (const WCHAR *)u"Error", -1, c, sizeof(c));
    return dialog_ask(c, t, type);
}

int WINAPI MessageBoxA(HWND h, const char *text, const char *caption, UINT type) {
    (void)h;
    return dialog_ask(caption ? caption : "Error", text ? text : "", type);
}

int WINAPI MessageBoxExW(HWND h, const WCHAR *text, const WCHAR *caption, UINT type, WORD lang) {
    (void)lang;
    return MessageBoxW(h, text, caption, type);
}

/* dialog templates (SDL's message box): the texts are read out and asked
 * as an ICDA dialog */
static const WCHAR *tmpl_skip(const WCHAR *p) {
    if (*p == 0xFFFF) return p + 2;
    while (*p) p++;
    return p + 1;
}

INT_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE inst, const void *tmpl, HWND parent, void *proc, LPARAM lp) {
    const uint8_t *t = tmpl;
    const WCHAR *p;
    char title[256] = "", text[2048] = "", buttons[512] = "";
    int ex = ((const uint16_t *)t)[1] == 0xFFFF, nitems;
    int ids[16], nb = 0;
    DWORD style;
    (void)inst, (void)parent, (void)proc, (void)lp;
    if (ex) {
        style = *(const DWORD *)(t + 12);
        nitems = *(const uint16_t *)(t + 16);
        p = (const WCHAR *)(t + 26);
    } else {
        style = *(const DWORD *)t;
        nitems = *(const uint16_t *)(t + 8);
        p = (const WCHAR *)(t + 18);
    }
    p = tmpl_skip(p);                                      /* menu */
    p = tmpl_skip(p);                                      /* class */
    w_to_utf8(p, -1, title, sizeof(title));
    p = tmpl_skip(p);
    if (style & 0x40) {                                    /* DS_SETFONT */
        p += ex ? 3 : 1;
        p = tmpl_skip(p);
    }
    for (int i = 0; i < nitems && i < 32; i++) {
        const uint8_t *it = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
        const WCHAR *q;
        DWORD id;
        int cls = 0;
        char s[512];
        if (ex) {
            id = *(const DWORD *)(it + 20);
            q = (const WCHAR *)(it + 24);
        } else {
            id = *(const uint16_t *)(it + 16);
            q = (const WCHAR *)(it + 18);
        }
        if (*q == 0xFFFF) cls = q[1], q += 2;
        else q = tmpl_skip(q);
        if (*q == 0xFFFF) s[0] = 0, q += 2;
        else {
            w_to_utf8(q, -1, s, sizeof(s));
            q = tmpl_skip(q);
        }
        q += 1 + (*q / 2);                                 /* creation data */
        p = q;
        if (cls == 0x80 && nb < 16) {                      /* a button */
            if (buttons[0]) strncat(buttons, ",", sizeof(buttons) - strlen(buttons) - 1);
            strncat(buttons, s, sizeof(buttons) - strlen(buttons) - 1);
            ids[nb++] = (int)id;
        } else if (cls == 0x82 && s[0]) {                  /* static text */
            if (text[0]) strncat(text, "\n", sizeof(text) - strlen(text) - 1);
            strncat(text, s, sizeof(text) - strlen(text) - 1);
        }
    }
    {
        int answer;
        wlog("dialog \"%s\": %s [%s]", title, text, buttons);
        for (char *c = buttons; *c; c++)
            if (*c == ',') *c = '|';
        answer = dialog_run("info", title, text, buttons[0] ? buttons : "OK", bk_window_id());
        if (answer < 0 || answer >= nb) answer = nb ? nb - 1 : 0;
        return nb ? ids[answer] : 1;
    }
}

INT_PTR WINAPI DialogBoxParamW(HINSTANCE i, const WCHAR *t, HWND p, void *proc, LPARAM lp) {
    (void)i, (void)t, (void)p, (void)proc, (void)lp;
    return -1;
}
BOOL WINAPI EndDialog(HWND h, INT_PTR r) {
    (void)h, (void)r;
    return TRUE;
}
HWND WINAPI CreateDialogIndirectParamW(HINSTANCE i, const void *t, HWND p, void *proc, LPARAM lp) {
    (void)i, (void)t, (void)p, (void)proc, (void)lp;
    return 0;
}
BOOL WINAPI IsDialogMessageW(HWND h, MSG *m) {
    (void)h, (void)m;
    return FALSE;
}
LRESULT WINAPI SendDlgItemMessageW(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) {
    return SendMessageW(GetDlgItem(h, id), m, wp, lp);
}
BOOL WINAPI SetDlgItemTextW(HWND h, int id, const WCHAR *t) { return SetWindowTextW(GetDlgItem(h, id), t); }

/* ---- hooks: kept, never called -------------------------------------------------------------- */

HHOOK WINAPI SetWindowsHookExW(int id, void *fn, HINSTANCE m, DWORD tid) {
    (void)id, (void)fn, (void)m, (void)tid;
    return (HHOOK)(uintptr_t)0xA0000 + id;
}
HHOOK WINAPI SetWindowsHookExA(int id, void *fn, HINSTANCE m, DWORD tid) { return SetWindowsHookExW(id, fn, m, tid); }
BOOL WINAPI UnhookWindowsHookEx(HHOOK h) {
    (void)h;
    return TRUE;
}
LRESULT WINAPI CallNextHookEx(HHOOK h, int code, WPARAM wp, LPARAM lp) {
    (void)h, (void)code, (void)wp, (void)lp;
    return 0;
}

HANDLE WINAPI RegisterDeviceNotificationW(HANDLE r, void *f, DWORD fl) {
    (void)r, (void)f, (void)fl;
    return (HANDLE)(uintptr_t)0xD0000;
}
HANDLE WINAPI RegisterDeviceNotificationA(HANDLE r, void *f, DWORD fl) { return RegisterDeviceNotificationW(r, f, fl); }
BOOL WINAPI UnregisterDeviceNotification(HANDLE h) {
    (void)h;
    return TRUE;
}
HANDLE WINAPI RegisterPowerSettingNotification(HANDLE r, const GUID *g, DWORD f) {
    (void)r, (void)g, (void)f;
    return (HANDLE)(uintptr_t)0xD0001;
}
BOOL WINAPI UnregisterPowerSettingNotification(HANDLE h) {
    (void)h;
    return TRUE;
}

/* ---- rectangles ------------------------------------------------------------------------------ */

BOOL WINAPI SetRect(RECT *r, int l, int t, int ri, int b) {
    r->left = l, r->top = t, r->right = ri, r->bottom = b;
    return TRUE;
}
BOOL WINAPI SetRectEmpty(RECT *r) {
    memset(r, 0, sizeof(*r));
    return TRUE;
}
BOOL WINAPI CopyRect(RECT *d, const RECT *s) {
    *d = *s;
    return TRUE;
}
BOOL WINAPI IsRectEmpty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
BOOL WINAPI PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
BOOL WINAPI OffsetRect(RECT *r, int dx, int dy) {
    r->left += dx, r->right += dx, r->top += dy, r->bottom += dy;
    return TRUE;
}
BOOL WINAPI InflateRect(RECT *r, int dx, int dy) {
    r->left -= dx, r->right += dx, r->top -= dy, r->bottom += dy;
    return TRUE;
}
BOOL WINAPI IntersectRect(RECT *d, const RECT *a, const RECT *b) {
    RECT r;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (r.right <= r.left || r.bottom <= r.top) {
        memset(d, 0, sizeof(*d));
        return FALSE;
    }
    *d = r;
    return TRUE;
}
BOOL WINAPI UnionRect(RECT *d, const RECT *a, const RECT *b) {
    if (IsRectEmpty(a)) {
        *d = *b;
        return !IsRectEmpty(b);
    }
    if (IsRectEmpty(b)) {
        *d = *a;
        return TRUE;
    }
    d->left = a->left < b->left ? a->left : b->left;
    d->top = a->top < b->top ? a->top : b->top;
    d->right = a->right > b->right ? a->right : b->right;
    d->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return TRUE;
}
BOOL WINAPI EqualRect(const RECT *a, const RECT *b) { return !memcmp(a, b, sizeof(RECT)); }

/* ---- text helpers --------------------------------------------------------------------------- */

WCHAR *WINAPI CharUpperW(WCHAR *s) {
    WCHAR win_towupper(WCHAR c);
    if ((uintptr_t)s < 0x10000) return (WCHAR *)(uintptr_t)win_towupper((WCHAR)(uintptr_t)s);
    for (WCHAR *p = s; *p; p++) *p = win_towupper(*p);
    return s;
}
WCHAR *WINAPI CharLowerW(WCHAR *s) {
    WCHAR win_towlower(WCHAR c);
    if ((uintptr_t)s < 0x10000) return (WCHAR *)(uintptr_t)win_towlower((WCHAR)(uintptr_t)s);
    for (WCHAR *p = s; *p; p++) *p = win_towlower(*p);
    return s;
}
char *WINAPI CharUpperA(char *s) {
    if ((uintptr_t)s < 0x10000) {
        int c = (int)(uintptr_t)s;
        return (char *)(uintptr_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    for (char *p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
char *WINAPI CharLowerA(char *s) {
    if ((uintptr_t)s < 0x10000) {
        int c = (int)(uintptr_t)s;
        return (char *)(uintptr_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    a_lower(s);
    return s;
}
DWORD WINAPI CharUpperBuffW(WCHAR *s, DWORD n) {
    WCHAR win_towupper(WCHAR c);
    for (DWORD i = 0; i < n; i++) s[i] = win_towupper(s[i]);
    return n;
}
WCHAR *WINAPI CharNextW(const WCHAR *s) { return (WCHAR *)(*s ? s + 1 : s); }
char *WINAPI CharNextA(const char *s) { return (char *)(*s ? s + 1 : s); }
WCHAR *WINAPI CharPrevW(const WCHAR *start, const WCHAR *s) { return (WCHAR *)(s > start ? s - 1 : start); }
BOOL WINAPI IsCharAlphaW(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0; }
BOOL WINAPI IsCharAlphaNumericW(WCHAR c) { return IsCharAlphaW(c) || (c >= '0' && c <= '9'); }

int WINAPI wvsprintfA(char *out, const char *fmt, ms_va_list ap) { return ms_vformat(out, 1024, fmt, ap, 0); }
int WINAPI wvsprintfW(WCHAR *out, const WCHAR *fmt, ms_va_list ap) { return ms_vformatw(out, 1024, fmt, ap); }

int WINAPI wsprintfA(char *out, const char *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformat(out, 1024, fmt, ap, 0);
    __builtin_ms_va_end(ap);
    return n;
}

int WINAPI wsprintfW(WCHAR *out, const WCHAR *fmt, ...) {
    ms_va_list ap;
    int n;
    __builtin_ms_va_start(ap, fmt);
    n = ms_vformatw(out, 1024, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

int WINAPI LoadStringW(HINSTANCE inst, UINT id, WCHAR *buf, int n) {
    /* string tables: 16 strings a block, block id/16+1 */
    wmodule_t *m = ld_by_handle(inst);
    DWORD size = 0;
    const WCHAR *p = ld_resource(m, (const WCHAR *)(uintptr_t)6, (const WCHAR *)(uintptr_t)(id / 16 + 1), &size);
    if (!p) {
        if (n > 0) buf[0] = 0;
        return 0;
    }
    for (UINT i = 0; i < id % 16; i++) p += 1 + *p;
    if (n == 0) {
        *(const WCHAR **)buf = p + 1;
        return *p;
    }
    {
        int len = *p < n - 1 ? *p : n - 1;
        memcpy(buf, p + 1, (size_t)len * 2);
        buf[len] = 0;
        return len;
    }
}

int WINAPI LoadStringA(HINSTANCE inst, UINT id, char *buf, int n) {
    WCHAR w[1024];
    int r = LoadStringW(inst, id, w, 1024);
    if (r <= 0) {
        if (n > 0) buf[0] = 0;
        return 0;
    }
    w_to_utf8(w, -1, buf, n);
    return (int)strlen(buf);
}

/* ---- the export table --------------------------------------------------------------------- */

/* gdi32.c has these, user32 exports them */
HDC WINAPI GetDC(HWND h);
HDC WINAPI GetDCEx(HWND h, HRGN r, DWORD f);
HDC WINAPI GetWindowDC(HWND h);
int WINAPI ReleaseDC(HWND h, HDC dc);
int WINAPI FillRect(HDC dc, const RECT *r, HBRUSH b);
int WINAPI FrameRect(HDC dc, const RECT *r, HBRUSH b);
int WINAPI DrawTextW(HDC dc, const WCHAR *s, int n, RECT *r, UINT f);
int WINAPI DrawTextExW(HDC dc, WCHAR *s, int n, RECT *r, UINT f, void *p);
int WINAPI DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT f);
BOOL WINAPI DrawEdge(HDC dc, RECT *r, UINT e, UINT f);
BOOL WINAPI DrawFocusRect(HDC dc, const RECT *r);
BOOL WINAPI InvertRect(HDC dc, const RECT *r);

#define X(f) WEXP(f)
static const wexport_t u32[] = {
    X(RegisterClassExW), X(RegisterClassExA), X(RegisterClassW), X(RegisterClassA), X(UnregisterClassW),
    X(UnregisterClassA), X(GetClassInfoExW), X(GetClassInfoExA), X(GetClassInfoW), X(CreateWindowExW),
    X(CreateWindowExA), X(DestroyWindow), X(IsWindow), X(IsWindowVisible), X(IsWindowEnabled), X(EnableWindow),
    X(IsIconic), X(IsZoomed), X(IsChild), X(GetParent), X(GetAncestor), X(SetParent), X(GetDesktopWindow),
    X(GetShellWindow), X(GetWindow), X(GetTopWindow), X(GetWindowThreadProcessId), X(EnumWindows),
    X(EnumThreadWindows), X(EnumChildWindows), X(FindWindowW), X(FindWindowA), X(SetPropW), X(GetPropW),
    X(RemovePropW), X(SetPropA), X(GetPropA), X(RemovePropA), X(GetWindowLongPtrW), X(SetWindowLongPtrW),
    WEXPN("GetWindowLongPtrA", GetWindowLongPtrW), WEXPN("SetWindowLongPtrA", SetWindowLongPtrW), X(GetWindowLongW),
    X(SetWindowLongW), WEXPN("GetWindowLongA", GetWindowLongW), WEXPN("SetWindowLongA", SetWindowLongW),
    X(GetClassLongPtrW), X(SetClassLongPtrW), WEXPN("GetClassLongPtrA", GetClassLongPtrW),
    WEXPN("SetClassLongPtrA", SetClassLongPtrW), X(GetClassLongW), X(SetClassLongW),
    WEXPN("GetClassLongA", GetClassLongW), WEXPN("SetClassLongA", SetClassLongW), X(GetClassNameW), X(GetClassNameA),
    X(SetWindowTextW), X(SetWindowTextA), X(GetWindowTextW), X(GetWindowTextA), X(GetWindowTextLengthW),
    X(GetWindowTextLengthA), X(GetWindowRect), X(GetClientRect), X(ClientToScreen), X(ScreenToClient),
    X(MapWindowPoints), X(AdjustWindowRectEx), X(AdjustWindowRect), X(AdjustWindowRectExForDpi), X(SetWindowPos),
    X(MoveWindow), X(ShowWindow), X(ShowWindowAsync), X(UpdateWindow), X(BringWindowToTop), X(SetForegroundWindow),
    X(GetForegroundWindow), X(GetActiveWindow), X(SetActiveWindow), X(GetFocus), X(SetFocus), X(GetCapture),
    X(SetCapture), X(ReleaseCapture), X(WindowFromPoint), X(ChildWindowFromPoint), X(FlashWindowEx), X(FlashWindow),
    X(SetLayeredWindowAttributes), X(SetWindowRgn), X(GetWindowPlacement), X(SetWindowPlacement), X(GetMenu),
    X(GetSystemMenu), X(EnableMenuItem), X(DrawMenuBar), X(GetDlgItem), X(BeginPaint), X(EndPaint), X(InvalidateRect),
    X(ValidateRect), X(GetUpdateRect), X(RedrawWindow), X(InvalidateRgn), X(PostMessageW), X(PostMessageA),
    X(PostThreadMessageW), X(PostThreadMessageA), X(PostQuitMessage), X(SendMessageW), X(SendMessageA),
    X(SendMessageTimeoutW), X(SendNotifyMessageW), X(CallWindowProcW), X(CallWindowProcA), X(RegisterWindowMessageA),
    X(RegisterWindowMessageW), X(SetTimer), X(KillTimer), X(PeekMessageW), X(PeekMessageA), X(GetMessageW),
    X(GetMessageA), X(WaitMessage), X(TranslateMessage), X(DispatchMessageW), X(DispatchMessageA), X(GetMessageTime),
    X(GetMessageExtraInfo), X(GetMessagePos), X(GetQueueStatus), X(GetInputState), X(InSendMessage), X(ReplyMessage),
    X(MsgWaitForMultipleObjectsEx), X(MsgWaitForMultipleObjects), X(DefWindowProcW), X(DefWindowProcA),
    X(DefRawInputProc), X(RegisterRawInputDevices), X(GetRegisteredRawInputDevices), X(GetRawInputData),
    X(GetRawInputBuffer), X(GetRawInputDeviceList), X(GetRawInputDeviceInfoA), X(GetRawInputDeviceInfoW),
    X(GetKeyState), X(GetAsyncKeyState), X(GetKeyboardState), X(SetKeyboardState), X(MapVirtualKeyW),
    X(MapVirtualKeyA), X(MapVirtualKeyExW), X(ToUnicodeEx), X(ToUnicode), X(ToAscii), X(GetKeyNameTextW),
    X(VkKeyScanW), X(VkKeyScanExW), X(GetKeyboardLayout), X(GetKeyboardLayoutList), X(ActivateKeyboardLayout),
    X(GetKeyboardLayoutNameW), X(GetKeyboardType), X(AttachThreadInput), X(BlockInput), X(TrackMouseEvent),
    X(SendInput), X(LoadCursorW), X(LoadCursorA), X(LoadIconW), X(LoadIconA), X(LoadImageW), X(LoadImageA),
    X(CreateIconIndirect), X(CreateIconFromResource), X(CreateIconFromResourceEx), X(CopyImage), X(DestroyIcon),
    X(DestroyCursor), X(CreateCursor), X(GetIconInfo), X(DrawIconEx), X(SetCursor), X(GetCursor), X(ShowCursor),
    X(GetCursorPos), X(GetPhysicalCursorPos), X(SetCursorPos), X(ClipCursor), X(GetClipCursor), X(GetCursorInfo),
    X(GetDoubleClickTime), X(GetSystemMetrics), X(GetSystemMetricsForDpi), X(MonitorFromWindow), X(MonitorFromPoint),
    X(MonitorFromRect), X(GetMonitorInfoW), X(GetMonitorInfoA), X(EnumDisplayMonitors), X(EnumDisplayDevicesW),
    X(EnumDisplayDevicesA), X(EnumDisplaySettingsExW), X(EnumDisplaySettingsW), X(EnumDisplaySettingsA),
    X(ChangeDisplaySettingsExW), X(ChangeDisplaySettingsW), X(ChangeDisplaySettingsA), X(ChangeDisplaySettingsExA),
    X(GetDisplayConfigBufferSizes), X(SetProcessDPIAware), X(IsProcessDPIAware), X(GetDpiForWindow),
    X(GetDpiForSystem), X(SystemParametersInfoW), X(SystemParametersInfoA), X(SystemParametersInfoForDpi),
    X(GetSysColor), X(GetSysColorBrush), X(OpenClipboard), X(CloseClipboard), X(EmptyClipboard), X(SetClipboardData),
    X(GetClipboardData), X(IsClipboardFormatAvailable), X(GetClipboardSequenceNumber), X(EnumClipboardFormats),
    X(CountClipboardFormats), X(GetClipboardOwner), X(AddClipboardFormatListener), X(RemoveClipboardFormatListener),
    X(RegisterClipboardFormatW), X(RegisterClipboardFormatA), X(MessageBoxW), X(MessageBoxA), X(MessageBoxExW),
    X(DialogBoxIndirectParamW), X(DialogBoxParamW), X(EndDialog), X(CreateDialogIndirectParamW), X(IsDialogMessageW),
    X(SendDlgItemMessageW), X(SetDlgItemTextW), X(SetWindowsHookExW), X(SetWindowsHookExA), X(UnhookWindowsHookEx),
    X(CallNextHookEx), X(RegisterDeviceNotificationW), X(RegisterDeviceNotificationA), X(UnregisterDeviceNotification),
    X(RegisterPowerSettingNotification), X(UnregisterPowerSettingNotification), X(SetRect), X(SetRectEmpty),
    X(CopyRect), X(IsRectEmpty), X(PtInRect), X(OffsetRect), X(InflateRect), X(IntersectRect), X(UnionRect),
    X(EqualRect), X(CharUpperW), X(CharLowerW), X(CharUpperA), X(CharLowerA), X(CharUpperBuffW), X(CharNextW),
    X(CharNextA), X(CharPrevW), X(IsCharAlphaW), X(IsCharAlphaNumericW), X(wvsprintfA), X(wvsprintfW), X(wsprintfA),
    X(wsprintfW), X(LoadStringW), X(LoadStringA), X(GetDC), X(GetDCEx), X(GetWindowDC), X(ReleaseDC), X(FillRect),
    X(FrameRect), X(DrawTextW), X(DrawTextA), X(DrawTextExW), X(DrawEdge), X(DrawFocusRect), X(InvertRect),
};

const wdll_t dll_user32 = { "user32.dll", u32, sizeof(u32) / sizeof(u32[0]) };
