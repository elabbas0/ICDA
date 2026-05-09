#include <stdint.h>

#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"
#include "drivers/serial/serial.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"

#include "cpu/isr.h"

#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/pf.h"

#include "proc/sched.h"

// ─────────────────────────────────────────────
// demo kernel threads, this whole file is testing purposes only
// ─────────────────────────────────────────────
static void task_a(void) {
    while (1) {
        fb_print("A ", FB_CYAN, FB_BLACK);
        for (volatile int i = 0; i < 5000000; i++);
    }
}

static void task_b(void) {
    while (1) {
        fb_print("B ", FB_YELLOW, FB_BLACK);
        for (volatile int i = 0; i < 5000000; i++);
    }
}

// timer handler, drives the scheduler
static void timer_handler(struct registers *regs) {
    schedule(regs);
}

// kernel begin

void kernel_main(void *multiboot_info) {
    serial_init();

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
    if (pmm_free_frames() == 0) {
        fb_print("PMM: FAIL\n", FB_RED, FB_BLACK);
        pmm_print_stats();
        while (1) __asm__ volatile("cli; hlt");
    }
    fb_print("PMM: OK\n", FB_GREEN, FB_BLACK);

    // virtual memory manager — sets up paging + HHDM
    // fb_phys_addr/size are read back so vmm can map the framebuffer
    // into the higher-half direct map before we switch CR3
    if (vmm_init(fb_phys_addr(), fb_phys_size()) != 0) {
        fb_print("VMM: FAIL\n", FB_RED, FB_BLACK);
        while (1) __asm__ volatile("cli; hlt");
    }
    fb_print("VMM: OK\n", FB_GREEN, FB_BLACK);

    // page fault handler — enables dynamic stack growth
    pf_init();
    fb_print("PF:  OK\n", FB_GREEN, FB_BLACK);

    sched_init();
    fb_print("SCHED: OK\n", FB_GREEN, FB_BLACK);

    proc_create_kernel(task_a);
    proc_create_kernel(task_b);
    fb_print("TASKS: OK\n", FB_GREEN, FB_BLACK);

    irq_register(0, timer_handler);
    fb_print("TIMER: OK\n", FB_GREEN, FB_BLACK);

    __asm__ volatile("sti");
    fb_print("INTERRUPTS: OK\n\n", FB_GREEN, FB_BLACK);

    fb_print("Scheduler running. You should see A and B interleaving:\n\n",
             FB_WHITE, FB_BLACK);

    // idle loop — the boot context becomes the idle process
    while (1) {
        __asm__ volatile("hlt");
    }
}
