#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "gui_proto.h"

/*
 * App-facing GUI library.
 * Usage:
 *   1. gui_open_window("My App", 800, 600)
 *   2. Draw into gui_pixel_buffer()  (direct 32bpp ARGB)
 *   3. gui_flush() to ask WM to repaint
 *   4. gui_poll_event() or gui_wait_event() for input
 *   5. gui_close_window() when done
 */

/* Open a window. Returns 0 on success, -1 on failure. */
int  gui_open_window(const char *title, int w, int h);

/* Get direct pointer to the 32bpp ARGB pixel buffer. */
uint32_t *gui_pixel_buffer(void);
int       gui_window_width(void);
int       gui_window_height(void);

/* Tell the WM this window's buffer has changed and needs compositing. */
void gui_flush(void);

/* Poll for an event. Returns 1 if event was written to *out, 0 if none. */
int gui_poll_event(gui_msg_t *out);

/* Block until an event arrives, then fill *out. */
void gui_wait_event(gui_msg_t *out);

/* Close the window and release resources. */
void gui_close_window(void);

/* -- Convenience drawing helpers (draw into gui_pixel_buffer()) -- */
void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gui_draw_text(int x, int y, const char *str, uint32_t fg, uint32_t bg);
void gui_draw_hline(int x, int y, int len, uint32_t color);
void gui_draw_vline(int x, int y, int len, uint32_t color);
void gui_draw_rect_outline(int x, int y, int w, int h, uint32_t color);

#endif /* GUI_H */
