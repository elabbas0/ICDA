/* comctl32: the common controls library.
 *
 * InitCommonControls(Ex), which programs call before making any window
 * (Chrome stops if it fails), and window subclassing (SetWindowSubclass and
 * the rest), which programs use to watch a window's messages without taking
 * its window procedure over.  The controls themselves are not here yet.
 *
 * Subclassing: a window with subclasses has subclass_proc as its window
 * procedure, which calls the newest subclass; DefSubclassProc, called from
 * a subclass, calls the one before it, and the last calls the window's own
 * procedure.  Which subclass is running is per thread, as a stack (one
 * window's subclass may send another window a message). */
#define _GNU_SOURCE
#include "win.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef LRESULT (WCALLBACK *WNDPROC_)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (WCALLBACK *SUBCLASSPROC)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

LONG_PTR WINAPI GetWindowLongPtrW(HWND h, int i);
LONG_PTR WINAPI SetWindowLongPtrW(HWND h, int i, LONG_PTR v);
BOOL     WINAPI IsWindow(HWND h);
BOOL     WINAPI TrackMouseEvent(void *tme);

#define GWLP_WNDPROC (-4)
#define WM_NCDESTROY 0x0082

typedef struct sub {
    struct sub  *older;
    SUBCLASSPROC proc;
    UINT_PTR     id;
    DWORD_PTR    ref;
    int          removed;     /* taken out while running: freed when the window goes */
} sub_t;

typedef struct subbed {
    struct subbed *next;
    HWND           h;
    WNDPROC_       own;       /* the window's procedure before the first subclass */
    sub_t         *newest;
    int            running;   /* calls of subclass_proc for it under way */
} subbed_t;

static subbed_t *subbed;
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { subbed_t *w; sub_t *s; } frame_t;
static __thread frame_t cur;     /* the subclass running on this thread */

static subbed_t *find(HWND h) {
    for (subbed_t *w = subbed; w; w = w->next)
        if (w->h == h) return w;
    return 0;
}

static sub_t *live(sub_t *s) {
    while (s && s->removed) s = s->older;
    return s;
}

/* the window's subclasses end: its own procedure back */
static void unsubclass(subbed_t *w) {
    for (subbed_t **p = &subbed; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            break;
        }
    while (w->newest) {
        sub_t *s = w->newest;
        w->newest = s->older;
        free(s);
    }
    free(w);
}

static LRESULT call_from(subbed_t *w, sub_t *s, HWND h, UINT m, WPARAM wp, LPARAM lp) {
    frame_t outer = cur;
    LRESULT r;
    s = live(s);
    cur.w = w, cur.s = s;
    r = s ? s->proc(h, m, wp, lp, s->id, s->ref) : (w->own ? w->own(h, m, wp, lp) : 0);
    cur = outer;
    return r;
}

static LRESULT WCALLBACK subclass_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    subbed_t *w;
    LRESULT r;
    pthread_mutex_lock(&slock);
    w = find(h);
    if (w) w->running++;
    pthread_mutex_unlock(&slock);
    if (!w) return 0;
    r = call_from(w, w->newest, h, m, wp, lp);
    pthread_mutex_lock(&slock);
    w->running--;
    /* the window is going: its subclasses go with it, as on Windows */
    if (m == WM_NCDESTROY && !w->running) {
        if ((WNDPROC_)GetWindowLongPtrW(h, GWLP_WNDPROC) == subclass_proc) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)w->own);
        unsubclass(w);
    }
    pthread_mutex_unlock(&slock);
    return r;
}

static BOOL WINAPI SetWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR ref) {
    subbed_t *w;
    sub_t *s;
    if (!proc || !IsWindow(h)) return FALSE;
    pthread_mutex_lock(&slock);
    w = find(h);
    if (!w) {
        w = calloc(1, sizeof(*w));
        if (!w) {
            pthread_mutex_unlock(&slock);
            return FALSE;
        }
        w->h = h;
        w->own = (WNDPROC_)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
        w->next = subbed, subbed = w;
    }
    for (s = w->newest; s; s = s->older)
        if (s->proc == proc && s->id == id && !s->removed) {
            s->ref = ref;      /* the same one again: its data replaced */
            pthread_mutex_unlock(&slock);
            return TRUE;
        }
    s = calloc(1, sizeof(*s));
    if (!s) {
        pthread_mutex_unlock(&slock);
        return FALSE;
    }
    s->proc = proc, s->id = id, s->ref = ref;
    s->older = w->newest, w->newest = s;
    pthread_mutex_unlock(&slock);
    return TRUE;
}

static BOOL WINAPI GetWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR *ref) {
    subbed_t *w;
    BOOL found = FALSE;
    pthread_mutex_lock(&slock);
    w = find(h);
    for (sub_t *s = w ? w->newest : 0; s; s = s->older)
        if (s->proc == proc && s->id == id && !s->removed) {
            if (ref) *ref = s->ref;
            found = TRUE;
            break;
        }
    pthread_mutex_unlock(&slock);
    if (!found && ref) *ref = 0;
    return found;
}

static BOOL WINAPI RemoveWindowSubclass(HWND h, SUBCLASSPROC proc, UINT_PTR id) {
    subbed_t *w;
    BOOL found = FALSE;
    pthread_mutex_lock(&slock);
    w = find(h);
    for (sub_t **p = w ? &w->newest : 0; p && *p; p = &(*p)->older) {
        sub_t *s = *p;
        if (s->proc != proc || s->id != id || s->removed) continue;
        found = TRUE;
        if (w->running) s->removed = 1;          /* it may be on a thread's stack */
        else *p = s->older, free(s);
        break;
    }
    /* none left: the window's own procedure back, unless something has
     * replaced subclass_proc since */
    if (w && !live(w->newest) && !w->running) {
        if ((WNDPROC_)GetWindowLongPtrW(h, GWLP_WNDPROC) == subclass_proc) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)w->own);
        unsubclass(w);
    }
    pthread_mutex_unlock(&slock);
    return found;
}

/* the subclass before the running one, or the window's own procedure */
static LRESULT WINAPI DefSubclassProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    subbed_t *w = cur.w;
    if (!w || w->h != h) {
        pthread_mutex_lock(&slock);
        w = find(h);
        pthread_mutex_unlock(&slock);
        return w && w->own ? w->own(h, m, wp, lp) : 0;
    }
    return call_from(w, cur.s ? cur.s->older : 0, h, m, wp, lp);
}

/* INITCOMMONCONTROLSEX: its size (8) and which classes */
static BOOL WINAPI InitCommonControlsEx(const DWORD *icc) {
    if (!icc || icc[0] != 8) return FALSE;
    return TRUE;
}
static void WINAPI InitCommonControls(void) {}

static BOOL WINAPI c__TrackMouseEvent(void *tme) { return TrackMouseEvent(tme); }

static const wexport_t cc[] = {
    WEXPO("InitCommonControls", InitCommonControls, 17), WEXP(InitCommonControlsEx),
    WEXPO("SetWindowSubclass", SetWindowSubclass, 410), WEXPO("GetWindowSubclass", GetWindowSubclass, 411),
    WEXPO("RemoveWindowSubclass", RemoveWindowSubclass, 412), WEXPO("DefSubclassProc", DefSubclassProc, 413),
    WEXPN("_TrackMouseEvent", c__TrackMouseEvent),
};
const wdll_t dll_comctl32 = { "comctl32.dll", cc, sizeof(cc) / sizeof(cc[0]) };
