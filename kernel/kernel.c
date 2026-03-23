#include "drivers/display/vga.h"
#include "drivers/display/framebuffer.h"
#include "multiboot2.h"

void debug_char(char c) {
    __asm__ volatile ("mov $0x3F8, %%dx\noutb %0, %%dx\n" : : "a"(c) : "dx");
}

void debug_print(const char* str) {
    for (int i = 0; str[i]; i++) debug_char(str[i]);
}

void kernel_main(void* multiboot_info) {
    debug_print("1\n");
    int has_fb = fb_init(multiboot_info);
    debug_print("2\n");

    if (has_fb) {
        debug_print("3-fb\n");
        fb_clear(FB_BLACK);
        debug_print("4\n");
        fb_print("ICDA (aka I CAN DO ANYTHING) by elabbas\n",              FB_CYAN,   FB_BLACK);
        fb_print("------------\n",             FB_WHITE,  FB_BLACK);
        fb_print("Status: ",                   FB_WHITE,  FB_BLACK);
        fb_print("RUNNING\n",                  FB_GREEN,  FB_BLACK);
        fb_print("Architecture: ",             FB_WHITE,  FB_BLACK);
        fb_print("x86_64\n",                   FB_YELLOW, FB_BLACK);
        fb_print("Display: ",                  FB_WHITE,  FB_BLACK);
        fb_print("Framebuffer ",               FB_GREEN,  FB_BLACK);
        fb_print_int(fb_width,                 FB_CYAN,   FB_BLACK);
        fb_print("x",                          FB_WHITE,  FB_BLACK);
        fb_print_int(fb_height,                FB_CYAN,   FB_BLACK);
        fb_print("\n",                         FB_WHITE,  FB_BLACK);
        fb_print("\nFramebuffer driver OK.\n", FB_GREEN,  FB_BLACK);
        debug_print("5\n");
    } else {
        debug_print("3-vga\n");
        vga_init();
        vga_print("ICDA Kernel\n",             VGA_CYAN_ON_BLACK);
        vga_print("------------\n",            VGA_WHITE_ON_BLACK);
        vga_print("Status: ",                  VGA_WHITE_ON_BLACK);
        vga_print("RUNNING\n",                 VGA_GREEN_ON_BLACK);
        vga_print("Architecture: ",            VGA_WHITE_ON_BLACK);
        vga_print("x86_64\n",                  VGA_YELLOW_ON_BLACK);
        vga_print("Display: ",                 VGA_WHITE_ON_BLACK);
        vga_print("VGA Fallback\n",            VGA_WHITE_ON_BLACK);
        debug_print("4-vga\n");
    }

    while(1);
}