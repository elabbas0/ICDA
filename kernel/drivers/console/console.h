#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

typedef enum {
    CONSOLE_STYLE_INFO = 0,
    CONSOLE_STYLE_OK,
    CONSOLE_STYLE_WARN,
    CONSOLE_STYLE_ERROR
} console_style_t;

void console_init(int has_framebuffer);
void console_clear(void);
void console_write(const char *str, console_style_t style);
void console_write_status(const char *label, const char *status, console_style_t style);
void console_write_hex64(uint64_t value, console_style_t style);
void console_write_dec64(uint64_t value, console_style_t style);

#endif
