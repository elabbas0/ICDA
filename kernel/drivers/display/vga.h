#ifndef VGA_H
#define VGA_H
// ============================================================
#define VGA_WIDTH    80
#define VGA_HEIGHT   25
#define VGA_MEMORY ((volatile char*)(unsigned long long)0xB8000)

#define VGA_BLACK        0x0
#define VGA_BLUE         0x1
#define VGA_GREEN        0x2
#define VGA_CYAN         0x3
#define VGA_RED          0x4
#define VGA_MAGENTA      0x5
#define VGA_BROWN        0x6
#define VGA_WHITE        0x7
#define VGA_DARK_GRAY    0x8
#define VGA_LIGHT_BLUE   0x9
#define VGA_LIGHT_GREEN  0xA
#define VGA_LIGHT_CYAN   0xB
#define VGA_LIGHT_RED    0xC
#define VGA_PINK         0xD
#define VGA_YELLOW       0xE
#define VGA_BRIGHT_WHITE 0xF

// combine foreground + background into one attribute byte
#define VGA_COLOR(fg, bg) ((bg << 4) | fg)

// common combos
#define VGA_WHITE_ON_BLACK  VGA_COLOR(VGA_WHITE,       VGA_BLACK)
#define VGA_RED_ON_BLACK    VGA_COLOR(VGA_RED,         VGA_BLACK)
#define VGA_GREEN_ON_BLACK  VGA_COLOR(VGA_GREEN,       VGA_BLACK)
#define VGA_CYAN_ON_BLACK   VGA_COLOR(VGA_CYAN,        VGA_BLACK)
#define VGA_YELLOW_ON_BLACK VGA_COLOR(VGA_YELLOW,      VGA_BLACK)
#define VGA_BLACK_ON_WHITE  VGA_COLOR(VGA_BLACK,       VGA_WHITE)
// ============================================================
// Functions
void vga_init();
void vga_clear();
void vga_putchar(char c, unsigned char color);
void vga_print(const char* str, unsigned char color);
void vga_print_int(int n, unsigned char color);
void vga_print_hex(unsigned int n, unsigned char color);
void vga_newline();
void vga_set_color(unsigned char color);

#endif