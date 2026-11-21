#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "gui_proto.h"












int  gui_open_window(const char *title, int w, int h);


uint32_t *gui_pixel_buffer(void);
int       gui_window_width(void);
int       gui_window_height(void);
int       gui_window_scale(void);
uint64_t  gui_font_shm(void);


void gui_flush(void);
/* gui_flush for part of the window (window coordinates): only that
 * rectangle is compared and published.  For what a program knows it changed
 * (a frame of a video or a web page under unchanged surroundings). */
void gui_flush_rect(int x, int y, int w, int h);
void gui_set_cursor(int shape);
/* draw straight into the window manager's buffer (for a whole-window
 * redraw each frame: no copy, and a flush says "everything") */
void gui_use_shared_buffer(int on);
/* key presses and releases as GUI_KEY_RAW events, and pointer lock (see
 * GUI_MSG_INPUT_MODE) */
void gui_set_input_mode(int raw_keys, int pointer_lock);
/* the window's id (0: none), for /dev/dialog's window= */
uint32_t gui_window_id(void);
/* tells the window manager a dialog request was just made (it looks at once) */
void gui_dialog_kick(void);


int gui_poll_event(gui_msg_t *out);


void gui_wait_event(gui_msg_t *out);


void gui_close_window(void);


void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_hline(int x, int y, int len, uint32_t color);
void gui_draw_vline(int x, int y, int len, uint32_t color);
void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color);

#endif 
