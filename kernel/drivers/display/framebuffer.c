#include "framebuffer.h"
#include "font.h"
#include "../device.h"
#include "../../cpu/multiboot2.h"
#include <stdint.h>

// internal state
static volatile uint32_t* fb_addr   = 0;
static uint64_t           fb_phys   = 0;
static uint32_t           fb_pitch  = 0;
static uint32_t           fb_bpp    = 0;
static int                fb_ready  = 0;
static int                fb_hhdm   = 0;

// cursor position in characters
static int cursor_x = 0;
static int cursor_y = 0;
static kernel_device_t framebuffer_device;

static void fb_fill_bytes(void *dst, uint8_t value, uint64_t size) {
    uint8_t *out8 = (uint8_t *)dst;

    while (((uintptr_t)out8 & 7ULL) && size) {
        *out8++ = value;
        size--;
    }

    if (size >= 8) {
        uint64_t pattern = 0x0101010101010101ULL * value;
        uint64_t *out64 = (uint64_t *)out8;
        uint64_t words = size / 8;

        for (uint64_t i = 0; i < words; i++) {
            out64[i] = pattern;
        }

        out8 = (uint8_t *)(out64 + words);
        size &= 7ULL;
    }

    while (size--) {
        *out8++ = value;
    }
}

static void fb_copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *out8 = (uint8_t *)dst;
    const uint8_t *in8 = (const uint8_t *)src;

    while ((((uintptr_t)out8 | (uintptr_t)in8) & 7ULL) && size) {
        *out8++ = *in8++;
        size--;
    }

    if (size >= 8) {
        uint64_t *out64 = (uint64_t *)out8;
        const uint64_t *in64 = (const uint64_t *)in8;
        uint64_t words = size / 8;

        for (uint64_t i = 0; i < words; i++) {
            out64[i] = in64[i];
        }

        out8 = (uint8_t *)(out64 + words);
        in8 = (const uint8_t *)(in64 + words);
        size &= 7ULL;
    }

    while (size--) {
        *out8++ = *in8++;
    }
}

static void framebuffer_device_clear(void *context) {
    (void)context;
    fb_clear(FB_BLACK);
}

static void framebuffer_device_write(void *context, const char *str, uint32_t fg, uint32_t bg) {
    (void)context;
    fb_print(str, fg, bg);
}

static void framebuffer_device_backspace(void *context, uint32_t bg) {
    (void)context;
    fb_backspace(bg);
}

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

            fb_phys   = (uint64_t)fb_tag->framebuffer_addr;
            fb_addr   = (volatile uint32_t*)(uintptr_t)fb_phys;
            fb_pitch  = fb_tag->framebuffer_pitch;
            fb_bpp    = fb_tag->framebuffer_bpp;
            fb_width  = (int)fb_tag->framebuffer_width;
            fb_height = (int)fb_tag->framebuffer_height;
            fb_ready  = 1;
            fb_hhdm   = 0;

            framebuffer_device.name = "framebuffer";
            framebuffer_device.class_id = DEVICE_CLASS_DISPLAY;
            static const display_device_ops_t ops = {
                .clear = framebuffer_device_clear,
                .write = framebuffer_device_write,
                .backspace = framebuffer_device_backspace
            };
            framebuffer_device.ops = &ops;
            framebuffer_device.context = 0;
            framebuffer_device.next = 0;
            device_register(&framebuffer_device);

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

// return the raw physical address of the framebuffer (0 if not ready)
uint64_t fb_phys_addr(void) {
    return fb_ready ? fb_phys : 0;
}

// return the size of the framebuffer in bytes
uint64_t fb_phys_size(void) {
    return fb_ready ? (uint64_t)fb_pitch * (uint64_t)fb_height : 0;
}

// adjust fb_addr to point through the HHDM; call once after vmm_init
void fb_remap(uint64_t physical_base) {
    if (!fb_ready || fb_hhdm) return;
    fb_addr = (volatile uint32_t *)(fb_phys + physical_base);
    fb_hhdm = 1;
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

    if (fb_bpp == 32) {
        uint32_t *row = (uint32_t *)(uintptr_t)fb_addr;
        uint32_t pixels_per_row = fb_pitch / 4;

        for (int y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < pixels_per_row; x++) {
                row[x] = color;
            }
            row = (uint32_t *)((uint8_t *)row + fb_pitch);
        }
    } else {
        for (int y = 0; y < fb_height; y++) {
            for (int x = 0; x < fb_width; x++) {
                volatile uint32_t* pixel = (volatile uint32_t*)((uint8_t*)fb_addr + y * fb_pitch + x * (fb_bpp / 8));
                *pixel = color;
            }
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

    for (int row = 0; row < FONT_CELL_HEIGHT; row++) {
        for (int col = 0; col < FONT_CELL_WIDTH; col++) {
            fb_put_pixel(x + col, y + row, bg);
        }
    }

    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            // MSB is leftmost pixel
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row + 1, color);
        }
    }
}

// ============================================================
// scroll
static void fb_scroll() {
    if (!fb_ready) return;
    uint64_t row_bytes = (uint64_t)FONT_CELL_HEIGHT * (uint64_t)fb_pitch;

    // move all rows up by one character height
    uint8_t* dst = (uint8_t*)fb_addr;
    uint8_t* src = (uint8_t*)fb_addr + row_bytes;
    uint64_t copy_size = (uint64_t)fb_pitch * (uint64_t)(fb_height - FONT_CELL_HEIGHT);

    fb_copy_bytes(dst, src, copy_size);

    // clear last row
    uint8_t* last_row = (uint8_t*)fb_addr + fb_pitch * (fb_height - FONT_CELL_HEIGHT);
    fb_fill_bytes(last_row, 0, (uint64_t)fb_pitch * (uint64_t)FONT_CELL_HEIGHT);

    cursor_y--;
    if (cursor_y < 0) cursor_y = 0;
}

// ============================================================
// char and string output
void fb_newline() {
    cursor_x = 0;
    cursor_y++;
    int max_rows = fb_height / FONT_CELL_HEIGHT;
    if (cursor_y >= max_rows) {
        fb_scroll();
    }
}

void fb_backspace(uint32_t bg) {
    if (!fb_ready) return;
    if (cursor_x == 0) return;

    cursor_x--;
    fb_draw_char(cursor_x * FONT_CELL_WIDTH, cursor_y * FONT_CELL_HEIGHT, ' ', FB_WHITE, bg);
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

    int max_cols = fb_width / FONT_CELL_WIDTH;

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

        fb_draw_char(cursor_x * FONT_CELL_WIDTH, cursor_y * FONT_CELL_HEIGHT, c, fg, bg);
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
