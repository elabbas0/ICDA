


#include "ic_app.h"
#include "ic_time.h"
#include "ic_theme.h"
#include "gui.h"
#include "icda_sys.h"

#define IC_CARET_PERIOD_MS 1060u
#define IC_APPEARANCE_POLL_MS 1000u



typedef struct {
    int     state;     
    uint32_t param;
} ic_keydec_t;

static void ic_app_emit_key(ic_app_t *app, uint32_t key) {
    ic_event_t ev;
    ev.type = IC_EV_KEY;
    ev.x = ev.y = 0;
    ev.button = 0;
    ev.key = key;
    if (app->desc->event) app->desc->event(app, &ev);
    ic_app_caret_reset(app);
    app->dirty = 1;
}

static void ic_app_feed_key(ic_app_t *app, ic_keydec_t *d, uint32_t c) {
    if (d->state == 0) {
        if (c == 27) {
            d->state = 1;
            return;
        }
        ic_app_emit_key(app, c == 127 ? IC_KEY_BACKSPACE : c);
        return;
    }
    if (d->state == 1) {
        if (c == '[') {
            d->state = 2;
            d->param = 0;
            return;
        }
        d->state = 0;
        ic_app_emit_key(app, IC_KEY_ESCAPE);
        ic_app_feed_key(app, d, c);
        return;
    }
    if (d->state == 2 || d->state == 3) {
        if (c >= '0' && c <= '9') {
            d->param = d->param * 10 + (c - '0');
            d->state = 3;
            return;
        }
        d->state = 0;
        switch (c) {
        case 'A': ic_app_emit_key(app, IC_KEY_UP); return;
        case 'B': ic_app_emit_key(app, IC_KEY_DOWN); return;
        case 'C': ic_app_emit_key(app, IC_KEY_RIGHT); return;
        case 'D': ic_app_emit_key(app, IC_KEY_LEFT); return;
        case 'H': ic_app_emit_key(app, IC_KEY_HOME); return;
        case 'F': ic_app_emit_key(app, IC_KEY_END); return;
        case '~':
            if (d->param == 3) ic_app_emit_key(app, IC_KEY_DELETE);
            else if (d->param == 1 || d->param == 7) ic_app_emit_key(app, IC_KEY_HOME);
            else if (d->param == 4 || d->param == 8) ic_app_emit_key(app, IC_KEY_END);
            else if (d->param == 5) ic_app_emit_key(app, IC_KEY_PAGE_UP);
            else if (d->param == 6) ic_app_emit_key(app, IC_KEY_PAGE_DOWN);
            return;
        default:
            return;
        }
    }
}

static void ic_app_emit(ic_app_t *app, ic_event_type_t type, int x, int y, uint8_t button) {
    ic_event_t ev;
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.button = button;
    ev.key = 0;
    if (app->desc->event) app->desc->event(app, &ev);
    app->dirty = 1;
}

static void ic_app_mouse(ic_app_t *app, const gui_msg_t *m) {
    int x = m->mouse.x, y = m->mouse.y;
    uint8_t now = m->mouse.buttons;
    uint8_t was = app->buttons;
    if (x < 0 && y < 0) {
        if (app->mouse_inside) {
            app->mouse_inside = 0;
            ic_app_emit(app, IC_EV_MOUSE_LEAVE, app->mouse_x, app->mouse_y, 0);
        }
        return;
    }
    app->mouse_inside = x >= 0 && y >= 0 && x < app->width && y < app->height;
    if (x != app->mouse_x || y != app->mouse_y) {
        app->mouse_x = x;
        app->mouse_y = y;
        ic_app_emit(app, IC_EV_MOUSE_MOVE, x, y, 0);
    }
    for (int b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        if ((now & bit) && !(was & bit)) {
            app->buttons |= bit;
            ic_app_emit(app, IC_EV_MOUSE_DOWN, x, y, bit);
        } else if (!(now & bit) && (was & bit)) {
            app->buttons &= (uint8_t)~bit;
            ic_app_emit(app, IC_EV_MOUSE_UP, x, y, bit);
        }
    }
}

static void ic_app_paint(ic_app_t *app) {
    ic_canvas_t c = ic_canvas_make(gui_pixel_buffer(), gui_window_width(), gui_window_height());
    app->width = c.w;
    app->height = c.h;
    app->animating = 0;
    app->wants_caret = 0;
    if (!c.px) return;
    if (app->desc->draw) app->desc->draw(app, &c);
    gui_flush();
    app->dirty = 0;
}

int ic_app_run(const ic_app_desc_t *desc, void *user) {
    ic_app_t app;
    ic_keydec_t keys;
    uint32_t last_caret_phase = 0;
    uint32_t last_appearance_poll;

    if (!desc) return -1;
    for (uint64_t i = 0; i < sizeof(app); i++) ((uint8_t *)&app)[i] = 0;
    keys.state = 0;
    keys.param = 0;
    app.desc = desc;
    app.user = user;
    app.mouse_x = app.mouse_y = -1;
    ic_time_init();
    ic_palette_reload();
    if (gui_open_window(desc->title, desc->width, desc->height) != 0) return -1;
    app.width = gui_window_width();
    app.height = gui_window_height();
    app.focused = 1;
    app.caret_epoch_ms = ic_time_ms();
    last_appearance_poll = ic_time_ms();
    if (desc->init) desc->init(&app);
    ic_app_paint(&app);

    while (!app.quit) {
        gui_msg_t msg;
        int events = 0;
        while (gui_poll_event(&msg)) {
            events++;
            switch (msg.type) {
            case GUI_MSG_CLOSE_WINDOW:
                app.quit = 1;
                break;
            case GUI_MSG_MOUSE_EVENT:
                ic_app_mouse(&app, &msg);
                break;
            case GUI_MSG_KEY_EVENT:
                if (msg.key.pressed) ic_app_feed_key(&app, &keys, msg.key.keycode);
                break;
            case GUI_MSG_FOCUS:
                app.focused = msg.focus.focused ? 1 : 0;
                if (app.focused && ic_palette_reload()) {
                    ic_app_emit(&app, IC_EV_APPEARANCE, 0, 0, 0);
                }
                ic_app_emit(&app, app.focused ? IC_EV_FOCUS : IC_EV_BLUR, 0, 0, 0);
                ic_app_caret_reset(&app);
                break;
            case GUI_MSG_RESIZE:
                
                app.width = gui_window_width();
                app.height = gui_window_height();
                ic_app_emit(&app, IC_EV_RESIZE, 0, 0, 0);
                break;
            default:
                break;
            }
            if (app.quit) break;
        }
        if (app.quit) break;
        
        if (keys.state == 1 && events == 0) {
            keys.state = 0;
            ic_app_emit_key(&app, IC_KEY_ESCAPE);
        }
        if (desc->tick) desc->tick(&app);

        {
            uint32_t now = ic_time_ms();
            if (now - last_appearance_poll >= IC_APPEARANCE_POLL_MS) {
                last_appearance_poll = now;
                if (ic_palette_reload()) ic_app_emit(&app, IC_EV_APPEARANCE, 0, 0, 0);
            }
            if (app.wants_caret) {
                uint32_t phase = (now - (uint32_t)app.caret_epoch_ms) / (IC_CARET_PERIOD_MS / 2);
                if (phase != last_caret_phase) {
                    last_caret_phase = phase;
                    app.dirty = 1;
                }
            }
        }
        if (app.dirty || app.animating) ic_app_paint(&app);
        if (app.quit) break;
        icda_sleep(1);
    }
    gui_close_window();
    return 0;
}

void ic_app_invalidate(ic_app_t *app) {
    if (app) app->dirty = 1;
}

void ic_app_animate(ic_app_t *app) {
    if (app) app->animating = 1;
}

int ic_app_caret_visible(ic_app_t *app) {
    uint32_t elapsed;
    if (!app) return 1;
    app->wants_caret = 1;
    elapsed = ic_time_ms() - (uint32_t)app->caret_epoch_ms;
    return (elapsed % IC_CARET_PERIOD_MS) < IC_CARET_PERIOD_MS / 2;
}

void ic_app_caret_reset(ic_app_t *app) {
    if (app) app->caret_epoch_ms = ic_time_ms();
}

void ic_app_quit(ic_app_t *app) {
    if (app) app->quit = 1;
}
