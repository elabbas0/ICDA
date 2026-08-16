/*
 * wm.c - the ICDA window manager / desktop engine host.
 *
 * The WM owns the physical framebuffer and composites every window.
 * Apps never touch the screen: they draw into shared-memory window
 * buffers (gui_pixel_buffer) and the WM blits them into the scene.
 * All chrome/theme/icon rendering comes from libicda so the desktop
 * and apps share one look.
 */
#include "libicda.h"
#include "gui_proto.h"

#define MAX_WINDOWS 16
#define BACK_BUFFER_WIDTH 2560
#define BACK_BUFFER_HEIGHT 1600
#define TASKBAR_H 42
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
    int      focus_glow;   /* one-shot brighten on focus gain, decays to 0 */
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
static const ic_theme_t *theme;

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

static ic_canvas_t bb_canvas(int w, int h) {
    ic_canvas_t c;
    c.px = back_buffer;
    c.w = w;
    c.h = h;
    return c;
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
}/* blend: 0..255 selection highlight intensity for the icon tile.  A
 * smooth triangle wave from the WM loop drives this so the tile gently
 * breathes instead of hard-toggling (a fast (tick/N)&1 toggle flashed). */
static void draw_desktop_icon(int w, int h, int x, int y, const char *label,
                              const char *icon_name, int blend) {
    ic_canvas_t c = bb_canvas(w, h);
    const ic_icon_t *icon = ic_icon_builtin(icon_name);
    ic_rect_r(&c, x, y, 74, 74, 8, ic_blend(0x002D7FB8, 0x001F6EA6, blend, 255));
    ic_outline_r(&c, x, y, 74, 74, 8, 0x0087C6FF);
    if (icon) {
        ic_icon_draw(&c, x + 13, y + 8, 48, 48, icon);
    }
    ic_text_clip(&c, x + 4, y + 56, label, 0x00FFFFFF, 0x001F6EA6, 66);


}

static void draw_background(int w, int h) {
    ic_canvas_t c = bb_canvas(w, h);
    int horizon = (h * 60) / 100;

    /* sky */
    for (int y = 0; y < horizon; y++) {
        ic_rect(&c, 0, y, w, 1, ic_blend(0x0056B6F7, 0x00CBE9FF, y, horizon));
    }
    /* ground */
    for (int y = horizon; y < h; y++) {
        ic_rect(&c, 0, y, w, 1, ic_blend(0x003FAF4B, 0x001D7D37, y - horizon, h - horizon));
    }
    /* hills */
    for (int x = 0; x < w; x++) {
        int64_t dx = x - (w / 3);
        int hill = horizon + 28 - (int)((dx * dx) / (w > 0 ? w * 3 : 1));
        if (hill < horizon - 38) hill = horizon - 38;
        if (hill > h - TASKBAR_H) hill = h - TASKBAR_H;
        for (int y = hill; y < h - TASKBAR_H; y++) {
            ic_rect(&c, x, y, 1, 1, ic_blend(0x0078C850, 0x002E8B3C, y - hill, h - TASKBAR_H - hill + 1));
        }
    }

    /* Desktop tiles sit static (blend 0); only the taskbar/selection
     * highlights animate, and only briefly. */
    draw_desktop_icon(w, h, 22, 64, "Explorer", "folder", 0);
    draw_desktop_icon(w, h, 22, 154, "Terminal", "terminal", 0);
    draw_desktop_icon(w, h, 22, 244, "Music", "music", 0);
}

static void draw_start_button(int w, int h, int active) {
    ic_canvas_t c = bb_canvas(w, h);
    int y = h - TASKBAR_H + 6;
    uint32_t top = active ? 0x0058C758 : 0x0042B943;
    uint32_t bottom = active ? 0x001B7B26 : 0x00229232;
    ic_gradient_v(&c, 6, y, 94, 30, top, bottom);
    ic_outline(&c, 6, y, 94, 30, 0x000D5A19);
    ic_text(&c, 20, y + 7, "Start", 0x00FFFFFF, bottom);
}

static const char *window_icon_name(const char *title) {
    char t[40];
    ic_strcpy(t, title, sizeof(t));
    for (int i = 0; t[i]; i++) t[i] = ic_lower(t[i]);
    if (ic_strprefix(t, "terminal")) return "terminal";
    if (ic_strprefix(t, "icda explorer")) return "folder";
    if (ic_strprefix(t, "disk")) return "disk";
    if (ic_strprefix(t, "editor") || ic_strprefix(t, "notepad")) return "editor";
    if (ic_strprefix(t, "icda demo")) return "app";
    return "app";
}

static void draw_taskbar(int w, int h) {
    ic_canvas_t c = bb_canvas(w, h);
    int y = h - TASKBAR_H;
    int tx = 112;
    icda_audio_info_t audio;

    ic_gradient_v(&c, 0, y, w, TASKBAR_H, theme->taskbar_top, theme->taskbar_bottom);
    ic_hline(&c, 0, y, w, 0x0089C4FF);
    draw_start_button(w, h, start_menu_open);

    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        wm_window_t *win = &windows[idx];
        uint32_t top;
        uint32_t bottom;
        const ic_icon_t *icon;

        if (!win->valid) continue;
        top = focused_window_idx == idx && !win->minimized ? 0x006EA6FF : 0x003A78CF;
        bottom = focused_window_idx == idx && !win->minimized ? 0x002A60BD : 0x00214F9D;
        ic_gradient_v(&c, tx, y + 7, 136, 28, top, bottom);
        ic_outline(&c, tx, y + 7, 136, 28, 0x0089C4FF);

        icon = ic_icon_builtin(window_icon_name(win->title));
        if (icon) ic_icon_draw(&c, tx + 6, y + 11, 20, 20, icon);
        ic_text_clip(&c, tx + 30, y + 13, win->title, 0x00FFFFFF, bottom, 100);
        if (win->minimized) {
            ic_rect(&c, tx + 122, y + 28, 8, 2, 0x00DDEBFF);
        }
        tx += 142;
    }

    if ((long)icda_audio_info(&audio) >= 0 && audio.active) {
        ic_text_clip(&c, w - 176, y + 14, "Audio:", 0x00DDEBFF, 0x0015449C, 56);
        ic_text_clip(&c, w - 120, y + 14, audio.name, 0x00FFFFFF, 0x0015449C, 104);
    } else {
        ic_text_clip(&c, w - 106, y + 14, "ICDA OS", 0x00DDEBFF, 0x0015449C, 88);
    }
}

static void draw_start_row(int sw, int sh, int x, int y, int w, int h,
                           const char *icon_name, const char *label) {
    ic_canvas_t c = bb_canvas(sw, sh);
    const ic_icon_t *icon = ic_icon_builtin(icon_name);
    ic_gradient_v(&c, x, y, w, h, 0x00FFFFFF, 0x00DDEBFF);
    ic_outline(&c, x, y, w, h, 0x0092B7E8);
    if (icon) ic_icon_draw(&c, x + 6, y + 4, 22, 22, icon);
    ic_text(&c, x + 34, y + 7, label, 0x001F2937, 0x00DDEBFF);
}

static void draw_start_menu(int w, int h) {
    ic_canvas_t c = bb_canvas(w, h);
    int x = 6;
    int y = h - TASKBAR_H - 250;
    if (!start_menu_open) return;
    ic_rect(&c, x + 5, y + 6, 270, 250, 0x004B5563);
    ic_rect(&c, x, y, 270, 250, 0x00F8FBFF);
    ic_outline(&c, x, y, 270, 250, 0x0015449C);
    ic_gradient_v(&c, x + 1, y + 1, 268, 42, 0x003D8BFF, 0x001D4FA8);
    ic_text(&c, x + 16, y + 13, "ICDA Desktop", 0x00FFFFFF, 0x002762C4);

    draw_start_row(w, h, x + 12, y + 56, 246, 30, "folder", "Explorer");
    draw_start_row(w, h, x + 12, y + 88, 246, 30, "terminal", "Terminal");
    draw_start_row(w, h, x + 12, y + 120, 246, 30, "disk", "Disk Manager");
    draw_start_row(w, h, x + 12, y + 152, 246, 30, "app", "GUI Demo");
    draw_start_row(w, h, x + 12, y + 184, 246, 30, "audio", "Audio Player");
    draw_start_row(w, h, x + 12, y + 216, 246, 30, "gear", "Stop Audio");
}

/* ---- focus notifications ------------------------------------------------- */

static void notify_focus_change(int old_idx, int new_idx) {
    if (old_idx == new_idx) return;
    if (old_idx >= 0 && old_idx < MAX_WINDOWS && windows[old_idx].valid) {
        gui_msg_t m;
        clear_msg(&m);
        m.type = GUI_MSG_FOCUS;
        m.window_id = windows[old_idx].id;
        m.focus.focused = 0;
        icda_msg_send(windows[old_idx].app_queue_handle, &m);
    }
    if (new_idx >= 0 && new_idx < MAX_WINDOWS && windows[new_idx].valid) {
        gui_msg_t m;
        clear_msg(&m);
        m.type = GUI_MSG_FOCUS;
        m.window_id = windows[new_idx].id;
        m.focus.focused = 1;
        icda_msg_send(windows[new_idx].app_queue_handle, &m);
    }
}

static void set_focus(int idx) {
    int old = focused_window_idx;
    focused_window_idx = idx;
    if (idx >= 0 && idx != old) {
        windows[idx].focus_glow = 96;   /* brief pulse on focus gain */
    }
    notify_focus_change(old, focused_window_idx);
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
    set_focus(win_idx);
}

static void focus_top_visible(void) {
    int old = focused_window_idx;
    int found = -1;
    for (int i = num_windows - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (windows[idx].valid && !windows[idx].minimized) {
            found = idx;
            break;
        }
    }
    if (found >= 0) {
        set_focus(found);
    } else if (old != -1) {
        focused_window_idx = -1;
        notify_focus_change(old, -1);
    } else {
        focused_window_idx = -1;
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
    if (win->y < IC_TITLE_H + 4) win->y = IC_TITLE_H + 4;
    if (win->x + win->w > w - 4) win->x = w - win->w - 4;
    if (win->y + win->h > h - TASKBAR_H - 4) win->y = h - TASKBAR_H - win->h - 4;
    if (win->x < 4) win->x = 4;
    if (win->y < IC_TITLE_H + 4) win->y = IC_TITLE_H + 4;
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

    {
        int win_w = msg->open_req.w;
        int win_h = msg->open_req.h;
        if (win_w < 160) win_w = 160;
        if (win_h < 120) win_h = 120;
        if (win_w > w - 40) win_w = w - 40;
        if (win_h > h - TASKBAR_H - 58) win_h = h - TASKBAR_H - 58;

        {
            uint64_t shm_hnd = icda_shm_create((uint64_t)win_w * win_h * 4);
            if (!shm_hnd) {
                clear_msg(&reply);
                reply.type = GUI_MSG_OPEN_FAIL;
                icda_msg_send(msg->window_id, &reply);
                return;
            }
            {
                uint64_t map_addr = icda_shm_map(shm_hnd);
                if (!map_addr) {
                    icda_shm_close(shm_hnd);
                    clear_msg(&reply);
                    reply.type = GUI_MSG_OPEN_FAIL;
                    icda_msg_send(msg->window_id, &reply);
                    return;
                }

                {
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
                    {
                        int ti = 0;
                        while (msg->open_req.title[ti] && ti < 31) {
                            win->title[ti] = msg->open_req.title[ti];
                            ti++;
                        }
                        win->title[ti] = 0;
                    }
                    clamp_window(win, w, h);

                    z_order[num_windows++] = slot;

                    clear_msg(&reply);
                    reply.type = GUI_MSG_OPEN_OK;
                    reply.window_id = win->id;
                    reply.open_ok.shm_handle = shm_hnd;
                    reply.open_ok.w = win_w;
                    reply.open_ok.h = win_h;
                    reply.open_ok.reply_queue = wm_queue;
                    icda_msg_send(win->app_queue_handle, &reply);

                    /* Notify focus only after the open handshake completes:
                     * the app is blocked in recv() waiting for OPEN_OK, so a
                     * FOCUS message sent first would be mistaken for the
                     * reply and abort the window open. */
                    {
                        set_focus(slot);
                    }
                }
            }
        }
    }
}

static int handle_taskbar_click(int mx, int my, int w, int h) {
    int task_y = h - TASKBAR_H;
    int tx = 112;
    (void)w;
    if (ic_hit_rect(mx, my, (ic_rect_t){6, task_y + 6, 94, 30})) {
        start_menu_open = !start_menu_open;
        return 1;
    }
    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        if (windows[idx].valid && ic_hit_rect(mx, my, (ic_rect_t){tx, task_y + 7, 136, 28})) {
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
    int y = h - TASKBAR_H - 250;
    (void)w;
    if (!start_menu_open) return 0;
    if (!ic_hit_rect(mx, my, (ic_rect_t){x, y, 270, 250})) {
        /* Outside the menu: dismiss it. Returning 1 here (instead of 0)
         * stops the taskbar click handler from immediately re-opening it
         * when the user clicks the Start button to close the menu. */
        start_menu_open = 0;
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 56, 246, 30})) icda_spawn("/apps/desktop.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 88, 246, 30})) icda_spawn("/apps/terminal.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 120, 246, 30})) icda_spawn("/apps/diskman.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 152, 246, 30})) icda_spawn("/apps/gui_demo.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 184, 246, 30})) icda_spawn("/apps/audioplay.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 216, 246, 30})) icda_stop_audio();
    start_menu_open = 0;
    return 1;
}

static int handle_desktop_icon_click(int mx, int my) {
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 64, 74, 74})) {
        icda_spawn("/apps/desktop.app");
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 154, 74, 74})) {
        icda_spawn("/apps/terminal.app");
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 244, 74, 74})) {
        icda_spawn("/apps/audioplay.app");
        return 1;
    }
    return 0;
}

static void composite_window(wm_window_t *win, int idx, int w, int h) {
    ic_canvas_t c = bb_canvas(w, h);
    int active;
    int anim;
    int wx;
    int wy;

    if (!win->valid || win->minimized) return;
    active = focused_window_idx == idx;

    anim = win->anim;
    if (anim < 0) anim = 0;
    if (anim > IC_ANIM_MAX) anim = IC_ANIM_MAX;
    wx = win->x;
    wy = win->y - (IC_ANIM_MAX - anim) * 2;

    {
        ic_window_t iw;
        ic_theme_t t = *theme;
        iw.x = win->x;
        iw.y = win->y;
        iw.w = win->w;
        iw.h = win->h;
        iw.focused = active;
        iw.minimized = 0;
        iw.anim = win->anim;
        iw.title = win->title;
        /* One-shot focus glow: the chrome brightens briefly when the
         * window gains focus, then settles fully static.  The old
         * perpetual (tick/N)&1 toggles and even a slow triangle wave kept
         * the whole chrome in motion, which read as flashing; a window
         * should sit still until the user interacts with it. */
        if (active && win->focus_glow > 0) {
            int m = win->focus_glow;
            t.title_top_active = ic_blend(0x006BB1FF, 0x00499BFF, m, 255);
            t.title_bottom_active = ic_blend(0x00296FD0, 0x001D4FA8, m, 255);
            t.border_active = ic_blend(0x00155EC2, 0x000D47A1, m, 255);
        }
        ic_draw_chrome(&c, &t, &iw, ic_icon_builtin("close"), ic_icon_builtin("min"));
    }

    /* client pixels */
    for (int cy = 0; cy < win->h; cy++) {
        int py = wy + cy;
        if (py < 0 || py >= h) continue;
        for (int cx = 0; cx < win->w; cx++) {
            int px = wx + cx;
            if (px < 0 || px >= w) continue;
            back_buffer[py * w + px] = win->pixels[cy * win->w + cx];
        }
    }

    if (win->anim < IC_ANIM_MAX) win->anim++;
    if (win->focus_glow > 0) win->focus_glow -= 4;
}

static void composite_screen(int w, int h, int mouse_x, int mouse_y) {
    draw_background(w, h);
    for (int i = 0; i < num_windows; i++) {
        int idx = z_order[i];
        composite_window(&windows[idx], idx, w, h);
    }
    draw_taskbar(w, h);
    draw_start_menu(w, h);
    draw_cursor(w, h, mouse_x, mouse_y);

    {
        uint32_t pitch_pixels = fb_info.pitch ? fb_info.pitch / 4 : (uint32_t)fb_info.width;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                real_fb[y * pitch_pixels + x] = back_buffer[y * w + x];
            }
        }
    }
}

int main(int argc, char **argv) {
    uint64_t addr;
    int w;
    int h;
    uint64_t wm_queue;
    int mouse_x;
    int mouse_y;
    uint8_t mouse_buttons = 0;
    int dragging_win_idx = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    uint32_t next_win_id = 1;

    (void)argc;
    (void)argv;

    theme = ic_theme_default();

    /* Replace the stock icon set with whatever the user dropped into
     * /usr/share/icons as .ico files (folder.ico, terminal.ico, ...). */
    ic_icon_load_folder("/usr/share/icons");

    addr = icda_map_framebuffer(&fb_info);
    if (!addr) return -1;
    real_fb = (uint32_t*)addr;

    w = fb_info.width;
    h = fb_info.height;
    if (w > BACK_BUFFER_WIDTH) w = BACK_BUFFER_WIDTH;
    if (h > BACK_BUFFER_HEIGHT) h = BACK_BUFFER_HEIGHT;
    if (w < 320 || h < 240) return -1;

    wm_queue = icda_msg_open(WM_QUEUE_NAME);
    if (!wm_queue) return -1;

    icda_spawn("/apps/desktop.app");

    mouse_x = w / 2;
    mouse_y = h / 2;

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

        {
            /* Drain every queued mouse event in one pass and handle each in
             * order.  Reading a single event per loop pass let the kernel
             * ring buffer back up while a frame composited, so the cursor
             * trailed the hand and kept gliding after the mouse stopped.
             * Processing the whole batch keeps press/release edges (clicks)
             * and drags correct while tracking the live position, and only
             * the final composite runs once the batch is consumed. */
            icda_mouse_event_t mev;
            while (icda_input_read_mouse(&mev) == 0) {
                int prev_mx = mouse_x;
                int prev_my = mouse_y;
                uint8_t prev_btn = mouse_buttons;
                need_redraw = 1;
                mouse_x = mev.abs_x;
                mouse_y = mev.abs_y;
                mouse_buttons = mev.buttons;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= w) mouse_x = w - 1;
                if (mouse_y >= h) mouse_y = h - 1;

                {
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
                                ic_window_t iw;

                                if (!win->valid || win->minimized) continue;
                                iw.x = win->x;
                                iw.y = win->y;
                                iw.w = win->w;
                                iw.h = win->h;
                                iw.title = win->title;

                                if (ic_hit_close(&iw, mouse_x, mouse_y)) {
                                    send_close_to_app(win);
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_minimize(&iw, mouse_x, mouse_y)) {
                                    win->minimized = 1;
                                    focus_top_visible();
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_title(&iw, mouse_x, mouse_y)) {
                                    bring_to_front(idx);
                                    dragging_win_idx = idx;
                                    drag_off_x = mouse_x - win->x;
                                    drag_off_y = mouse_y - win->y;
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_client(&iw, mouse_x, mouse_y)) {
                                    bring_to_front(idx);
                                    {
                                        gui_msg_t click_msg;
                                        clear_msg(&click_msg);
                                        click_msg.type = GUI_MSG_MOUSE_EVENT;
                                        click_msg.window_id = win->id;
                                        click_msg.mouse.x = mouse_x - win->x;
                                        click_msg.mouse.y = mouse_y - win->y;
                                        click_msg.mouse.buttons = mouse_buttons;
                                        icda_msg_send(win->app_queue_handle, &click_msg);
                                    }
                                    hit = idx;
                                    break;
                                }
                            }
                            if (hit == -1) {
                                if (!handle_desktop_icon_click(mouse_x, mouse_y)) {
                                    if (focused_window_idx != -1) {
                                        int old = focused_window_idx;
                                        focused_window_idx = -1;
                                        notify_focus_change(old, -1);
                                    }
                                }
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
                        if (win->valid && !win->minimized) {
                            ic_window_t iw = { win->x, win->y, win->w, win->h, 0, 0, 0, win->title };
                            if (ic_hit_client(&iw, mouse_x, mouse_y)) {
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
                }
            }
        }

        {
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
        }

        /* Repaint on any event; otherwise keep ~16fps so desktop
         * animations (icon pulse, focus pulse) stay smooth without
         * burning a full 100Hz composite every tick. */
        if (need_redraw || dragging_win_idx != -1 || (icda_ticks() % 6) == 0) {
            composite_screen(w, h, mouse_x, mouse_y);
        }
        icda_sleep(1);
    }
}
