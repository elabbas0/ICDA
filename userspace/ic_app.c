


#include "ic_app.h"
#include "ic_time.h"
#include "ic_theme.h"
#include "ic_font.h"
#include "gui.h"
#include "icda_sys.h"

#define IC_CARET_PERIOD_MS 1060u
#define IC_APPEARANCE_POLL_MS 1000u



typedef struct {
    int     state;     
    uint32_t param;
    uint32_t param2;
    uint32_t param3;
    uint32_t esc_ms;
} ic_keydec_t;

#define IC_ESC_TIMEOUT_MS 50

static float ic_wheel_vel = 0.0f;
static uint32_t ic_wheel_last_ms = 0;
static uint32_t ic_wheel_coast_ms = 0;

static void ic_app_emit_scroll(ic_app_t *app, int wheel) {
    ic_event_t ev;
    ev.type = IC_EV_SCROLL;
    ev.x = app->mouse_x;
    ev.y = app->mouse_y;
    ev.button = 0;
    ev.key = 0;
    ev.wheel = wheel;
    ev.mods = 0;
    if (app->desc->event) app->desc->event(app, &ev);
    app->dirty = 1;
}

static void ic_app_wheel_input(int wheel) {
    uint32_t now = ic_time_ms();
    if (now - ic_wheel_last_ms < 120u && (ic_wheel_vel > 0.0f) == (wheel > 0)) ic_wheel_vel += (float)wheel;
    else ic_wheel_vel = (float)wheel;
    if (ic_wheel_vel > 12.0f) ic_wheel_vel = 12.0f;
    if (ic_wheel_vel < -12.0f) ic_wheel_vel = -12.0f;
    ic_wheel_last_ms = now;
    ic_wheel_coast_ms = now;
}

static void ic_app_wheel_coast(ic_app_t *app) {
    uint32_t now = ic_time_ms();
    float speed = ic_wheel_vel < 0.0f ? -ic_wheel_vel : ic_wheel_vel;
    if (speed < 3.0f) return;
    if (now - ic_wheel_last_ms < 80u) return;
    if (now - ic_wheel_coast_ms < 45u) return;
    ic_wheel_coast_ms = now;
    ic_app_emit_scroll(app, ic_wheel_vel > 0.0f ? 1 : -1);
    ic_wheel_vel *= 0.8f;
}

static void ic_app_emit_key_mods(ic_app_t *app, uint32_t key, uint32_t mods) {
    ic_event_t ev;
    ev.type = IC_EV_KEY;
    ev.x = ev.y = 0;
    ev.button = 0;
    ev.key = key;
    ev.wheel = 0;
    ev.mods = mods;
    if (app->desc->event) app->desc->event(app, &ev);
    ic_app_caret_reset(app);
    app->dirty = 1;
}

static void ic_app_emit_key(ic_app_t *app, uint32_t key) {
    ic_app_emit_key_mods(app, key, 0);
}

static void ic_app_emit_plain(ic_app_t *app, uint32_t c, uint32_t mods) {
    if (c == 127) {
        ic_app_emit_key_mods(app, IC_KEY_BACKSPACE, mods);
    } else if (c >= 1 && c <= 26 && c != IC_KEY_BACKSPACE && c != IC_KEY_TAB && c != IC_KEY_ENTER) {
        ic_app_emit_key_mods(app, 'a' + c - 1, mods | IC_MOD_CTRL);
    } else {
        if (c >= 'A' && c <= 'Z') mods |= IC_MOD_SHIFT;
        ic_app_emit_key_mods(app, c, mods);
    }
}

static void ic_app_feed_key(ic_app_t *app, ic_keydec_t *d, uint32_t c) {
    if (d->state == 0) {
        if (c == 27) {
            d->state = 1;
            d->esc_ms = ic_time_ms();
            return;
        }
        ic_app_emit_plain(app, c, 0);
        return;
    }
    if (d->state == 1) {
        if (c == '[') {
            d->state = 2;
            d->param = 0;
            d->param2 = 0;
            d->param3 = 0;
            return;
        }
        d->state = 0;
        if (c >= 32 && c < 127) {
            ic_app_emit_plain(app, c, IC_MOD_ALT);
            return;
        }
        ic_app_emit_key(app, IC_KEY_ESCAPE);
        ic_app_feed_key(app, d, c);
        return;
    }
    if (d->state == 2 || d->state == 3 || d->state == 4 || d->state == 5) {
        uint32_t mods;
        if (c >= '0' && c <= '9') {
            if (d->state == 5) d->param3 = d->param3 * 10 + (c - '0');
            else if (d->state == 4) d->param2 = d->param2 * 10 + (c - '0');
            else d->param = d->param * 10 + (c - '0');
            if (d->state == 2) d->state = 3;
            return;
        }
        if (c == ';') {
            d->state = d->state == 4 ? 5 : 4;
            return;
        }
        d->state = 0;
        mods = d->param2 > 1 ? d->param2 - 1 : 0;
        switch (c) {
        case 'A': ic_app_emit_key_mods(app, IC_KEY_UP, mods); return;
        case 'B': ic_app_emit_key_mods(app, IC_KEY_DOWN, mods); return;
        case 'C': ic_app_emit_key_mods(app, IC_KEY_RIGHT, mods); return;
        case 'D': ic_app_emit_key_mods(app, IC_KEY_LEFT, mods); return;
        case 'H': ic_app_emit_key_mods(app, IC_KEY_HOME, mods); return;
        case 'F': ic_app_emit_key_mods(app, IC_KEY_END, mods); return;
        case 'Z': ic_app_emit_key_mods(app, IC_KEY_TAB, mods | IC_MOD_SHIFT); return;
        case '~':
            if (d->param == 27 && d->param3) ic_app_emit_key_mods(app, d->param3, mods);
            else if (d->param == 3) ic_app_emit_key_mods(app, IC_KEY_DELETE, mods);
            else if (d->param == 2) ic_app_emit_key_mods(app, IC_KEY_INSERT, mods);
            else if (d->param == 1 || d->param == 7) ic_app_emit_key_mods(app, IC_KEY_HOME, mods);
            else if (d->param == 4 || d->param == 8) ic_app_emit_key_mods(app, IC_KEY_END, mods);
            else if (d->param == 5) ic_app_emit_key_mods(app, IC_KEY_PAGE_UP, mods);
            else if (d->param == 6) ic_app_emit_key_mods(app, IC_KEY_PAGE_DOWN, mods);
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
    ev.wheel = 0;
    ev.mods = 0;
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
            ic_app_set_cursor(app, IC_CURSOR_ARROW);
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
    if (m->mouse.wheel) {
        ic_app_emit_scroll(app, m->mouse.wheel);
        ic_app_wheel_input(m->mouse.wheel);
    }
}

static void ic_app_paint(ic_app_t *app) {
    int s = gui_window_scale();
    ic_canvas_t c = ic_canvas_make(gui_pixel_buffer(), gui_window_width() * s, gui_window_height() * s);
    c.scale = s;
    app->width = gui_window_width();
    app->height = gui_window_height();
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
    keys.param2 = 0;
    keys.param3 = 0;
    keys.esc_ms = 0;
    app.desc = desc;
    app.user = user;
    app.mouse_x = app.mouse_y = -1;
    ic_time_init();
    ic_palette_reload();
    if (gui_open_window(desc->title, desc->width, desc->height) != 0) return -1;
    if (gui_window_scale() > 1 && gui_font_shm()) {
        uint64_t addr = icda_shm_map(gui_font_shm());
        if (addr) ic_font_attach_2x((const void *)(uintptr_t)addr, 16ULL * 1024ULL * 1024ULL);
    }
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
        
        if (keys.state == 1 && events == 0 && ic_time_ms() - keys.esc_ms >= IC_ESC_TIMEOUT_MS) {
            keys.state = 0;
            ic_app_emit_key(&app, IC_KEY_ESCAPE);
        }
        if (desc->tick) desc->tick(&app);
        ic_app_wheel_coast(&app);

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

void ic_app_set_cursor(ic_app_t *app, int shape) {
    if (!app || app->cursor == shape) return;
    app->cursor = shape;
    gui_set_cursor(shape);
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

int ic_clipboard_set(const char *text, uint64_t len) {
    return icda_write_file(IC_CLIPBOARD_PATH, text, len) == (uint64_t)-1 ? -1 : 0;
}

long ic_clipboard_get(char *buf, uint64_t cap) {
    long n;
    if (cap == 0) return 0;
    n = (long)icda_read_file(IC_CLIPBOARD_PATH, buf, cap - 1);
    if (n < 0) n = 0;
    if ((uint64_t)n >= cap) n = (long)cap - 1;
    buf[n] = 0;
    return n;
}
