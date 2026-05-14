#include "vga.h"
#include "../device.h"
// internal state
static int cursor_row = 0;
static int cursor_col = 0;
static unsigned char current_color = VGA_WHITE_ON_BLACK;
static volatile char* vga;
static kernel_device_t vga_device;

static void vga_device_clear(void *context) {
    (void)context;
    vga_clear();
}

static void vga_device_write(void *context, const char *str, uint32_t fg, uint32_t bg) {
    (void)context;
    (void)bg;
    current_color = (unsigned char)fg;
    vga_print(str, current_color);
}

static void vga_device_backspace(void *context, uint32_t bg) {
    (void)context;
    (void)bg;
    vga_backspace();
}

// ============================================================
// helpers
// write a character + color directly to VGA memory at a position
static void vga_write_cell(int row, int col, char c, unsigned char color) {
    int index = (row * VGA_WIDTH + col) * 2;
    vga[index]     = c;
    vga[index + 1] = color;
}
// scroll screen up by one line when we hit the bottom
static void vga_scroll() {
    // move every row one row up
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            int dst = ((row - 1) * VGA_WIDTH + col) * 2;
            int src = (row       * VGA_WIDTH + col) * 2;
            vga[dst]     = vga[src];
            vga[dst + 1] = vga[src + 1];
        }
    }
    // clear the last row
    for (int col = 0; col < VGA_WIDTH; col++) {
        vga_write_cell(VGA_HEIGHT - 1, col, ' ', current_color);
    }
    // move cursor to start of last row
    cursor_row = VGA_HEIGHT - 1;
    cursor_col = 0;
}

// ============================================================
// public API
// initialize VGA 
void vga_init() {
    vga = (volatile char*)(unsigned long long)0xB8000;
    cursor_row = 0;
    cursor_col = 0;
    current_color = VGA_WHITE_ON_BLACK;
    vga_clear();

    vga_device.name = "vga";
    vga_device.class_id = DEVICE_CLASS_DISPLAY;
    static const display_device_ops_t ops = {
        .clear = vga_device_clear,
        .write = vga_device_write,
        .backspace = vga_device_backspace
    };
    vga_device.ops = &ops;
    vga_device.context = 0;
    vga_device.next = 0;
    device_register(&vga_device);
}
// fill entire screen with blank characters
void vga_clear() {
    for (int row = 0; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            vga_write_cell(row, col, ' ', current_color);
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

// set default color for future prints
void vga_set_color(unsigned char color) {
    current_color = color;
}

// handle newline and scroll if we hit the bottom
void vga_newline() {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= VGA_HEIGHT) {
        vga_scroll();
    }
}

void vga_backspace(void) {
    if (cursor_col == 0) {
        if (cursor_row == 0) {
            return;
        }
        cursor_row--;
        cursor_col = VGA_WIDTH;
    }

    cursor_col--;
    vga_write_cell(cursor_row, cursor_col, ' ', current_color);
}

void vga_set_cursor_pos(int col, int row) {
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    cursor_col = col;
    cursor_row = row;
}

void vga_write_at(int row, int col, const char *str, unsigned char color) {
    int out_row = row;
    int out_col = col;

    if (!str) {
        return;
    }

    while (*str && out_row >= 0 && out_row < VGA_HEIGHT) {
        if (*str == '\n') {
            out_row++;
            out_col = col;
            str++;
            continue;
        }

        if (out_col >= 0 && out_col < VGA_WIDTH) {
            vga_write_cell(out_row, out_col, *str, color);
        }

        out_col++;
        if (out_col >= VGA_WIDTH) {
            break;
        }
        str++;
    }
}

// print a single character at current cursor position
void vga_putchar(char c, unsigned char color) {
    if (c == '\n') {
        vga_newline();
        return;
    }

    if (c == '\r') {
        cursor_col = 0;
        return;
    }

    if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;
        if (cursor_col >= VGA_WIDTH) {
            vga_newline();
        }
        return;
    }

    vga_write_cell(cursor_row, cursor_col, c, color);
    cursor_col++;

    // wrap to next line if we hit the edge
    if (cursor_col >= VGA_WIDTH) {
        vga_newline();
    }
}

// print a null-terminated string
void vga_print(const char* str, unsigned char color) {
    for (int i = 0; str[i] != '\0'; i++) {
        vga_putchar(str[i], color);
    }
}

// print a signed integer
void vga_print_int(int n, unsigned char color) {
    if (n < 0) {
        vga_putchar('-', color);
        n = -n;
    }

    if (n == 0) {
        vga_putchar('0', color);
        return;
    }

    // build digits in reverse
    char buf[20];
    int len = 0;
    while (n > 0) {
        buf[len++] = '0' + (n % 10);
        n /= 10;
    }

    for (int i = len - 1; i >= 0; i--) {
        vga_putchar(buf[i], color);
    }
}

// print an unsigned int as hexadecimal e.g. 0x1F3A
void vga_print_hex(unsigned int n, unsigned char color) {
    vga_print("0x", color);

    const char* hex_chars = "0123456789ABCDEF";
    char buf[8];
    int len = 0;

    if (n == 0) {
        vga_putchar('0', color);
        return;
    }

    while (n > 0) {
        buf[len++] = hex_chars[n % 16];
        n /= 16;
    }

    for (int i = len - 1; i >= 0; i--) {
        vga_putchar(buf[i], color);
    }
}
