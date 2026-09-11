#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

typedef enum {
    CONSOLE_STYLE_INFO = 0,
    CONSOLE_STYLE_OK,
    CONSOLE_STYLE_MUTED,
    CONSOLE_STYLE_ACCENT,
    CONSOLE_STYLE_WARN,
    CONSOLE_STYLE_ERROR
} console_style_t;

void console_init(int has_framebuffer);
void console_set_serial_mirror(int enabled);
void console_clear(void);
/* Slice B: mute framebuffer text while the GUI owns the screen (or
 * before the WM's first present) so pre/post-WM console writes go
 * serial-only and never flash over the splash/wallpaper. Text VTs
 * leave this unmuted. */
void console_mute_fb(int muted);
int console_fb_muted(void);
void console_set_cursor(int x, int y);
void console_get_cursor(int *x_out, int *y_out);
void console_write_char(char c, console_style_t style);
void console_write(const char *str, console_style_t style);
void console_backspace(void);
void console_write_status(const char *label, const char *status, console_style_t style);
void console_write_hex64(uint64_t value, console_style_t style);
void console_write_dec64(uint64_t value, console_style_t style);
void console_set_overlay_top_right(const char *text, console_style_t style);
void console_clear_overlay_top_right(void);
void console_refresh_overlay(void);

#endif
