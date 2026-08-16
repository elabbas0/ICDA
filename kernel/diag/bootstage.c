#include "bootstage.h"

#include "../drivers/display/framebuffer.h"
#include "../drivers/display/vga.h"
#include "../drivers/serial/serial.h"

#include <stdint.h>

static volatile uint32_t current_stage = 0;
static const char *current_label = "reset";

static void append_char(char *buf, int *len, int max_len, char c) {
    if (*len >= max_len - 1) {
        return;
    }
    buf[*len] = c;
    (*len)++;
    buf[*len] = '\0';
}

static void append_str(char *buf, int *len, int max_len, const char *str) {
    if (!str) {
        return;
    }
    while (*str) {
        append_char(buf, len, max_len, *str++);
    }
}

static void append_dec(char *buf, int *len, int max_len, uint32_t value) {
    char digits[10];
    int count = 0;

    if (value == 0) {
        append_char(buf, len, max_len, '0');
        return;
    }

    while (value && count < (int)sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (count > 0) {
        append_char(buf, len, max_len, digits[--count]);
    }
}

void bootstage_set(uint32_t stage, const char *label) {
    char stamp[64];
    int len = 0;

    current_stage = stage;
    current_label = label ? label : "";

    append_char(stamp, &len, (int)sizeof(stamp), '[');
    append_char(stamp, &len, (int)sizeof(stamp), 'S');
    append_dec(stamp, &len, (int)sizeof(stamp), stage);
    if (label && *label) {
        append_char(stamp, &len, (int)sizeof(stamp), ' ');
        append_str(stamp, &len, (int)sizeof(stamp), label);
    }
    append_char(stamp, &len, (int)sizeof(stamp), ']');

    if (serial_ready()) {
        serial_write("[stage] ");
        serial_write(stamp);
        serial_write("\n");
    }

    if (fb_available()) {
        /* Draw the stamp in place without touching the console cursor:
         * fb_print_at() would move the cursor to (8,8), so every boot
         * line written after a stamp would overwrite the previous one
         * on the same row and the real log would be invisible. */
        fb_write_at_cells(8, 8, stamp, FB_YELLOW, FB_BLACK);
    } else {
        vga_write_at(0, 60, stamp, VGA_YELLOW_ON_BLACK);
    }
}

uint32_t bootstage_current(void) {
    return current_stage;
}

const char *bootstage_label(void) {
    return current_label;
}
