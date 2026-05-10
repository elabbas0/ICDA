#include "console.h"

#include "../device.h"
#include "../display/framebuffer.h"
#include "../display/vga.h"

static kernel_device_t *display_device = 0;
static kernel_device_t *serial_device = 0;
static int console_display_is_framebuffer = 0;

static int console_has_framebuffer = 0;

static uint32_t fb_color_for(console_style_t style) {
    switch (style) {
        case CONSOLE_STYLE_OK:
            return FB_GREEN;
        case CONSOLE_STYLE_MUTED:
            return FB_LIGHT_GRAY;
        case CONSOLE_STYLE_ACCENT:
            return FB_CYAN;
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
        case CONSOLE_STYLE_MUTED:
            return VGA_WHITE_ON_BLACK;
        case CONSOLE_STYLE_ACCENT:
            return VGA_CYAN_ON_BLACK;
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
    kernel_device_t *framebuffer = device_find("framebuffer");

    console_has_framebuffer = has_framebuffer;
    if (console_has_framebuffer && framebuffer) {
        display_device = framebuffer;
    }
    if (!display_device) {
        display_device = device_find("vga");
    }
    if (!display_device) {
        display_device = device_first(DEVICE_CLASS_DISPLAY);
    }
    serial_device = device_find("serial");
    console_display_is_framebuffer = (display_device && framebuffer && display_device == framebuffer);
}

void console_clear(void) {
    if (display_device) {
        const display_device_ops_t *ops = (const display_device_ops_t *)display_device->ops;
        ops->clear(display_device->context);
        return;
    }
}

void console_write_char(char c, console_style_t style) {
    char buf[2];

    buf[0] = c;
    buf[1] = '\0';
    console_write(buf, style);
}

void console_write(const char *str, console_style_t style) {
    const display_device_ops_t *display_ops;
    const serial_device_ops_t *serial_ops;

    if (!str) {
        return;
    }

    if (serial_device) {
        serial_ops = (const serial_device_ops_t *)serial_device->ops;
        serial_ops->write(serial_device->context, str);
    }

    if (!display_device) {
        return;
    }

    display_ops = (const display_device_ops_t *)display_device->ops;
    if (console_has_framebuffer && console_display_is_framebuffer) {
        display_ops->write(display_device->context, str, fb_color_for(style), FB_BLACK);
    } else {
        display_ops->write(display_device->context, str, vga_color_for(style), FB_BLACK);
    }
}

void console_backspace(void) {
    const display_device_ops_t *display_ops;

    if (!display_device) {
        return;
    }

    display_ops = (const display_device_ops_t *)display_device->ops;
    display_ops->backspace(display_device->context, FB_BLACK);
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
