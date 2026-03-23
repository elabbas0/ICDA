#include "drivers/display/vga.h"
#include "drivers/display/framebuffer.h"
#include "multiboot2.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"

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

    // print header
    fb_print("ICDA Kernel\n", FB_CYAN, FB_BLACK);
    fb_print("------------\n", FB_WHITE, FB_BLACK);
    fb_print("Status: ", FB_WHITE, FB_BLACK);
    fb_print("RUNNING\n", FB_GREEN, FB_BLACK);

    // init GDT
    fb_print("GDT: ", FB_WHITE, FB_BLACK);
    gdt_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    // init PIC
    fb_print("PIC: ", FB_WHITE, FB_BLACK);
    pic_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    // init IDT
    fb_print("IDT: ", FB_WHITE, FB_BLACK);
    idt_init();
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    // enable interrupts
    fb_print("Interrupts: ", FB_WHITE, FB_BLACK);
    __asm__ volatile("sti");
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("\nStage 3 complete.\n", FB_GREEN, FB_BLACK);


   
}