#include "vga.h"
// internal state
static int cursor_row = 0;
static int cursor_col = 0;
static unsigned char current_color = VGA_WHITE_ON_BLACK;
static char* vga = (char*)VGA_MEMORY;

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
    cursor_row   = 0;
    cursor_col   = 0;
    current_color = VGA_WHITE_ON_BLACK;
    vga_clear();
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

    char hex_chars[] = "0123456789ABCDEF";
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