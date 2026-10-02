/*
 * ic_app.h - the application runtime.
 *
 * Every GUI app runs on ic_app_run(): it opens the window, turns raw WM
 * messages into typed events (press/release/move/leave, decoded keys,
 * focus, resize, appearance changes), keeps pointer state, and redraws
 * only when something asked for it - continuously while an animation
 * is running, never while idle.
 *
 *     static void draw(ic_app_t *app, ic_canvas_t *c) { ... }
 *     static void event(ic_app_t *app, const ic_event_t *ev) { ... }
 *     int main(void) {
 *         static const ic_app_desc_t desc = { "Settings", 640, 460, 0, draw, event, 0 };
 *         return ic_app_run(&desc, 0);
 *     }
 */
#ifndef USERSPACE_IC_APP_H
#define USERSPACE_IC_APP_H

#include <stdint.h>
#include "ic_gfx.h"

/* Decoded keys: printable ASCII and control characters keep their byte
 * value; navigation keys get codes above 0xFF. */
enum {
    IC_KEY_BACKSPACE = 8,
    IC_KEY_TAB       = 9,
    IC_KEY_ENTER     = 13,
    IC_KEY_ESCAPE    = 27,
    IC_KEY_UP        = 0x100,
    IC_KEY_DOWN,
    IC_KEY_LEFT,
    IC_KEY_RIGHT,
    IC_KEY_DELETE,
    IC_KEY_HOME,
    IC_KEY_END,
    IC_KEY_PAGE_UP,
    IC_KEY_PAGE_DOWN
};

typedef enum {
    IC_EV_MOUSE_MOVE = 1,   /* x, y (also while a button is held) */
    IC_EV_MOUSE_DOWN,       /* x, y, button */
    IC_EV_MOUSE_UP,         /* x, y, button */
    IC_EV_MOUSE_LEAVE,      /* pointer left the window */
    IC_EV_KEY,              /* key */
    IC_EV_FOCUS,            /* window became key */
    IC_EV_BLUR,             /* window lost key status */
    IC_EV_RESIZE,           /* width/height changed (already applied) */
    IC_EV_APPEARANCE        /* palette changed (dark/light/accent) */
} ic_event_type_t;

typedef struct {
    ic_event_type_t type;
    int             x, y;
    uint8_t         button;   /* GUI_BTN_* for DOWN/UP */
    uint32_t        key;
} ic_event_t;

typedef struct ic_app ic_app_t;

typedef struct {
    const char *title;
    int         width, height;
    /* Called once after the window exists. */
    void (*init)(ic_app_t *app);
    /* Paint the whole window into c. */
    void (*draw)(ic_app_t *app, ic_canvas_t *c);
    /* Handle one event. */
    void (*event)(ic_app_t *app, const ic_event_t *ev);
    /* Optional: called every loop iteration (~10 ms) for background
     * work such as polling a child process. */
    void (*tick)(ic_app_t *app);
} ic_app_desc_t;

struct ic_app {
    const ic_app_desc_t *desc;
    void    *user;
    int      width, height;
    int      focused;
    int      mouse_x, mouse_y;  /* last position inside the window */
    int      mouse_inside;
    uint8_t  buttons;           /* currently held GUI_BTN_* */
    /* private */
    int      dirty;
    int      animating;
    int      wants_caret;
    int      quit;
    uint64_t caret_epoch_ms;
};

int  ic_app_run(const ic_app_desc_t *desc, void *user);

/* Schedule a redraw. */
void ic_app_invalidate(ic_app_t *app);
/* Call from draw() while something is still moving: another frame will
 * follow at display rate. */
void ic_app_animate(ic_app_t *app);
/* Caret blink phase; calling it from draw() also keeps the blink going. */
int  ic_app_caret_visible(ic_app_t *app);
/* Restart the blink with the caret visible (after typing / moving). */
void ic_app_caret_reset(ic_app_t *app);
/* Close the window and return from ic_app_run. */
void ic_app_quit(ic_app_t *app);

#endif /* USERSPACE_IC_APP_H */
