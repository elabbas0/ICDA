#include "gui_proto.h"
#include "icda_sys.h"
#include "font.h"

#include <stddef.h>

#define MAX_WINDOWS 16
#define BACK_BUFFER_WIDTH 2560
#define BACK_BUFFER_HEIGHT 1600
#define TASKBAR_H 42
#define TITLE_H 26
#define CURSOR_W 12
#define CURSOR_H 19

typedef struct {
    int      valid;
    uint32_t id;
    uint64_t app_queue_handle;
    uint64_t shm_handle;
    uint32_t *pixels;
    int      x;
    int      y;
    int      w;
    int      h;
    int      minimized;
    int      anim;
    char     title[32];
} wm_window_t;

static wm_window_t windows[MAX_WINDOWS];
static int z_order[MAX_WINDOWS];
static int num_windows = 0;
static int focused_window_idx = -1;
static int start_menu_open = 0;

static uint32_t back_buffer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
static icda_fb_info_t fb_info;
static uint32_t *real_fb = NULL;

static const char cursor_bitmap[CURSOR_H][CURSOR_W + 1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X   X..X  ",
    "XX     X..X ",
    "X       X..X",
    "         XX ",
    "            "
};

static void clear_msg(gui_msg_t *msg) {
    for (int i = 0; i < 64; i++) ((uint8_t*)msg)[i] = 0;
}

static uint32_t blend(uint32_t a, uint32_t b, int n, int d) {
    int ar = (int)((a >> 16) & 0xFF);
    int ag = (int)((a >> 8) & 0xFF);
    int ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF);
    int bg = (int)((b >> 8) & 0xFF);
    int bb = (int)(b & 0xFF);
    int r = ar + ((br - ar) * n) / d;
    int g = ag + ((bg - ag) * n) / d;
    int c = ab + ((bb - ab) * n) / d;
    return (uint32_t)((r << 16) | (g << 8) | c);
}

static void put_px(int w, int h, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    back_buffer[y * w + x] = color;
}

static void fill_rect(int w, int h, int x, int y, int rw, int rh, uint32_t color) {
    int x1 = x;
    int y1 = y;
    int x2 = x + rw;
    int y2 = y + rh;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w) x2 = w;
    if (y2 > h) y2 = h;
    for (int cy = y1; cy < y2; cy++) {
        for (int cx = x1; cx < x2; cx++) {
            back_buffer[cy * w + cx] = color;
        }
    }
}

static void gradient_rect(int w, int h, int x, int y, int rw, int rh, uint32_t top, uint32_t bottom) {
    if (rh <= 1) {
        fill_rect(w, h, x, y, rw, rh, top);
        return;
    }
    for (int row = 0; row < rh; row++) {
        fill_rect(w, h, x, y + row, rw, 1, blend(top, bottom, row, rh - 1));
    }
}

static void rect_outline(int w, int h, int x, int y, int rw, int rh, uint32_t color) {
    for (int cx = x; cx < x + rw; cx++) {
        put_px(w, h, cx, y, color);
        put_px(w, h, cx, y + rh - 1, color);
    }
    for (int cy = y; cy < y + rh; cy++) {
        put_px(w, h, x, cy, color);
        put_px(w, h, x + rw - 1, cy, color);
    }
}

static void draw_text(int x, int y, const char *str, uint32_t fg, uint32_t bg, int w, int h) {
    int cx = x;
    while (str && *str) {
        font_draw_char(back_buffer, w, h, w, cx, y, *str, fg, bg);
        cx += FONT_CELL_WIDTH;
        str++;
    }
}

static void draw_text_clip(int x, int y, const char *str, uint32_t fg, uint32_t bg, int max_px, int w, int h) {
    int cx = x;
    while (str && *str && cx + FONT_CELL_WIDTH <= x + max_px) {
        font_draw_char(back_buffer, w, h, w, cx, y, *str, fg, bg);
        cx += FONT_CELL_WIDTH;
        str++;
    }
}

static void draw_cursor(int w, int h, int mx, int my) {
    /* White arrow body, black 1px outline so the pointer stays visible
     * on both the bright desktop and the dark taskbar/menu. */
    for (int cy = 0; cy < CURSOR_H; cy++) {
        int py = my + cy;
        if (py < 0 || py >= h) continue;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            int px = mx + cx;
            if (px < 0 || px >= w) continue;
            if (cursor_bitmap[cy][cx] == 'X') {
                back_buffer[py * w + px] = 0x00FFFFFF;
            }
        }
    }
    for (int cy = 0; cy < CURSOR_H; cy++) {
        for (int cx = 0; cx < CURSOR_W; cx++) {
            if (cursor_bitmap[cy][cx] != 'X') continue;
            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    int cxx = cx + ox;
                    int cyy = cy + oy;
                    if (cxx < 0 || cxx >= CURSOR_W || cyy < 0 || cyy >= CURSOR_H) continue;
                    if (cursor_bitmap[cyy][cxx] == 'X') continue;
                    {
                        int px = mx + cxx;
                        int py = my + cyy;
                        if (px < 0 || px >= w || py < 0 || py >= h) continue;
                        back_buffer[py * w + px] = 0x00000000;
                    }
                }
            }
        }
    }
}

static int hit_rect(int mx, int my, int x, int y, int rw, int rh) {
    return mx >= x && my >= y && mx < x + rw && my < y + rh;
}

static void draw_desktop_icon(int w, int h, int x, int y, const char *label, int kind, int pulse) {
    uint32_t bg = pulse ? 0x55FFFFFF : 0x22FFFFFF;
    (void)bg;
    fill_rect(w, h, x, y, 74, 74, 0x001F6EA6);
    rect_outline(w, h, x, y, 74, 74, 0x0087C6FF);
    if (kind == 0) {
        fill_rect(w, h, x + 16, y + 16, 19, 9, 0x00FFD262);
        fill_rect(w, h, x + 11, y + 24, 46, 30, 0x00F6B840);
        rect_outline(w, h, x + 11, y + 24, 46, 30, 0x009C6A13);
    } else if (kind == 1) {
        fill_rect(w, h, x + 13, y + 14, 48, 38, 0x000F172A);
        rect_outline(w, h, x + 13, y + 14, 48, 38, 0x004ADE80);
        draw_text(x + 20, y + 25, ">_", 0x004ADE80, 0x000F172A, w, h);
    } else if (kind == 2) {
        fill_rect(w, h, x + 32, y + 15, 5, 28, 0x00A855F7);
        fill_rect(w, h, x + 23, y + 42, 18, 10, 0x00A855F7);
        fill_rect(w, h, x + 37, y + 16, 16, 5, 0x00A855F7);
    } else {
        fill_rect(w, h, x + 18, y + 15, 38, 42, 0x00E5E7EB);
        rect_outline(w, h, x + 18, y + 15, 38, 42, 0x0064758B);
    }
    draw_text_clip(x + 4, y + 56, label, 0x00FFFFFF, 0x001F6EA6, 66, w, h);
}

static void draw_background(int w, int h, uint64_t tick) {
    int horizon = (h * 60) / 100;
    for (int y = 0; y < h; y++) {
        uint32_t color;
        if (y < horizon) {
            color = blend(0x0056B6F7, 0x00CBE9FF, y, horizon);
        } else {
            color = blend(0x003FAF4B, 0x001D7D37, y - horizon, h - horizon);
        }
        for (int x = 0; x < w; x++) {
            back_buffer[y * w + x] = color;
        }
    }

    for (int x = 0; x < w; x++) {
        int64_t dx = x - (w / 3);
        int hill = horizon + 28 - (int)((dx * dx) / (w > 0 ? w * 3 : 1));
        if (hill < horizon - 38) hill = horizon - 38;
        if (hill > h - TASKBAR_H) hill = h - TASKBAR_H;
        for (int y = hill; y < h - TASKBAR_H; y++) {
            back_buffer[y * w + x] = blend(0x0078C850, 0x002E8B3C, y - hill, h - TASKBAR_H - hill + 1);
        }
    }

    draw_desktop_icon(w, h, 22, 64, "Explorer", 0, (int)((tick / 24) & 1));
    draw_desktop_icon(w, h, 22, 154, "Terminal", 1, 0);
    draw_desktop_icon(w, h, 22, 244, "Music", 2, 0);
}

static void draw_start_button(int w, int h, int active) {
    int y = h - TASKBAR_H + 6;
    uint32_t top = active ? 0x0058C758 : 0x0042B943;
    uint32_t bottom = active ? 0x001B7B26 : 0x00229232;
    (void)w;
    gradient_rect(w, h, 6, y, 94, 30, top, bottom);
    rect_outline(w, h, 6, y, 94, 30, 0x000D5A19);
    draw_text(20, y + 7, "Start", 0x00FFFFFF, bottom, w, h);
}

static void draw_taskbar(int w, int h) {
    int y = h - TASKBAR_H;
    int tx = 112;
    icda_audio_info_t audio;

    gradient_rect(w, h, 0, y, w, TASKBAR_H, 0x002B73D7, 0x0015449C);
    fill_rect(w, h, 0, y, w, 1, 0x0089C4FF);
    draw_start_button(w, h, start_menu_open);

    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        wm_window_t *win = &windows[idx];
        uint32_t top;
        uint32_t bottom;
        if (!win->valid) continue;
        top = focused_window_idx == idx && !win->minimized ? 0x006EA6FF : 0x003A78CF;
        bottom = focused_window_idx == idx && !win->minimized ? 0x002A60BD : 0x00214F9D;
        gradient_rect(w, h, tx, y + 7, 136, 28, top, bottom);
        rect_outline(w, h, tx, y + 7, 136, 28, 0x0089C4FF);
        draw_text_clip(tx + 10, y + 13, win->title, 0x00FFFFFF, bottom, 112, w, h);
        if (win->minimized) {
            fill_rect(w, h, tx + 122, y + 28, 8, 2, 0x00DDEBFF);
        }
        tx += 142;
    }

    if ((long)icda_audio_info(&audio) >= 0 && audio.active) {
        draw_text_clip(w - 176, y + 14, "Audio:", 0x00DDEBFF, 0x0015449C, 56, w, h);
        draw_text_clip(w - 120, y + 14, audio.name, 0x00FFFFFF, 0x0015449C, 104, w, h);
    } else {
        draw_text_clip(w - 106, y + 14, "ICDA OS", 0x00DDEBFF, 0x0015449C, 88, w, h);
    }
}

static void draw_start_menu(int w, int h) {
    int x = 6;
    int y = h - TASKBAR_H - 214;
    if (!start_menu_open) return;
    fill_rect(w, h, x + 5, y + 6, 270, 214, 0x004B5563);
    fill_rect(w, h, x, y, 270, 214, 0x00F8FBFF);
    rect_outline(w, h, x, y, 270, 214, 0x0015449C);
    gradient_rect(w, h, x + 1, y + 1, 268, 42, 0x003D8BFF, 0x001D4FA8);
    draw_text(x + 16, y + 13, "ICDA Desktop", 0x00FFFFFF, 0x002762C4, w, h);

    gradient_rect(w, h, x + 12, y + 58, 246, 30, 0x00FFFFFF, 0x00DDEBFF);
    gradient_rect(w, h, x + 12, y + 94, 246, 30, 0x00FFFFFF, 0x00DDEBFF);
    gradient_rect(w, h, x + 12, y + 130, 246, 30, 0x00FFFFFF, 0x00DDEBFF);
    gradient_rect(w, h, x + 12, y + 166, 246, 30, 0x00FFFFFF, 0x00DDEBFF);
    rect_outline(w, h, x + 12, y + 58, 246, 30, 0x0092B7E8);
    rect_outline(w, h, x + 12, y + 94, 246, 30, 0x0092B7E8);
    rect_outline(w, h, x + 12, y + 130, 246, 30, 0x0092B7E8);
    rect_outline(w, h, x + 12, y + 166, 246, 30, 0x0092B7E8);
    draw_text(x + 26, y + 65, "Explorer", 0x001F2937, 0x00DDEBFF, w, h);
    draw_text(x + 26, y + 101, "Terminal", 0x001F2937, 0x00DDEBFF, w, h);
    draw_text(x + 26, y + 137, "Disk Manager", 0x001F2937, 0x00DDEBFF, w, h);
    draw_text(x + 26, y + 173, "Stop Audio", 0x001F2937, 0x00DDEBFF, w, h);
}

static void bring_to_front(int win_idx) {
    int z_idx = -1;
    for (int i = 0; i < num_windows; i++) {
        if (z_order[i] == win_idx) {
            z_idx = i;
            break;
        }
    }
    if (z_idx != -1) {
        for (int i = z_idx; i < num_windows - 1; i++) z_order[i] = z_order[i + 1];
        z_order[num_windows - 1] = win_idx;
    }
    windows[win_idx].minimized = 0;
    focused_window_idx = win_idx;
}

static void focus_top_visible(void) {
    focused_window_idx = -1;
    for (int i = num_windows - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (windows[idx].valid && !windows[idx].minimized) {
            focused_window_idx = idx;
            return;
        }
    }
}

static void remove_window(int win_idx) {
    int z_idx = -1;
    for (int i = 0; i < num_windows; i++) {
        if (z_order[i] == win_idx) {
            z_idx = i;
            break;
        }
    }
    if (z_idx != -1) {
        for (int i = z_idx; i < num_windows - 1; i++) z_order[i] = z_order[i + 1];
        num_windows--;
    }
    if (focused_window_idx == win_idx) focus_top_visible();
}

static int alloc_window_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].valid) return i;
    }
    return -1;
}

static int find_window_by_id(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].valid && windows[i].id == id) return i;
    }
    return -1;
}

static void send_close_to_app(wm_window_t *win) {
    gui_msg_t close_msg;
    clear_msg(&close_msg);
    close_msg.type = GUI_MSG_CLOSE_WINDOW;
    close_msg.window_id = win->id;
    icda_msg_send(win->app_queue_handle, &close_msg);
}

static void clamp_window(wm_window_t *win, int w, int h) {
    if (win->x < 4) win->x = 4;
    if (win->y < TITLE_H + 4) win->y = TITLE_H + 4;
    if (win->x + win->w > w - 4) win->x = w - win->w - 4;
    if (win->y + win->h > h - TASKBAR_H - 4) win->y = h - TASKBAR_H - win->h - 4;
    if (win->x < 4) win->x = 4;
    if (win->y < TITLE_H + 4) win->y = TITLE_H + 4;
}

static void open_window_from_msg(gui_msg_t *msg, uint64_t wm_queue, int w, int h, uint32_t *next_win_id) {
    int slot = alloc_window_slot();
    gui_msg_t reply;
    if (slot == -1) {
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }

    int win_w = msg->open_req.w;
    int win_h = msg->open_req.h;
    if (win_w < 160) win_w = 160;
    if (win_h < 120) win_h = 120;
    if (win_w > w - 40) win_w = w - 40;
    if (win_h > h - TASKBAR_H - 58) win_h = h - TASKBAR_H - 58;

    uint64_t shm_hnd = icda_shm_create((uint64_t)win_w * win_h * 4);
    if (!shm_hnd) {
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }
    uint64_t map_addr = icda_shm_map(shm_hnd);
    if (!map_addr) {
        icda_shm_close(shm_hnd);
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }

    wm_window_t *win = &windows[slot];
    int range_x = w - win_w - 120;
    int range_y = h - win_h - TASKBAR_H - 110;
    if (range_x < 1) range_x = 1;
    if (range_y < 1) range_y = 1;

    win->valid = 1;
    win->id = (*next_win_id)++;
    win->app_queue_handle = msg->window_id;
    win->shm_handle = shm_hnd;
    win->pixels = (uint32_t*)map_addr;
    win->w = win_w;
    win->h = win_h;
    win->x = 92 + (slot * 38) % range_x;
    win->y = 78 + (slot * 32) % range_y;
    win->minimized = 0;
    win->anim = 0;
    for (int i = 0; i < win_w * win_h; i++) win->pixels[i] = 0x00FFFFFF;
    int ti = 0;
    while (msg->open_req.title[ti] && ti < 31) {
        win->title[ti] = msg->open_req.title[ti];
        ti++;
    }
    win->title[ti] = 0;
    clamp_window(win, w, h);

    z_order[num_windows++] = slot;
    focused_window_idx = slot;

    clear_msg(&reply);
    reply.type = GUI_MSG_OPEN_OK;
    reply.window_id = win->id;
    reply.open_ok.shm_handle = shm_hnd;
    reply.open_ok.w = win_w;
    reply.open_ok.h = win_h;
    reply.open_ok.reply_queue = wm_queue;
    icda_msg_send(win->app_queue_handle, &reply);
}

static int handle_taskbar_click(int mx, int my, int w, int h) {
    int task_y = h - TASKBAR_H;
    int tx = 112;
    (void)my;
    if (hit_rect(mx, my, 6, task_y + 6, 94, 30)) {
        start_menu_open = !start_menu_open;
        return 1;
    }
    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        if (windows[idx].valid && hit_rect(mx, my, tx, task_y + 7, 136, 28)) {
            if (focused_window_idx == idx && !windows[idx].minimized) {
                windows[idx].minimized = 1;
                focus_top_visible();
            } else {
                bring_to_front(idx);
            }
            start_menu_open = 0;
            return 1;
        }
        tx += 142;
    }
    return 0;
}

static int handle_start_menu_click(int mx, int my, int w, int h) {
    int x = 6;
    int y = h - TASKBAR_H - 214;
    (void)w;
    if (!start_menu_open) return 0;
    if (!hit_rect(mx, my, x, y, 270, 214)) {
        start_menu_open = 0;
        return 0;
    }
    if (hit_rect(mx, my, x + 12, y + 58, 246, 30)) icda_spawn("/apps/desktop.app");
    else if (hit_rect(mx, my, x + 12, y + 94, 246, 30)) icda_spawn("/apps/terminal.app");
    else if (hit_rect(mx, my, x + 12, y + 130, 246, 30)) icda_spawn("/apps/diskman.app");
    else if (hit_rect(mx, my, x + 12, y + 166, 246, 30)) icda_stop_audio();
    start_menu_open = 0;
    return 1;
}

static int handle_desktop_icon_click(int mx, int my) {
    if (hit_rect(mx, my, 22, 64, 74, 74)) {
        icda_spawn("/apps/desktop.app");
        return 1;
    }
    if (hit_rect(mx, my, 22, 154, 74, 74)) {
        icda_spawn("/apps/terminal.app");
        return 1;
    }
    if (hit_rect(mx, my, 22, 244, 74, 74)) {
        icda_spawn("/apps/audioplay.app");
        return 1;
    }
    return 0;
}

static void composite_window(wm_window_t *win, int idx, int w, int h) {
    if (!win->valid || win->minimized) return;

    int anim_offset = (8 - win->anim) * 2;
    int wx = win->x;
    int wy = win->y - anim_offset;
    int active = focused_window_idx == idx;
    uint32_t border = active ? 0x000D47A1 : 0x00475569;
    uint32_t title_top = active ? 0x00499BFF : 0x008EA3B7;
    uint32_t title_bottom = active ? 0x001D4FA8 : 0x00475569;

    fill_rect(w, h, wx + 7, wy - TITLE_H + 8, win->w + 2, win->h + TITLE_H + 1, 0x004B5563);
    rect_outline(w, h, wx - 1, wy - TITLE_H - 1, win->w + 2, win->h + TITLE_H + 2, border);
    gradient_rect(w, h, wx, wy - TITLE_H, win->w, TITLE_H, title_top, title_bottom);
    draw_text_clip(wx + 9, wy - TITLE_H + 6, win->title, 0x00FFFFFF, title_bottom, win->w - 74, w, h);

    gradient_rect(w, h, wx + win->w - 48, wy - TITLE_H + 5, 18, 16, 0x00E5EEFF, 0x00AFCBFF);
    rect_outline(w, h, wx + win->w - 48, wy - TITLE_H + 5, 18, 16, 0x000D47A1);
    fill_rect(w, h, wx + win->w - 43, wy - TITLE_H + 16, 8, 2, 0x000D47A1);

    gradient_rect(w, h, wx + win->w - 24, wy - TITLE_H + 5, 18, 16, 0x00FF7B7B, 0x00D31616);
    rect_outline(w, h, wx + win->w - 24, wy - TITLE_H + 5, 18, 16, 0x008B0000);
    draw_text(wx + win->w - 19, wy - TITLE_H + 5, "x", 0x00FFFFFF, 0x00D31616, w, h);

    for (int cy = 0; cy < win->h; cy++) {
        int py = wy + cy;
        if (py < 0 || py >= h) continue;
        for (int cx = 0; cx < win->w; cx++) {
            int px = wx + cx;
            if (px < 0 || px >= w) continue;
            back_buffer[py * w + px] = win->pixels[cy * win->w + cx];
        }
    }

    if (win->anim < 8) win->anim++;
}

static void composite_screen(int w, int h, int mouse_x, int mouse_y) {
    uint64_t tick = icda_ticks();
    draw_background(w, h, tick);
    for (int i = 0; i < num_windows; i++) {
        int idx = z_order[i];
        composite_window(&windows[idx], idx, w, h);
    }
    draw_taskbar(w, h);
    draw_start_menu(w, h);
    draw_cursor(w, h, mouse_x, mouse_y);

    uint32_t pitch_pixels = fb_info.pitch ? fb_info.pitch / 4 : (uint32_t)fb_info.width;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            real_fb[y * pitch_pixels + x] = back_buffer[y * w + x];
        }
    }
}

int wm_main(void) {
    uint64_t addr = icda_map_framebuffer(&fb_info);
    if (!addr) return -1;
    real_fb = (uint32_t*)addr;

    int w = fb_info.width;
    int h = fb_info.height;
    if (w > BACK_BUFFER_WIDTH) w = BACK_BUFFER_WIDTH;
    if (h > BACK_BUFFER_HEIGHT) h = BACK_BUFFER_HEIGHT;
    if (w < 320 || h < 240) return -1;

    uint64_t wm_queue = icda_msg_open(WM_QUEUE_NAME);
    if (!wm_queue) return -1;

    icda_spawn("/apps/desktop.app");

    int mouse_x = w / 2;
    int mouse_y = h / 2;
    uint8_t mouse_buttons = 0;
    int dragging_win_idx = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    uint32_t next_win_id = 1;

    for (;;) {
        int need_redraw = 0;
        while (icda_msg_poll(wm_queue) > 0) {
            gui_msg_t msg;
            if (icda_msg_recv(wm_queue, &msg, 0) != 0) continue;
            need_redraw = 1;
            if (msg.type == GUI_MSG_OPEN_WINDOW) {
                open_window_from_msg(&msg, wm_queue, w, h, &next_win_id);
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                int slot = find_window_by_id(msg.window_id);
                if (slot != -1) {
                    wm_window_t *win = &windows[slot];
                    icda_shm_unmap(win->shm_handle);
                    icda_shm_close(win->shm_handle);
                    win->valid = 0;
                    remove_window(slot);
                }
            }
        }

        icda_mouse_event_t mev;
        if (icda_input_read_mouse(&mev) == 0) {
            int prev_mx = mouse_x;
            int prev_my = mouse_y;
            uint8_t prev_btn = mouse_buttons;
            need_redraw = 1;
            mouse_x = mev.abs_x;
            mouse_y = mev.abs_y;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= w) mouse_x = w - 1;
            if (mouse_y >= h) mouse_y = h - 1;
            mouse_buttons = mev.buttons;

            int left_clicked = (mouse_buttons & 1) && !(prev_btn & 1);
            int left_released = !(mouse_buttons & 1) && (prev_btn & 1);

            if (left_clicked) {
                int handled = 0;
                if (start_menu_open) handled = handle_start_menu_click(mouse_x, mouse_y, w, h);
                if (!handled && mouse_y >= h - TASKBAR_H) handled = handle_taskbar_click(mouse_x, mouse_y, w, h);

                if (!handled) {
                    int hit = -1;
                    start_menu_open = 0;
                    for (int i = num_windows - 1; i >= 0; i--) {
                        int idx = z_order[i];
                        wm_window_t *win = &windows[idx];
                        if (!win->valid || win->minimized) continue;

                        if (hit_rect(mouse_x, mouse_y, win->x + win->w - 24, win->y - TITLE_H + 5, 18, 16)) {
                            send_close_to_app(win);
                            hit = idx;
                            break;
                        }
                        if (hit_rect(mouse_x, mouse_y, win->x + win->w - 48, win->y - TITLE_H + 5, 18, 16)) {
                            win->minimized = 1;
                            focus_top_visible();
                            hit = idx;
                            break;
                        }
                        if (hit_rect(mouse_x, mouse_y, win->x, win->y - TITLE_H, win->w, TITLE_H)) {
                            bring_to_front(idx);
                            dragging_win_idx = idx;
                            drag_off_x = mouse_x - win->x;
                            drag_off_y = mouse_y - win->y;
                            hit = idx;
                            break;
                        }
                        if (hit_rect(mouse_x, mouse_y, win->x, win->y, win->w, win->h)) {
                            bring_to_front(idx);
                            gui_msg_t click_msg;
                            clear_msg(&click_msg);
                            click_msg.type = GUI_MSG_MOUSE_EVENT;
                            click_msg.window_id = win->id;
                            click_msg.mouse.x = mouse_x - win->x;
                            click_msg.mouse.y = mouse_y - win->y;
                            click_msg.mouse.buttons = mouse_buttons;
                            icda_msg_send(win->app_queue_handle, &click_msg);
                            hit = idx;
                            break;
                        }
                    }
                    if (hit == -1) {
                        if (!handle_desktop_icon_click(mouse_x, mouse_y)) focused_window_idx = -1;
                    }
                }
            } else if (left_released) {
                dragging_win_idx = -1;
            } else if (dragging_win_idx != -1) {
                wm_window_t *win = &windows[dragging_win_idx];
                win->x = mouse_x - drag_off_x;
                win->y = mouse_y - drag_off_y;
                clamp_window(win, w, h);
            } else if (focused_window_idx != -1 && (mouse_x != prev_mx || mouse_y != prev_my)) {
                wm_window_t *win = &windows[focused_window_idx];
                if (win->valid && !win->minimized && hit_rect(mouse_x, mouse_y, win->x, win->y, win->w, win->h)) {
                    gui_msg_t motion_msg;
                    clear_msg(&motion_msg);
                    motion_msg.type = GUI_MSG_MOUSE_EVENT;
                    motion_msg.window_id = win->id;
                    motion_msg.mouse.x = mouse_x - win->x;
                    motion_msg.mouse.y = mouse_y - win->y;
                    motion_msg.mouse.buttons = mouse_buttons;
                    icda_msg_send(win->app_queue_handle, &motion_msg);
                }
            }
        }

        long key = icda_read_char_timeout(1);
        if (key >= 0) {
            need_redraw = 1;
            if (focused_window_idx != -1) {
                wm_window_t *win = &windows[focused_window_idx];
                if (win->valid && !win->minimized) {
                    gui_msg_t kmsg;
                    clear_msg(&kmsg);
                    kmsg.type = GUI_MSG_KEY_EVENT;
                    kmsg.window_id = win->id;
                    kmsg.key.keycode = (uint32_t)key;
                    kmsg.key.pressed = 1;
                    icda_msg_send(win->app_queue_handle, &kmsg);
                }
            }
        }

        /* Repaint on any event; otherwise keep ~16fps so desktop
         * animations (icon pulse, selection blink) stay smooth without
         * burning a full 100Hz composite every tick. */
        if (need_redraw || dragging_win_idx != -1 || (icda_ticks() % 6) == 0) {
            composite_screen(w, h, mouse_x, mouse_y);
        }
        icda_sleep(1);
    }
}
