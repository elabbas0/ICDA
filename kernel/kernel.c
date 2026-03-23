#include "drivers/display/vga.h"
#include "drivers/display/framebuffer.h"
#include "multiboot2.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "pmm.h"

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

    // ── PMM smoke test ────────────────────────────────────────
    fb_print("\n-- PMM smoke test --\n", FB_YELLOW, FB_BLACK);

    uint64_t frames[4];
    for (int i = 0; i < 4; i++) {
        frames[i] = pmm_alloc();
        fb_print("  alloc -> ", FB_WHITE, FB_BLACK);
        fb_print_hex((unsigned int)(frames[i] >> 32), FB_CYAN, FB_BLACK);
        fb_print_hex((unsigned int)(frames[i] & 0xFFFFFFFF), FB_CYAN, FB_BLACK);
        fb_print("\n", FB_WHITE, FB_BLACK);
    }

    uint64_t free_before = pmm_free_frames();
    for (int i = 0; i < 4; i++)
        pmm_free(frames[i]);
    uint64_t free_after = pmm_free_frames();

    fb_print("  freed 4 frames: ", FB_WHITE, FB_BLACK);
    if (free_after == free_before + 4)
        fb_print("OK\n", FB_GREEN, FB_BLACK);
    else
        fb_print("FAIL\n", FB_RED, FB_BLACK);

    uint64_t free_check = pmm_free_frames();
    pmm_free(frames[0]);
    if (pmm_free_frames() == free_check)
        fb_print("  double-free guard: OK\n", FB_GREEN, FB_BLACK);
    else
        fb_print("  double-free guard: FAIL\n", FB_RED, FB_BLACK);

    fb_print("-- smoke test done --\n\n", FB_YELLOW, FB_BLACK);
    // ─────────────────────────────────────────────────────────

    irq_register(0, timer_handler);

    fb_print("Interrupts: ", FB_WHITE, FB_BLACK);
    __asm__ volatile("sti");
    fb_print("OK\n", FB_GREEN, FB_BLACK);

    fb_print("\nICDA ready.\n", FB_CYAN, FB_BLACK);

    while (1) __asm__ volatile("hlt");
}