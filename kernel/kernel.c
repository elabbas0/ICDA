#include <stdint.h>

#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"

#include "cpu/isr.h"

#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/pf.h"

// simple timer tick counter (IRQ0)
volatile uint64_t ticks = 0;

static void timer_handler(struct registers *regs) {
    (void)regs;
    ticks++;
}

// crude delay using PIT ticks (~18.2 Hz default in your setup)
static void sleep_seconds(int seconds) {
    uint64_t target = ticks + (uint64_t)(seconds * 18);
    while (ticks < target) {
        __asm__ volatile("hlt");
    }
}

// kernel begin
void kernel_main(void *multiboot_info) {
    // framebuffer or fallback VGA
    int has_fb = fb_init(multiboot_info);
    if (!has_fb) {
        vga_init();
    }

    fb_print("ICDA Kernel Boot\n\n", FB_CYAN, FB_BLACK);

    gdt_init();
    fb_print("GDT: OK\n", FB_GREEN, FB_BLACK);

    idt_init();
    fb_print("IDT: OK\n", FB_GREEN, FB_BLACK);

    pic_init();
    fb_print("PIC: OK\n", FB_GREEN, FB_BLACK);

    // physical memory manager — must come before vmm
    pmm_init(multiboot_info);
    fb_print("PMM: OK\n", FB_GREEN, FB_BLACK);

    // virtual memory manager — sets up paging + HHDM
    // fb_phys_addr/size are read back so vmm can map the framebuffer
    // into the higher-half direct map before we switch CR3
    vmm_init(fb_phys_addr(), fb_phys_size());
    fb_print("VMM: OK\n", FB_GREEN, FB_BLACK);

    // page fault handler — enables dynamic stack growth
    pf_init();
    fb_print("PF:  OK\n", FB_GREEN, FB_BLACK);

    irq_register(0, timer_handler);
    fb_print("TIMER: OK\n", FB_GREEN, FB_BLACK);

    __asm__ volatile("sti");
    fb_print("INTERRUPTS: OK\n", FB_GREEN, FB_BLACK);

    fb_print("\nStage 3 complete.\n", FB_WHITE, FB_BLACK);

    fb_print("\nWaiting 5 seconds...\n", FB_YELLOW, FB_BLACK);
    sleep_seconds(5);

    // ── scrolling test ────────────────────────
    fb_print("\n--- SCROLL TEST START ---\n\n", FB_CYAN, FB_BLACK);

    while (1) {
        __asm__ volatile("hlt");
    }
}