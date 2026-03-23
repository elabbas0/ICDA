#include "framebuffer.h"
#include "font.h"
#include "../../multiboot2.h"
#include <stdint.h>

// internal state
static volatile uint32_t* fb_addr   = 0;
static uint32_t fb_pitch            = 0;
static uint32_t fb_bpp              = 0;
static int      fb_ready            = 0;

// cursor position in characters
static int cursor_x = 0;
static int cursor_y = 0;

// public screen dimensions
int fb_width  = 0;
int fb_height = 0;


// ============================================================
// parse multiboot2 info to find framebuffer tag
int fb_init(void* multiboot_info) {
    if (!multiboot_info) return 0;

    struct multiboot_info* info = (struct multiboot_info*)multiboot_info;

    uint8_t* tag_ptr = (uint8_t*)multiboot_info + 8;

    uint8_t* end_ptr = (uint8_t*)multiboot_info + info->total_size;

    while (tag_ptr < end_ptr) {
        struct multiboot_tag* tag = (struct multiboot_tag*)tag_ptr;

        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;

        if (tag->type == MULTIBOOT_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot_tag_framebuffer* fb_tag =
                (struct multiboot_tag_framebuffer*)tag;

            fb_addr   = (volatile uint32_t*)(uintptr_t)fb_tag->framebuffer_addr;
            fb_pitch  = fb_tag->framebuffer_pitch;
            fb_bpp    = fb_tag->framebuffer_bpp;
            fb_width  = (int)fb_tag->framebuffer_width;
            fb_height = (int)fb_tag->framebuffer_height;
            fb_ready  = 1;

            fb_clear(FB_BLACK);
            return 1;
        }

        uint32_t next = (tag->size + (MULTIBOOT_TAG_ALIGN - 1)) & ~(MULTIBOOT_TAG_ALIGN - 1);
        tag_ptr += next;
    }

    return 0;
}

int fb_available() {
    return fb_ready;
}

// ============================================================
// pixel operations
void fb_put_pixel(int x, int y, uint32_t color) {
    if (!fb_ready) return;
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    volatile uint32_t* pixel = (volatile uint32_t*)((uint8_t*)fb_addr + y * fb_pitch + x * (fb_bpp / 8));
    *pixel = color;
}

void fb_clear(uint32_t color) {
    if (!fb_ready) return;
    for (int y = 0; y < fb_height; y++) {
        for (int x = 0; x < fb_width; x++) {
            volatile uint32_t* pixel = (volatile uint32_t*)((uint8_t*)fb_addr + y * fb_pitch + x * (fb_bpp / 8));
            *pixel = color;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

// ============================================================
// render font
void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!fb_ready) return;
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';

    const unsigned char* glyph = font_data[c - FONT_FIRST];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            // MSB is leftmost pixel
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

// ============================================================
// scroll
static void fb_scroll() {
    if (!fb_ready) return;
    int row_bytes = FONT_HEIGHT * fb_pitch;

    // move all rows up by one character height
    uint8_t* dst = (uint8_t*)fb_addr;
    uint8_t* src = (uint8_t*)fb_addr + row_bytes;
    int copy_size = fb_pitch * (fb_height - FONT_HEIGHT);

    for (int i = 0; i < copy_size; i++) {
        dst[i] = src[i];
    }

    // clear last row
    uint8_t* last_row = (uint8_t*)fb_addr + fb_pitch * (fb_height - FONT_HEIGHT);
    for (int i = 0; i < (int)(fb_pitch * FONT_HEIGHT); i++) {
        last_row[i] = 0;
    }

    cursor_y--;
    if (cursor_y < 0) cursor_y = 0;
}

// ============================================================
// char and string output
void fb_newline() {
    cursor_x = 0;
    cursor_y++;
    int max_rows = fb_height / FONT_HEIGHT;
    if (cursor_y >= max_rows) {
        fb_scroll();
    }
}

void fb_set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}

void fb_print_at(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    cursor_x = x;
    cursor_y = y;
    fb_print(str, fg, bg);
}

void fb_print(const char* str, uint32_t fg, uint32_t bg) {
    if (!fb_ready) return;
    int max_cols = fb_width / FONT_WIDTH;

    for (int i = 0; str[i] != '\0'; i++) {
        char c = str[i];

        if (c == '\n') {
            fb_newline();
            continue;
        }

        if (c == '\r') {
            cursor_x = 0;
            continue;
        }

        if (c == '\t') {
            cursor_x = (cursor_x + 4) & ~3;
            if (cursor_x >= max_cols) fb_newline();
            continue;
        }

        fb_draw_char(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT, c, fg, bg);
        cursor_x++;

        if (cursor_x >= max_cols) {
            fb_newline();
        }
    }
}

void fb_print_int(int n, uint32_t fg, uint32_t bg) {
    if (n < 0) {
        fb_print("-", fg, bg);
        n = -n;
    }
    if (n == 0) {
        fb_print("0", fg, bg);
        return;
    }
    char buf[20];
    int len = 0;
    while (n > 0) {
        buf[len++] = '0' + (n % 10);
        n /= 10;
    }
    for (int i = len - 1; i >= 0; i--) {
        char tmp[2] = {buf[i], '\0'};
        fb_print(tmp, fg, bg);
    }
}

void fb_print_hex(unsigned int n, uint32_t fg, uint32_t bg) {
    fb_print("0x", fg, bg);
    if (n == 0) {
        fb_print("0", fg, bg);
        return;
    }
    const char* hex_chars = "0123456789ABCDEF";
    char buf[8];
    int len = 0;
    while (n > 0) {
        buf[len++] = hex_chars[n % 16];
        n /= 16;
    }
    for (int i = len - 1; i >= 0; i--) {
        char tmp[2] = {buf[i], '\0'};
        fb_print(tmp, fg, bg);
    }
}