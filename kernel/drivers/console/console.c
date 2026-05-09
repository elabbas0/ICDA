#include "console.h"

#include "../display/framebuffer.h"
#include "../display/vga.h"
#include "../serial/serial.h"

static int console_has_framebuffer = 0;

static uint32_t fb_color_for(console_style_t style) {
    switch (style) {
        case CONSOLE_STYLE_OK:
            return FB_GREEN;
        case CONSOLE_STYLE_WARN:
            return FB_YELLOW;
        case CONSOLE_STYLE_ERROR:
            return FB_RED;
        case CONSOLE_STYLE_INFO:
        default:
            return FB_WHITE;
    }
}

static unsigned char vga_color_for(console_style_t style) {
    switch (style) {
        case CONSOLE_STYLE_OK:
            return VGA_GREEN_ON_BLACK;
        case CONSOLE_STYLE_WARN:
            return VGA_YELLOW_ON_BLACK;
        case CONSOLE_STYLE_ERROR:
            return VGA_RED_ON_BLACK;
        case CONSOLE_STYLE_INFO:
        default:
            return VGA_WHITE_ON_BLACK;
    }
}

void console_init(int has_framebuffer) {
    console_has_framebuffer = has_framebuffer;
}

void console_clear(void) {
    if (console_has_framebuffer && fb_available()) {
        fb_clear(FB_BLACK);
        return;
    }

    vga_set_color(VGA_WHITE_ON_BLACK);
    vga_clear();
}

void console_write(const char *str, console_style_t style) {
    if (!str) {
        return;
    }

    if (console_has_framebuffer && fb_available()) {
        fb_print(str, fb_color_for(style), FB_BLACK);
        return;
    }

    if (serial_ready()) {
        serial_write(str);
    }
    vga_print(str, vga_color_for(style));
}

void console_write_status(const char *label, const char *status, console_style_t style) {
    console_write(label, CONSOLE_STYLE_INFO);
    console_write(": ", CONSOLE_STYLE_INFO);
    console_write(status, style);
    console_write("\n", CONSOLE_STYLE_INFO);
}

void console_write_hex64(uint64_t value, console_style_t style) {
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    buf[18] = '\0';

    for (int i = 17; i >= 2; i--) {
        int nibble = value & 0xF;
        buf[i] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
        value >>= 4;
    }

    console_write(buf, style);
}

void console_write_dec64(uint64_t value, console_style_t style) {
    if (value == 0) {
        console_write("0", style);
        return;
    }

    char buf[21];
    int i = 20;
    buf[20] = '\0';

    while (value) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }

    console_write(buf + i, style);
}
