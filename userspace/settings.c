/*
 * settings.app - ICDA Settings.
 *
 * A real GUI window with toggles for the Slice C system settings:
 * VSync, window animations, boot animation, and audio. Reads
 * /cfg/icda-settings on start (see settings_store.h), saves on every
 * toggle; the WM re-reads the file live so changes apply without a
 * reboot. Audio clients read the same file before playing.
 */
#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"
#include "settings_store.h"

#include <stdint.h>

#define SET_WIN_W 480
#define SET_WIN_H 400
#define SET_STATUS_CAP 128

#define SET_ROW_X 16
#define SET_ROW_W (SET_WIN_W - 32)
#define SET_ROW_H 30
#define SET_FIRST_Y 78
#define SET_ROW_GAP 36
#define SET_BOX_W 64

static icda_settings_t set_state;
static char set_status[SET_STATUS_CAP];
static int set_row_count = 4;

static void set_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void set_append(char *dst, const char *src, uint64_t cap) {
    uint64_t at = 0;
    uint64_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (dst[at]) {
        at++;
    }
    if (at >= cap) {
        return;
    }
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
}

static int set_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static void set_reload(void) {
    icda_settings_load(&set_state);
}

static void set_persist(const char *what) {
    if (icda_settings_save(&set_state) == 0) {
        set_copy(set_status, what, sizeof(set_status));
        set_append(set_status, " saved", sizeof(set_status));
    } else {
        set_copy(set_status, "Could not save settings", sizeof(set_status));
    }
}

static int set_row_value(int row) {
    if (row == 0) {
        return set_state.vsync;
    }
    if (row == 1) {
        return set_state.animations;
    }
    if (row == 2) {
        return set_state.boot_anim;
    }
    return set_state.audio;
}

static const char *set_row_label(int row) {
    if (row == 0) {
        return "VSync (paced present)";
    }
    if (row == 1) {
        return "Window animations";
    }
    if (row == 2) {
        return "Boot animation";
    }
    return "Audio";
}

static const char *set_row_hint(int row) {
    if (row == 0) {
        return "tick-paced flip, tear-free";
    }
    if (row == 1) {
        return "open/close/min/max fades";
    }
    if (row == 2) {
        return "splash + power fade (WM)";
    }
    return "master mute for players";
}

static void set_toggle(int row) {
    if (row == 0) {
        set_state.vsync = !set_state.vsync;
        set_persist(set_state.vsync ? "VSync on" : "VSync off");
    } else if (row == 1) {
        set_state.animations = !set_state.animations;
        set_persist(set_state.animations ? "Animations on" : "Animations off");
    } else if (row == 2) {
        set_state.boot_anim = !set_state.boot_anim;
        set_persist(set_state.boot_anim ? "Boot anim on" : "Boot anim off");
    } else if (row == 3) {
        set_state.audio = !set_state.audio;
        if (!set_state.audio) {
            icda_stop_audio();
        }
        set_persist(set_state.audio ? "Audio on" : "Audio off");
    }
}

static void set_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) {
        return;
    }
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void set_draw(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int i = 0;
    icda_gpu_info_t gpu;

    gui_fill_rect(0, 0, w, h, 0x00E7F1FF);
    for (int row = 0; row < 64; row++) {
        int t = row, d = 63;
        int r = (int)(0x3D + ((0x1F - 0x3D) * t) / d);
        int g = (int)(0x8B + ((0x5E - 0x8B) * t) / d);
        int b = (int)(0xFF + ((0xBE - 0xFF) * t) / d);
        gui_fill_rect(0, row, w, 1, (uint32_t)((r << 16) | (g << 8) | b));
    }
    gui_fill_rect(0, 63, w, 1, 0x0015449C);
    set_draw_text(16, 16, "ICDA Settings", 0x00FFFFFF, 0x002C73D2, 200);
    set_draw_text(16, 38, "Click or 1-4 toggle   Q close", 0x00EAF2FF, 0x002C73D2, w - 32);

    for (i = 0; i < set_row_count; i++) {
        int y = SET_FIRST_Y + i * SET_ROW_GAP;
        int on = set_row_value(i);
        int bx = SET_ROW_X + SET_ROW_W - SET_BOX_W;
        uint32_t fill = on ? 0x001F9D55 : 0x0094A3B8;
        gui_fill_rect(SET_ROW_X, y, SET_ROW_W, SET_ROW_H, 0x00FFFFFF);
        gui_draw_rect_outline(SET_ROW_X, y, SET_ROW_W, SET_ROW_H, 0x0092B7E8);
        set_draw_text(SET_ROW_X + 10, y + 3, set_row_label(i), 0x001F2937, 0x00FFFFFF,
                      SET_ROW_W - SET_BOX_W - 24);
        set_draw_text(SET_ROW_X + 10, y + 15, set_row_hint(i), 0x0064758B, 0x00FFFFFF,
                      SET_ROW_W - SET_BOX_W - 24);
        gui_fill_rect(bx, y + 4, SET_BOX_W - 8, SET_ROW_H - 8, fill);
        gui_draw_rect_outline(bx, y + 4, SET_BOX_W - 8, SET_ROW_H - 8, 0x0015449C);
        set_draw_text(bx + 12, y + 9, on ? "ON" : "OFF", 0x00FFFFFF, fill, SET_BOX_W - 20);
    }

    /* Present-mode readout (kernel-reported, read-only). */
    if (icda_gpu_query(&gpu) == 0) {
        const char *mode = gpu.flip_active ? "flip (tear-free)" :
                           gpu.needs_present ? "present (dma)" : "fbdev (direct)";
        char line[96];
        set_copy(line, "Present: ", sizeof(line));
        set_append(line, mode, sizeof(line));
        set_draw_text(16, SET_FIRST_Y + set_row_count * SET_ROW_GAP + 4, line,
                      0x00334455, 0x00E7F1FF, w - 32);
    }
    set_draw_text(16, SET_FIRST_Y + set_row_count * SET_ROW_GAP + 22, "Stored in /cfg/icda-settings",
                  0x0064758B, 0x00E7F1FF, w - 32);

    gui_fill_rect(0, h - 30, w, 30, 0x00EAF2FF);
    gui_draw_hline(0, h - 30, w, 0x0092B7E8);
    set_draw_text(12, h - 24, set_status, 0x00334455, 0x00EAF2FF, w - 24);
}

static void set_handle_mouse(gui_msg_t *msg) {
    int mx = msg->mouse.x;
    int my = msg->mouse.y;
    int i = 0;

    for (i = 0; i < set_row_count; i++) {
        int y = SET_FIRST_Y + i * SET_ROW_GAP;
        if (set_hit(mx, my, SET_ROW_X, y, SET_ROW_W, SET_ROW_H)) {
            set_toggle(i);
            return;
        }
    }
}

static void set_handle_key(uint32_t key) {
    if (key == 24 || key == 'q' || key == 'Q') {
        gui_close_window();
        icda_exit(0);
        return;
    }
    if (key >= '1' && key <= '4') {
        set_toggle((int)(key - '1'));
    }
}

int main(int argc, char **argv) {
    int key_seq = 0;
    (void)argc;
    (void)argv;

    set_status[0] = 0;
    set_reload();
    set_copy(set_status, "Toggle a setting - saved instantly", sizeof(set_status));
    if (gui_open_window("Settings", SET_WIN_W, SET_WIN_H) != 0) {
        icda_write("settings requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    set_draw();
    gui_flush();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT && (msg.mouse.buttons & GUI_BTN_LEFT)) {
                set_handle_mouse(&msg);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                uint32_t code = msg.key.keycode;
                if (key_seq == 0 && code == 27) {
                    key_seq = 1;
                } else if (key_seq == 1 && code == '[') {
                    key_seq = 2;
                } else if (key_seq == 2) {
                    key_seq = 0;
                } else {
                    key_seq = 0;
                    set_handle_key(code);
                }
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        if (changed || (icda_ticks() % 8) == 0) {
            set_draw();
            gui_flush();
        }
        icda_sleep(1);
    }
}
