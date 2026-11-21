/* What user32, gdi32 and opengl32 share: windows, device contexts and the
 * surfaces programs draw into. */
#ifndef WINLAYER_WINGUI_H
#define WINLAYER_WINGUI_H

#include "win.h"

typedef LRESULT (WCALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
    DWORD  lPrivate;
} MSG;

typedef struct wclass {
    struct wclass *next;
    WCHAR     name[128];
    ATOM      atom;
    UINT      style;
    WNDPROC   proc;
    int       wnd_extra, cls_extra;
    HINSTANCE inst;
    HICON     icon, icon_sm;
    HCURSOR   cursor;
    HBRUSH    bg;
    int       ansi;
    LONG_PTR  extra[8];
} wclass_t;

typedef struct wwnd {
    int        used;
    HWND       h;
    wclass_t  *cls;
    WNDPROC    proc;
    DWORD      style, exstyle;
    HWND       parent, owner;
    LONG_PTR   id, userdata;
    HINSTANCE  inst;
    RECT       rect;             /* screen coordinates; no frame: the window is all client */
    WCHAR      title[256];
    int        visible, minimized, maximized, enabled, destroyed;
    DWORD      tid;              /* the thread that made it (its messages go there) */
    struct { WCHAR name[64]; ATOM atom; HANDLE val; } props[24];
    int        nprops;
    uint8_t   *extra;
    int        nextra;
    int        top;              /* shown in the ICDA window */
    int        invalid;          /* needs WM_PAINT */
    void      *dc;               /* its device context (CS_OWNDC: one) */
    /* where it is drawn: the ICDA window's buffer for the main window,
     * memory of its own otherwise */
    uint32_t  *surface;
    int        sw, sh;
    int        own_surface;
    int        pixel_format;
} wwnd_t;

wwnd_t *wnd_get(HWND h);
wwnd_t *wnd_main(void);                  /* the one in the ICDA window, 0 if none */
void    wnd_surface(wwnd_t *w);          /* makes surface match the client size */
void    wnd_present(wwnd_t *w);          /* shows what was drawn (main window) */

/* device contexts (gdi32.c) */
typedef struct wdc {
    int       magic;
    HWND      hwnd;              /* 0: memory or display DC */
    void     *bitmap;            /* selected bitmap (memory DCs) */
    void     *brush, *pen, *font;
    uint32_t  text_color, bk_color;
    int       bk_mode;
    int       pixel_format;
    int       is_display;
} wdc_t;

wdc_t  *dc_get(HDC h);
HDC     dc_for_window(wwnd_t *w);
wwnd_t *dc_window(HDC h);

/* opengl32.c */
BOOL gl_swap_buffers(HDC dc);
void gl_window_changed(wwnd_t *w);      /* its surface moved: a context drawing there follows */

#endif
