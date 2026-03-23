#include "drivers/display/vga.h"
#include "drivers/display/framebuffer.h"
#include "cpu/multiboot2.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "memory/pmm.h"

void debug_char(char c)
{
    __asm__ volatile("mov $0x3F8, %%dx\noutb %0, %%dx\n" : : "a"(c) : "dx");
}

void debug_print(const char *str)
{
    for (int i = 0; str[i]; i++)
        debug_char(str[i]);
}

// timer tick counter
static volatile int ticks = 0;

// timer irq handler - called every tick
static void timer_handler(struct registers *regs)
{
    (void)regs;
    ticks++;
}

void kernel_main(void *multiboot_info)
{
    // init display
    int has_fb = fb_init(multiboot_info);
    if (!has_fb)
    {
        vga_init();
        vga_print("display: VGA fallback\n", VGA_WHITE_ON_BLACK);
    }

    fb_print("ICDA Kernel\n", FB_CYAN, FB_BLACK);
    fb_print("------------\n", FB_WHITE, FB_BLACK);

    fb_print("GDT:        ", FB_WHITE, FB_BLACK);
    gdt_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("PIC:        ", FB_WHITE, FB_BLACK);
    pic_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("IDT:        ", FB_WHITE, FB_BLACK);
    idt_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("PMM:        ", FB_WHITE, FB_BLACK);
    pmm_init(multiboot_info);

    

    irq_register(0, timer_handler);

    fb_print("Interrupts: ", FB_WHITE, FB_BLACK);
    __asm__ volatile("sti");
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("\nICDA ready.\n", FB_CYAN, FB_BLACK);

    while (1) __asm__ volatile("hlt");
}