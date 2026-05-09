#include <stdint.h>

#include "drivers/console/console.h"
#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"
#include "drivers/input/input.h"
#include "drivers/input/keyboard.h"
#include "drivers/serial/serial.h"
#include "fs/initramfs.h"
#include "fs/vfs.h"
#include "syscall/syscall.h"
#include "tty/tty.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq_controller.h"
#include "cpu/isr.h"

#include "memory/pf.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#include "proc/sched.h"

static void timer_handler(struct registers *regs) {
    schedule(regs);
}

static void boot_prefix(const char *topic) {
    console_write("[boot] ", CONSOLE_STYLE_MUTED);
    console_write(topic, CONSOLE_STYLE_ACCENT);
    console_write(": ", CONSOLE_STYLE_MUTED);
}

static void boot_line(const char *topic, const char *message) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void boot_line_dec(const char *topic, const char *message, uint64_t value, const char *suffix) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_INFO);
    console_write_dec64(value, CONSOLE_STYLE_INFO);
    if (suffix) {
        console_write(suffix, CONSOLE_STYLE_INFO);
    }
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void boot_halt(const char *topic, const char *message) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void kernel_main(void *multiboot_info) {
    serial_init();

    int has_fb = fb_init(multiboot_info);
    if (!has_fb) {
        vga_init();
    }
    console_init(has_fb);

    console_write("ICDA Boot Sequence\n\n", CONSOLE_STYLE_INFO);
    if (has_fb) {
        boot_prefix("display");
        console_write("framebuffer attached ", CONSOLE_STYLE_INFO);
        console_write_dec64((uint64_t)fb_width, CONSOLE_STYLE_INFO);
        console_write("x", CONSOLE_STYLE_MUTED);
        console_write_dec64((uint64_t)fb_height, CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("display", "vga fallback attached");
    }

    gdt_init();
    boot_line("cpu", "gdt loaded");

    idt_init();
    boot_line("cpu", "idt loaded");

    pmm_init(multiboot_info);
    if (pmm_free_frames() == 0) {
        boot_prefix("memory");
        console_write("no free frames discovered\n", CONSOLE_STYLE_ERROR);
        pmm_print_stats();
        boot_halt("memory", "physical memory manager refused to start");
    }
    boot_prefix("memory");
    console_write("pmm online, free=", CONSOLE_STYLE_INFO);
    console_write_dec64(pmm_free_frames(), CONSOLE_STYLE_INFO);
    console_write(" total=", CONSOLE_STYLE_MUTED);
    console_write_dec64(pmm_total_frames(), CONSOLE_STYLE_INFO);
    console_write(" frames\n", CONSOLE_STYLE_INFO);

    if (vmm_init(fb_phys_addr(), fb_phys_size()) != 0) {
        boot_halt("memory", "virtual memory manager failed to map kernel space");
    }
    boot_line("memory", "higher-half mappings active");

    if (heap_init() != 0) {
        boot_halt("memory", "kernel heap allocator failed to initialize");
    }
    boot_line_dec("memory", "kernel heap reserved ", heap_bytes_total(), " bytes");

    if (irq_controller_init(multiboot_info) != 0) {
        boot_halt("interrupts", "failed to enable local apic / ioapic");
    }
    boot_prefix("interrupts");
    console_write(irq_controller_name(), CONSOLE_STYLE_INFO);
    console_write(" active\n", CONSOLE_STYLE_INFO);

    pf_init();
    boot_line("interrupts", "page fault handler armed");

    sched_init();
    boot_line("scheduler", "scheduler core online");

    irq_register(0, timer_handler);
    boot_line("timer", "irq0 handler registered");

    keyboard_init();
    boot_line("input", "ps2 keyboard attached");

    if (vfs_init() != 0) {
        boot_halt("storage", "virtual filesystem core failed to initialize");
    }
    if (initramfs_init() != 0) {
        boot_halt("storage", "initramfs image is not valid");
    }
    if (initramfs_populate() != 0) {
        boot_halt("storage", "initramfs could not populate the live filesystem");
    }
    boot_prefix("storage");
    console_write("ramfs online, seeded files=", CONSOLE_STYLE_INFO);
    console_write_dec64(initramfs_file_count(), CONSOLE_STYLE_INFO);
    console_write(" bytes=", CONSOLE_STYLE_MUTED);
    console_write_dec64(initramfs_total_bytes(), CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);

    syscall_init();
    boot_line("syscall", "int 0x80 dispatcher armed");

    boot_line("tty", "starting interactive console");
    if (tty_init() != 0) {
        console_write("\n", CONSOLE_STYLE_INFO);
        boot_halt("tty", "interactive console failed to start");
    }
    irq_controller_unmask(0);
    __asm__ volatile("sti");
    console_write("\n", CONSOLE_STYLE_INFO);

    while (1) {
        tty_poll();
        if (input_has_char()) {
            continue;
        }

        __asm__ volatile("hlt");
    }
}
