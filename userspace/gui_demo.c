/*
 * gui_demo.c - a small GUI app demonstrating the libicda engine.
 *
 * Shows how a proper userland app is written on ICDA:
 *   - plain C with a standard main(argc, argv) entry (see crt0.asm)
 *   - links libicda.o for strings, drawing, icons, theme, widgets
 *   - opens a window, draws with the engine, and handles input events
 *
 * Build & package: see docs/WRITING_APPS.md.
 */
#include "libicda.h"

#define DEMO_W 560
#define DEMO_H 640

typedef struct {
    int btn_hover;
    int btn_pressed;
    int btn_was_down;
    int clicks;
    int right_clicks;
    int right_was_down;
    int slider_val;
    int last_x;
    int last_y;
} demo_state_t;

static ic_rect_t demo_slider_rect(void) {
    ic_rect_t r;
    r.x = 20;
    r.y = 556;
    r.w = 300;
    r.h = 28;
    return r;
}

static ic_rect_t demo_btn_rect(void) {
    ic_rect_t r;
    r.x = 20;
    r.y = 20;
    r.w = 150;
    r.h = 32;
    return r;
}

static int demo_event(void *ud, const gui_msg_t *msg) {
    demo_state_t *st = (demo_state_t *)ud;

    if (msg->type == GUI_MSG_MOUSE_EVENT) {
        st->last_x = msg->mouse.x;
        st->last_y = msg->mouse.y;
        {
            ic_rect_t r = demo_btn_rect();
            int over = ic_hit_rect(msg->mouse.x, msg->mouse.y, r);
            int down = (msg->mouse.buttons & GUI_BTN_LEFT) != 0;
            int rdown = (msg->mouse.buttons & GUI_BTN_RIGHT) != 0;
            st->btn_hover = over;
            st->btn_pressed = over && down;
            if (down && !st->btn_was_down && over) {
                st->clicks++;   /* count on the press edge */
            }
            st->btn_was_down = down;
            if (rdown && !st->right_was_down) {
                st->right_clicks++;   /* right press edge, anywhere */
            }
            st->right_was_down = rdown;
            /* Slider drag with the left button held. */
            {
                ic_rect_t sr = demo_slider_rect();
                if (down && ic_slider_hit(sr, msg->mouse.x, msg->mouse.y)) {
                    st->slider_val = ic_slider_value_from_x(
                        sr, 0, 100, msg->mouse.x);
                }
            }
        }
    }

    if (msg->type == GUI_MSG_KEY_EVENT && msg->key.pressed) {
        if (msg->key.keycode == 27) { /* Esc closes the window */
            return 0;
        }
        if (msg->key.keycode == ' ') {
            st->clicks++;
        }
    }
    return 1;
}

static void demo_draw(void *ud) {
    demo_state_t *st = (demo_state_t *)ud;
    ic_canvas_t c;
    const ic_theme_t *t = ic_theme_default();
    char buf[64];
    int x = 20;
    int y = 90;

    c.px = gui_pixel_buffer();
    c.w = gui_window_width();
    c.h = gui_window_height();
    if (!c.px) return;

    ic_fill(&c, 0x00F4F7FC);
    ic_rect_r(&c, 2, 2, c.w - 4, c.h - 4, 6, 0x00D7E2F1);
    ic_outline_r(&c, 2, 2, c.w - 4, c.h - 4, 6, 0x00AFCBFF);

    /* Title + hint */
    ic_text(&c, 20, 64, "ICDA engine demo - press a key, move the mouse", t->text_muted, 0x00F4F7FC);

    /* A real button widget */
    {
        ic_rect_t r = demo_btn_rect();
        ic_btn_state_t bs = ic_button_state(1, st->btn_hover, st->btn_pressed);
        ic_draw_button(&c, t, r, "Click me", bs);
    }
    ic_uint_to_str((uint64_t)st->clicks, buf, sizeof(buf));
    ic_text(&c, 190, 26, "clicks: ", t->text, 0x00F4F7FC);
    ic_text(&c, 190 + ic_text_width("clicks: "), 26, buf, t->accent, 0x00F4F7FC);
    ic_uint_to_str((uint64_t)st->right_clicks, buf, sizeof(buf));
    ic_text(&c, 290, 26, "right: ", t->text, 0x00F4F7FC);
    ic_text(&c, 290 + ic_text_width("right: "), 26, buf, t->accent, 0x00F4F7FC);

    /* Mouse position */
    ic_uint_to_str((uint64_t)st->last_x, buf, sizeof(buf));
    ic_text(&c, 20, 340, "mouse x: ", t->text_muted, 0x00F4F7FC);
    ic_text(&c, 20 + ic_text_width("mouse x: "), 340, buf, t->text, 0x00F4F7FC);
    ic_uint_to_str((uint64_t)st->last_y, buf, sizeof(buf));
    ic_text(&c, 120, 340, "  y: ", t->text_muted, 0x00F4F7FC);
    ic_text(&c, 120 + ic_text_width("  y: "), 340, buf, t->text, 0x00F4F7FC);

    /* Widget showcase: popup menu, dialog, slider */
    ic_text(&c, 20, 420, "Widgets", t->text_muted, 0x00F4F7FC);
    {
        static const char *items[] = { "New window", "Open...", "Save as..." };
        ic_menu_t menu;
        ic_rect_t sr;
        menu.items[0] = items[0];
        menu.items[1] = items[1];
        menu.items[2] = items[2];
        menu.count = 3;
        menu.selected = 1;
        ic_menu_draw(&c, t, 20, 444, &menu);
        ic_dialog_draw(&c, t, (ic_rect_t){180, 444, 360, 120},
                       "Properties", "demo.app 6,216 bytes");
        sr = demo_slider_rect();
        ic_slider_draw(&c, t, sr, st->slider_val, 0, 100);
        ic_uint_to_str((uint64_t)st->slider_val, buf, sizeof(buf));
        ic_text(&c, 330, 560, buf, t->accent, 0x00F4F7FC);
    }

    /* The builtin icon set, drawn at several sizes */
    {
        static const char *names[] = {
            "terminal", "folder", "music", "disk", "app", "file", "wav",
            "shell", "desktop", "gear", "audio", "editor"
        };
        int n = (int)(sizeof(names) / sizeof(names[0]));
        for (int i = 0; i < n; i++) {
            const ic_icon_t *icon = ic_icon_builtin(names[i]);
            if (!icon) continue;
            ic_icon_draw(&c, x, y, 32, 32, icon);
            ic_text_clip(&c, x, y + 34, names[i], t->text_muted, 0x00F4F7FC, 64);
            x += 84;
            if (x + 84 > c.w) {
                x = 20;
                y += 64;
            }
        }
    }

    ic_text(&c, 20, c.h - 24, "Esc closes the window", t->text_muted, 0x00F4F7FC);
    gui_flush();
}

int main(int argc, char **argv) {
    demo_state_t st;
    (void)argc;
    (void)argv;
    st.btn_hover = 0;
    st.btn_pressed = 0;
    st.btn_was_down = 0;
    st.clicks = 0;
    st.right_clicks = 0;
    st.right_was_down = 0;
    st.slider_val = 30;
    st.last_x = 0;
    st.last_y = 0;
    return ic_run_app("ICDA Demo", DEMO_W, DEMO_H, demo_event, demo_draw, &st);
}
