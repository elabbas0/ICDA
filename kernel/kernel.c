#include <stdint.h>

#include "drivers/console/console.h"
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
    console_init(has_fb);

    console_write("ICDA Kernel Boot\n\n", CONSOLE_STYLE_INFO);

    gdt_init();
    console_write_status("GDT", "OK", CONSOLE_STYLE_OK);

    idt_init();
    console_write_status("IDT", "OK", CONSOLE_STYLE_OK);

    pic_init();
    console_write_status("PIC", "OK", CONSOLE_STYLE_OK);

    // physical memory manager — must come before vmm
    pmm_init(multiboot_info);
    if (pmm_free_frames() == 0) {
        console_write_status("PMM", "FAIL", CONSOLE_STYLE_ERROR);
        pmm_print_stats();
        while (1) __asm__ volatile("cli; hlt");
    }
    console_write_status("PMM", "OK", CONSOLE_STYLE_OK);

    // virtual memory manager — sets up paging + HHDM
    // fb_phys_addr/size are read back so vmm can map the framebuffer
    // into the higher-half direct map before we switch CR3
    if (vmm_init(fb_phys_addr(), fb_phys_size()) != 0) {
        console_write_status("VMM", "FAIL", CONSOLE_STYLE_ERROR);
        while (1) __asm__ volatile("cli; hlt");
    }
    console_write_status("VMM", "OK", CONSOLE_STYLE_OK);

    // page fault handler — enables dynamic stack growth
    pf_init();
    console_write_status("PF", "OK", CONSOLE_STYLE_OK);

    sched_init();
    console_write_status("SCHED", "OK", CONSOLE_STYLE_OK);

    proc_create_kernel(task_a);
    proc_create_kernel(task_b);
    console_write_status("TASKS", "OK", CONSOLE_STYLE_OK);

    irq_register(0, timer_handler);
    console_write_status("TIMER", "OK", CONSOLE_STYLE_OK);

    __asm__ volatile("sti");
    console_write_status("INTERRUPTS", "OK", CONSOLE_STYLE_OK);
    console_write("\n", CONSOLE_STYLE_INFO);

    console_write("Scheduler running. You should see A and B interleaving:\n\n",
                  CONSOLE_STYLE_INFO);

    // idle loop — the boot context becomes the idle process
    while (1) {
        __asm__ volatile("hlt");
    }
}
