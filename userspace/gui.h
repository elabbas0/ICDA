#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "gui_proto.h"












int  gui_open_window(const char *title, int w, int h);


uint32_t *gui_pixel_buffer(void);
int       gui_window_width(void);
int       gui_window_height(void);


void gui_flush(void);
void gui_set_cursor(int shape);


int gui_poll_event(gui_msg_t *out);


void gui_wait_event(gui_msg_t *out);


void gui_close_window(void);


void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_hline(int x, int y, int len, uint32_t color);
void gui_draw_vline(int x, int y, int len, uint32_t color);
void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color);

#endif 
