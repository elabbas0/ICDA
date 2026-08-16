#include "gui.h"
#include "icda_sys.h"
#include <stddef.h>

static uint64_t wm_queue = 0;
static uint64_t app_queue = 0;
static uint64_t win_shm_handle = 0;
static uint32_t *win_pixels = NULL;
static uint32_t *win_shm_pixels = NULL;
static int win_w = 0;
static int win_h = 0;
static uint32_t win_id = 0;
static uint64_t win_reply_queue = 0;

/* Staging buffer.  Apps draw into this private copy instead of the shared
 * window buffer so the WM never composites a half-painted frame: the WM
 * re-samples window buffers on its own timer, and an app's multi-step draw
 * (background first, then panels, then items) showed up as a flash of
 * partial content whenever the two clocks crossed.  gui_flush() commits
 * the finished frame to shared memory with one copy, then asks the WM to
 * repaint.  Sized for any window the engine will hand out on a 1280x800
 * screen; larger requests fall back to drawing straight into the SHM. */
#define GUI_STAGING_PIXELS (1280 * 800)
static uint32_t gui_staging[GUI_STAGING_PIXELS];

int gui_open_window(const char *title, int w, int h) {
    wm_queue = icda_msg_open(WM_QUEUE_NAME);
    if (!wm_queue) return -1;

    char qname[64];
    uint64_t pid = icda_get_pid();
    qname[0] = '/'; qname[1] = 'a'; qname[2] = 'p'; qname[3] = 'p'; qname[4] = '/';
    int idx = 5;
    uint64_t temp = pid;
    char num[32];
    int nidx = 0;
    if (temp == 0) {
        num[nidx++] = '0';
    } else {
        while (temp > 0) {
            num[nidx++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    for (int i = nidx - 1; i >= 0; i--) {
        qname[idx++] = num[i];
    }
    qname[idx] = '\0';

    app_queue = icda_msg_open(qname);
    if (!app_queue) return -1;

    gui_msg_t msg;
    for (int i = 0; i < 64; i++) ((uint8_t*)&msg)[i] = 0;
    msg.type = GUI_MSG_OPEN_WINDOW;
    msg.window_id = (uint32_t)app_queue;
    msg.open_req.w = w;
    msg.open_req.h = h;
    int len = 0;
    while (title[len] && len < 31) {
        msg.open_req.title[len] = title[len];
        len++;
    }
    msg.open_req.title[len] = '\0';

    if (icda_msg_send(wm_queue, &msg) != 0) {
        return -1;
    }

    gui_msg_t reply;
    if (icda_msg_recv(app_queue, &reply, 1) != 0) {
        return -1;
    }

    if (reply.type != GUI_MSG_OPEN_OK) {
        return -1;
    }

    win_id = reply.window_id;
    win_shm_handle = reply.open_ok.shm_handle;
    win_w = reply.open_ok.w;
    win_h = reply.open_ok.h;
    win_reply_queue = reply.open_ok.reply_queue;

    uint64_t addr = icda_shm_map(win_shm_handle);
    if (!addr) {
        return -1;
    }
    win_shm_pixels = (uint32_t*)addr;
    /* Draw into the staging copy when it fits (the normal case); the
     * WM sees only committed frames via gui_flush(). */
    if ((uint64_t)win_w * (uint64_t)win_h <= GUI_STAGING_PIXELS) {
        win_pixels = gui_staging;
    } else {
        win_pixels = win_shm_pixels;
    }

    return 0;
}

uint32_t *gui_pixel_buffer(void) { return win_pixels; }
int gui_window_width(void) { return win_w; }
int gui_window_height(void) { return win_h; }

void gui_flush(void) {
    gui_msg_t msg;
    if (!win_reply_queue) return;
    if (win_pixels != win_shm_pixels && win_shm_pixels && win_pixels) {
        /* Commit the finished frame: one tight copy beats the WM
         * catching us between draw steps. */
        for (int i = 0; i < win_w * win_h; i++) {
            win_shm_pixels[i] = win_pixels[i];
        }
    }
    for (int i = 0; i < 64; i++) ((uint8_t*)&msg)[i] = 0;
    msg.type = GUI_MSG_FLUSH;
    msg.window_id = win_id;
    icda_msg_send(win_reply_queue, &msg);
}

int gui_poll_event(gui_msg_t *out) {
    if (!app_queue) return 0;
    if (icda_msg_poll(app_queue) <= 0) return 0;
    if (icda_msg_recv(app_queue, out, 0) == 0) {
        return 1;
    }
    return 0;
}

void gui_wait_event(gui_msg_t *out) {
    if (!app_queue) return;
    icda_msg_recv(app_queue, out, 1);
}

void gui_close_window(void) {
    if (win_reply_queue) {
        gui_msg_t msg;
        for (int i = 0; i < 64; i++) ((uint8_t*)&msg)[i] = 0;
        msg.type = GUI_MSG_CLOSE_WINDOW;
        msg.window_id = win_id;
        icda_msg_send(win_reply_queue, &msg);
    }
    if (win_shm_handle) {
        /* Unmap our side only.  The WM also has this region mapped (it
         * composites from it), so closing the handle here would drop the
         * refcount to zero and free the physical pages while the WM is
         * still looking at them - a page fault.  The WM releases its own
         * mapping and closes the region when it processes CLOSE_WINDOW. */
        icda_shm_unmap(win_shm_handle);
        win_shm_handle = 0;
    }
    win_pixels = NULL;
    win_shm_pixels = NULL;
    win_w = 0;
    win_h = 0;
    win_id = 0;
    win_reply_queue = 0;
}

void gui_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!win_pixels) return;
    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > win_w) x2 = win_w;
    if (y2 > win_h) y2 = win_h;
    for (int cy = y1; cy < y2; cy++) {
        for (int cx = x1; cx < x2; cx++) {
            win_pixels[cy * win_w + cx] = color;
        }
    }
}

#include "font.h"

void gui_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!win_pixels) return;
    font_draw_char(win_pixels, win_w, win_h, win_w, x, y, c, fg, bg);
}

void gui_draw_text(int x, int y, const char *str, uint32_t fg, uint32_t bg) {
    if (!win_pixels || !str) return;
    int cx = x;
    while (*str) {
        gui_draw_char(cx, y, *str, fg, bg);
        cx += FONT_CELL_WIDTH;
        str++;
    }
}

void gui_draw_hline(int x, int y, int len, uint32_t color) {
    if (!win_pixels || y < 0 || y >= win_h) return;
    int x1 = x;
    int x2 = x + len;
    if (x1 < 0) x1 = 0;
    if (x2 > win_w) x2 = win_w;
    for (int cx = x1; cx < x2; cx++) {
        win_pixels[y * win_w + cx] = color;
    }
}

void gui_draw_vline(int x, int y, int len, uint32_t color) {
    if (!win_pixels || x < 0 || x >= win_w) return;
    int y1 = y;
    int y2 = y + len;
    if (y1 < 0) y1 = 0;
    if (y2 > win_h) y2 = win_h;
    for (int cy = y1; cy < y2; cy++) {
        win_pixels[cy * win_w + x] = color;
    }
}

void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    gui_draw_hline(x, y, w, color);
    gui_draw_hline(x, y + h - 1, w, color);
    gui_draw_vline(x, y, h, color);
    gui_draw_vline(x + w - 1, y, h, color);
}
