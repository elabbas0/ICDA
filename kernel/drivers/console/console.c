#include "console.h"

#include "../device.h"
#include "../display/framebuffer.h"
#include "../display/font.h"
#include "../display/vga.h"

static kernel_device_t *display_device = 0;
static kernel_device_t *serial_device = 0;
static int console_display_is_framebuffer = 0;
static int console_serial_mirror_enabled = 1;

static int console_has_framebuffer = 0;
static int console_overlay_active = 0;
static console_style_t console_overlay_style = CONSOLE_STYLE_ACCENT;
static char console_overlay_text[80];
static int console_overlay_last_col = -1;
static int console_overlay_last_width = 0;

static uint64_t console_str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void console_copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

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

void console_refresh_overlay(void) {
    char line[160];
    char blank[160];
    int cols;
    int start_col;
    uint64_t overlay_len;
    console_style_t style = console_overlay_style;

    if (!console_overlay_active || !console_overlay_text[0]) {
        return;
    }

    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        cols = fb_columns();
    } else {
        cols = VGA_WIDTH;
    }
    if (cols <= 0) {
        return;
    }

    overlay_len = console_str_len(console_overlay_text);
    if (overlay_len > sizeof(line) - 1) {
        overlay_len = sizeof(line) - 1;
    }
    if ((int)overlay_len >= cols) {
        console_copy_text(line, &console_overlay_text[overlay_len - (uint64_t)(cols - 1)], sizeof(line));
        overlay_len = console_str_len(line);
        start_col = 0;
    } else {
        console_copy_text(line, console_overlay_text, sizeof(line));
        start_col = cols - (int)overlay_len - 1;
        if (start_col < 0) start_col = 0;
    }
    if (console_overlay_last_width > 0 && console_overlay_last_col >= 0) {
        for (int i = 0; i < console_overlay_last_width && i < (int)sizeof(blank) - 1; i++) blank[i] = ' ';
        blank[console_overlay_last_width < (int)sizeof(blank) - 1 ? console_overlay_last_width : (int)sizeof(blank) - 1] = 0;
        if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
            fb_write_at_cells(console_overlay_last_col, 0, blank, fb_color_for(CONSOLE_STYLE_INFO), FB_BLACK);
        } else {
            vga_write_at(0, console_overlay_last_col, blank, vga_color_for(CONSOLE_STYLE_INFO));
        }
    }

    console_overlay_last_col = start_col;
    console_overlay_last_width = (int)overlay_len;

    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        fb_write_at_cells(start_col, 0, line, fb_color_for(style), FB_BLACK);
    } else {
        vga_write_at(0, start_col, line, vga_color_for(style));
    }
}

void console_set_overlay_top_right(const char *text, console_style_t style) {
    console_overlay_active = (text && text[0]) ? 1 : 0;
    console_overlay_style = style;
    console_copy_text(console_overlay_text, text ? text : "", sizeof(console_overlay_text));
    if (!console_overlay_active) {
        console_clear_overlay_top_right();
        return;
    }
    console_refresh_overlay();
}

void console_clear_overlay_top_right(void) {
    char blank[160];

    console_overlay_active = 0;
    console_overlay_text[0] = 0;
    if (console_overlay_last_width <= 0 || console_overlay_last_col < 0) {
        return;
    }
    for (int i = 0; i < console_overlay_last_width && i < (int)sizeof(blank) - 1; i++) blank[i] = ' ';
    blank[console_overlay_last_width < (int)sizeof(blank) - 1 ? console_overlay_last_width : (int)sizeof(blank) - 1] = 0;

    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        fb_write_at_cells(console_overlay_last_col, 0, blank, fb_color_for(CONSOLE_STYLE_INFO), FB_BLACK);
    } else {
        vga_write_at(0, console_overlay_last_col, blank, vga_color_for(CONSOLE_STYLE_INFO));
    }
    console_overlay_last_col = -1;
    console_overlay_last_width = 0;
}

void console_set_serial_mirror(int enabled) {
    console_serial_mirror_enabled = enabled ? 1 : 0;
}

void console_clear(void) {
    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        fb_clear(FB_BLACK);
        console_refresh_overlay();
        return;
    }
    if (display_device) {
        vga_clear();
    }
    console_refresh_overlay();
}

void console_set_cursor(int x, int y) {
    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        fb_set_cursor(x, y);
        console_refresh_overlay();
        return;
    }
    vga_set_cursor_pos(x, y);
    console_refresh_overlay();
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

    if (serial_device && console_serial_mirror_enabled) {
        serial_ops = (const serial_device_ops_t *)serial_device->ops;
        serial_ops->write(serial_device->context, str);
    }

    if (console_has_framebuffer && console_display_is_framebuffer) {
        if (fb_available()) {
            fb_print(str, fb_color_for(style), FB_BLACK);
        }
    } else {
        (void)display_ops;
        vga_set_color(vga_color_for(style));
        vga_print(str, vga_color_for(style));
    }
    console_refresh_overlay();
}

void console_backspace(void) {
    if (console_has_framebuffer && console_display_is_framebuffer && fb_available()) {
        fb_backspace(FB_BLACK);
        console_refresh_overlay();
        return;
    }
    vga_backspace();
    console_refresh_overlay();
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
