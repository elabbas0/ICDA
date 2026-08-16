/*
 * audioplay.app - ICDA Audio Player.
 *
 * A real GUI window (unlike the old console stub that only knew about a
 * request file nobody wrote).  Lists every .wav in /usr/share/audio and
 * /home, lets you pick one with the mouse or arrow keys, and plays/stops
 * it through the kernel's background audio engine.
 */
#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"

#include <stdint.h>

#define AP_WIN_W 520
#define AP_WIN_H 400
#define AP_MAX_TRACKS 48
#define AP_NAME_CAP 96
#define AP_PATH_CAP 192
#define AP_STATUS_CAP 128

#define AP_LIST_X 14
#define AP_LIST_Y 74
#define AP_LIST_W (AP_WIN_W - 28)
#define AP_ROW_H 20
#define AP_MAX_ROWS 14

static char ap_tracks[AP_MAX_TRACKS][AP_NAME_CAP];
static char ap_paths[AP_MAX_TRACKS][AP_PATH_CAP];
static int ap_count = 0;
static int ap_sel = 0;
static char ap_status[AP_STATUS_CAP];

static uint64_t ap_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ap_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void ap_append(char *dst, const char *src, uint64_t cap) {
    uint64_t at = ap_strlen(dst);
    uint64_t i = 0;
    if (!dst || cap == 0 || at >= cap) return;
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
}

static char ap_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int ap_has_wav_suffix(const char *name) {
    uint64_t len = ap_strlen(name);
    static const char suffix[] = ".wav";
    uint64_t sl = 4;
    if (len < sl) return 0;
    for (uint64_t i = 0; i < sl; i++) {
        if (ap_lower(name[len - sl + i]) != suffix[i]) return 0;
    }
    return 1;
}

static int ap_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static void ap_set_status(const char *text) {
    ap_copy(ap_status, text, sizeof(ap_status));
}

static void ap_add_dir(const char *dir) {
    char buf[4096];
    long rc;
    uint64_t pos = 0;

    if (!dir || !*dir) return;
    rc = (long)icda_list_dir(dir, buf, sizeof(buf));
    if (rc < 0) return;

    while (pos < (uint64_t)rc && ap_count < AP_MAX_TRACKS) {
        char entry[AP_NAME_CAP];
        uint64_t ei = 0;
        int dup = 0;

        while (pos < (uint64_t)rc && buf[pos] != '\n' && ei + 1 < sizeof(entry)) {
            entry[ei++] = buf[pos++];
        }
        while (pos < (uint64_t)rc && buf[pos] != '\n') pos++;
        if (pos < (uint64_t)rc && buf[pos] == '\n') pos++;
        entry[ei] = 0;

        if (ei == 0 || entry[ei - 1] == '/') continue;
        if (!ap_has_wav_suffix(entry)) continue;

        for (int i = 0; i < ap_count; i++) {
            if (ap_strlen(ap_tracks[i]) == ap_strlen(entry)) {
                uint64_t j = 0;
                int same = 1;
                while (entry[j]) {
                    if (ap_tracks[i][j] != entry[j]) { same = 0; break; }
                    j++;
                }
                if (same) { dup = 1; break; }
            }
        }
        if (dup) continue;

        ap_copy(ap_tracks[ap_count], entry, sizeof(ap_tracks[0]));
        if (dir[0] == '/' && ap_strlen(dir) == 1) {
            ap_append(ap_paths[ap_count], "/", sizeof(ap_paths[0]));
        } else {
            ap_append(ap_paths[ap_count], dir, sizeof(ap_paths[0]));
            ap_append(ap_paths[ap_count], "/", sizeof(ap_paths[0]));
        }
        ap_append(ap_paths[ap_count], entry, sizeof(ap_paths[0]));
        ap_count++;
    }
}

static void ap_scan(void) {
    ap_count = 0;
    ap_sel = 0;
    ap_add_dir("/usr/share/audio");
    ap_add_dir("/home");
    if (ap_count == 0) {
        ap_set_status("No .wav files found - put some in /usr/share/audio");
    } else {
        ap_set_status("Select a track and press Play");
    }
}

static void ap_play_selected(void) {
    if (ap_count == 0) {
        ap_set_status("No track selected");
        return;
    }
    if (ap_sel < 0 || ap_sel >= ap_count) ap_sel = 0;
    if ((long)icda_play_audio_file(ap_paths[ap_sel]) < 0) {
        ap_set_status("Could not play track");
    } else {
        char msg[AP_STATUS_CAP];
        ap_copy(msg, "Playing ", sizeof(msg));
        ap_append(msg, ap_tracks[ap_sel], sizeof(msg));
        ap_set_status(msg);
    }
}

static void ap_draw_button(int x, int y, int w, int h, const char *label, int active) {
    uint32_t top = active ? 0x003D8BFF : 0x00FFFFFF;
    uint32_t bottom = active ? 0x001F5EBE : 0x00DDEBFF;
    uint32_t edge = active ? 0x000C3C88 : 0x006EA6E8;
    for (int row = 0; row < h; row++) {
        int t = row;
        int d = h - 1;
        int r = (int)(((top >> 16) & 0xFF) + ((((bottom >> 16) & 0xFF) - ((top >> 16) & 0xFF)) * t) / d);
        int g = (int)(((top >> 8) & 0xFF) + ((((bottom >> 8) & 0xFF) - ((top >> 8) & 0xFF)) * t) / d);
        int b = (int)((top & 0xFF) + (((bottom & 0xFF) - (top & 0xFF)) * t) / d);
        gui_fill_rect(x, y + row, w, 1, (uint32_t)((r << 16) | (g << 8) | b));
    }
    gui_draw_rect_outline(x, y, w, h, edge);
    {
        int cx = x + 8;
        int cy = y + 5;
        const char *s = label;
        while (*s && cx + FONT_CELL_WIDTH <= x + w - 8) {
            gui_draw_char(cx, cy, *s, active ? 0x00FFFFFF : 0x001D3F66, bottom);
            cx += FONT_CELL_WIDTH;
            s++;
        }
    }
}

static void ap_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) return;
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void ap_refresh_status(void) {
    icda_audio_info_t audio;
    if ((long)icda_audio_info(&audio) >= 0 && audio.active) {
        char msg[AP_STATUS_CAP];
        ap_copy(msg, "Playing: ", sizeof(msg));
        ap_append(msg, audio.name, sizeof(msg));
        {
            char num[24];
            uint64_t i = sizeof(num) - 1;
            uint64_t secs = audio.seconds_left;
            num[i] = 0;
            if (secs == 0) {
                num[--i] = '0';
            } else {
                while (secs && i > 0) {
                    num[--i] = (char)('0' + (secs % 10));
                    secs /= 10;
                }
            }
            ap_append(msg, "  ", sizeof(msg));
            ap_append(msg, &num[i], sizeof(msg));
            ap_append(msg, "s left", sizeof(msg));
        }
        ap_set_status(msg);
    } else if (ap_count > 0 && ap_status[0] == 0) {
        ap_set_status("Select a track and press Play");
    }
}

static void ap_draw(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int rows = (h - AP_LIST_Y - 96) / AP_ROW_H;
    if (rows > AP_MAX_ROWS) rows = AP_MAX_ROWS;
    if (rows < 1) rows = 1;

    gui_fill_rect(0, 0, w, h, 0x00E7F1FF);
    for (int row = 0; row < 64; row++) {
        int t = row, d = 63;
        int r = (int)(0x3D + ((0x1F - 0x3D) * t) / d);
        int g = (int)(0x8B + ((0x5E - 0x8B) * t) / d);
        int b = (int)(0xFF + ((0xBE - 0xFF) * t) / d);
        gui_fill_rect(0, row, w, 1, (uint32_t)((r << 16) | (g << 8) | b));
    }
    gui_fill_rect(0, 63, w, 1, 0x0015449C);
    ap_draw_text(16, 16, "ICDA Audio Player", 0x00FFFFFF, 0x002C73D2, 200);
    ap_draw_text(16, 38, "Up/Down select   Enter/Space play   S stop   R refresh   Q close",
                 0x00EAF2FF, 0x002C73D2, w - 32);

    gui_fill_rect(AP_LIST_X, AP_LIST_Y, AP_LIST_W, rows * AP_ROW_H, 0x00FFFFFF);
    gui_draw_rect_outline(AP_LIST_X, AP_LIST_Y, AP_LIST_W, rows * AP_ROW_H, 0x0092B7E8);

    for (int i = 0; i < rows; i++) {
        int idx = i;
        int y = AP_LIST_Y + 2 + i * AP_ROW_H;
        if (idx >= ap_count) break;
        if (idx == ap_sel) {
            gui_fill_rect(AP_LIST_X + 1, AP_LIST_Y + i * AP_ROW_H, AP_LIST_W - 2, AP_ROW_H - 1, 0x00CFE6FF);
            ap_draw_text(AP_LIST_X + 8, y, ap_tracks[idx], 0x001F2937, 0x00CFE6FF, AP_LIST_W - 20);
        } else {
            ap_draw_text(AP_LIST_X + 8, y, ap_tracks[idx], 0x001F2937, 0x00FFFFFF, AP_LIST_W - 20);
        }
    }
    if (ap_count == 0) {
        ap_draw_text(AP_LIST_X + 8, AP_LIST_Y + 6, "(no audio files found)", 0x0064758B, 0x00FFFFFF, AP_LIST_W - 20);
    }

    ap_draw_button(16, h - 58, 70, 28, "Play", ap_count > 0);
    ap_draw_button(96, h - 58, 70, 28, "Stop", 1);
    ap_draw_button(176, h - 58, 80, 28, "Refresh", 1);

    gui_fill_rect(0, h - 30, w, 30, 0x00EAF2FF);
    gui_draw_hline(0, h - 30, w, 0x0092B7E8);
    ap_draw_text(12, h - 24, ap_status, 0x00334455, 0x00EAF2FF, w - 24);
}

static void ap_handle_mouse(gui_msg_t *msg) {
    int mx = msg->mouse.x;
    int my = msg->mouse.y;
    int w = gui_window_width();
    int h = gui_window_height();
    int rows = (h - AP_LIST_Y - 96) / AP_ROW_H;
    int i;

    if (rows > AP_MAX_ROWS) rows = AP_MAX_ROWS;
    if (rows < 1) rows = 1;

    if (ap_hit(mx, my, 16, h - 58, 70, 28)) { ap_play_selected(); return; }
    if (ap_hit(mx, my, 96, h - 58, 70, 28)) { icda_stop_audio(); ap_set_status("Audio stopped"); return; }
    if (ap_hit(mx, my, 176, h - 58, 80, 28)) { ap_scan(); return; }

    if (ap_hit(mx, my, AP_LIST_X, AP_LIST_Y, AP_LIST_W, rows * AP_ROW_H)) {
        i = (my - AP_LIST_Y) / AP_ROW_H;
        if (i >= 0 && i < ap_count) ap_sel = i;
        return;
    }
    (void)w;
}

static void ap_handle_key(uint32_t key) {
    if (key == 24 || key == 'q' || key == 'Q') {
        gui_close_window();
        icda_exit(0);
        return;
    }
    if (key == 3) { /* SPECIAL_UP */ if (ap_sel > 0) ap_sel--; return; }
    if (key == 4) { /* SPECIAL_DOWN */ if (ap_sel + 1 < ap_count) ap_sel++; return; }
    if (key == '\r' || key == '\n' || key == ' ') { ap_play_selected(); return; }
    if (key == 's' || key == 'S') { icda_stop_audio(); ap_set_status("Audio stopped"); return; }
    if (key == 'r' || key == 'R') { ap_scan(); return; }
}

int main(int argc, char **argv) {
    int key_seq = 0;
    (void)argc;
    (void)argv;

    ap_status[0] = 0;
    if (gui_open_window("Audio Player", AP_WIN_W, AP_WIN_H) != 0) {
        /* No desktop running (e.g. booted to a text virtual terminal). */
        icda_write("audio player requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    ap_scan();
    ap_draw();
    gui_flush();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT && (msg.mouse.buttons & GUI_BTN_LEFT)) {
                ap_handle_mouse(&msg);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                uint32_t code = msg.key.keycode;
                if (key_seq == 0 && code == 27) {
                    key_seq = 1;
                } else if (key_seq == 1 && code == '[') {
                    key_seq = 2;
                } else if (key_seq == 2) {
                    key_seq = 0;
                    if (code == 'A') ap_handle_key(3);
                    else if (code == 'B') ap_handle_key(4);
                } else {
                    key_seq = 0;
                    ap_handle_key(code);
                }
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        ap_refresh_status();
        if (changed || (icda_ticks() % 8) == 0) {
            ap_draw();
            gui_flush();
        }
        icda_sleep(1);
    }
}
