#include "drivers/display/vga.h"

void debug_char(char c) {
    __asm__ volatile (
        "mov $0x3F8, %%dx\n"
        "outb %0, %%dx\n"
        : : "a"(c) : "dx"
    );
}
void debug_print(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) debug_char(str[i]);
}

void kernel_main() {
    vga_init();                                               debug_print("1\n");
    vga_print("ICDA Kernel\n", VGA_CYAN_ON_BLACK);           debug_print("2\n");
    vga_print("------------\n", VGA_WHITE_ON_BLACK);         debug_print("3\n");
    vga_print("Status: ", VGA_WHITE_ON_BLACK);               debug_print("4\n");
    vga_print("RUNNING\n", VGA_GREEN_ON_BLACK);              debug_print("5\n");
    vga_print("Architecture: ", VGA_WHITE_ON_BLACK);         debug_print("6\n");
    vga_print("x86_64\n", VGA_YELLOW_ON_BLACK);              debug_print("7\n");
    vga_print("Display: ", VGA_WHITE_ON_BLACK);              debug_print("8\n");
    vga_print("VGA Fallback (80x25)\n", VGA_WHITE_ON_BLACK); debug_print("9\n");
    vga_print("\nTest int: ", VGA_WHITE_ON_BLACK);           debug_print("10\n");
    vga_print_int(1234, VGA_YELLOW_ON_BLACK);                debug_print("11\n");
    vga_print("\nTest hex: ", VGA_WHITE_ON_BLACK);           debug_print("12\n");
    vga_print_hex(0xFF3A, VGA_CYAN_ON_BLACK);                debug_print("13\n");
    vga_print("\nTest negative: ", VGA_WHITE_ON_BLACK);      debug_print("14\n");
    vga_print_int(-42, VGA_RED_ON_BLACK);                    debug_print("15\n");
    vga_print("\n\nVGA driver OK.\n", VGA_GREEN_ON_BLACK);   debug_print("16\n");
    while(1);
}