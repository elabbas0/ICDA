#include <stdint.h>

#include "drivers/console/console.h"
#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"
#include "drivers/input/input.h"
#include "drivers/input/keyboard.h"
#include "drivers/serial/serial.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq_controller.h"
#include "cpu/isr.h"

#include "memory/pf.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#include "proc/sched.h"

static void timer_handler(struct registers *regs) {
    schedule(regs);
}

void kernel_main(void *multiboot_info) {
    serial_init();

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

    pmm_init(multiboot_info);
    if (pmm_free_frames() == 0) {
        console_write_status("PMM", "FAIL", CONSOLE_STYLE_ERROR);
        pmm_print_stats();
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
    console_write_status("PMM", "OK", CONSOLE_STYLE_OK);

    if (vmm_init(fb_phys_addr(), fb_phys_size()) != 0) {
        console_write_status("VMM", "FAIL", CONSOLE_STYLE_ERROR);
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
    console_write_status("VMM", "OK", CONSOLE_STYLE_OK);

    if (irq_controller_init(multiboot_info) != 0) {
        console_write_status("IRQCTL", "FAIL", CONSOLE_STYLE_ERROR);
        while (1) {
            __asm__ volatile("cli; hlt");
        }
    }
    console_write("IRQCTL: ", CONSOLE_STYLE_INFO);
    console_write(irq_controller_name(), CONSOLE_STYLE_OK);
    console_write("\n", CONSOLE_STYLE_INFO);

    pf_init();
    console_write_status("PF", "OK", CONSOLE_STYLE_OK);

    sched_init();
    console_write_status("SCHED", "OK", CONSOLE_STYLE_OK);

    irq_register(0, timer_handler);
    console_write_status("TIMER", "READY", CONSOLE_STYLE_OK);

    keyboard_init();
    console_write_status("KEYBOARD", "OK", CONSOLE_STYLE_OK);

    __asm__ volatile("sti");
    console_write_status("INTERRUPTS", "OK", CONSOLE_STYLE_OK);
    console_write("\n", CONSOLE_STYLE_INFO);
    console_write("Keyboard echo ready. Type below:\n\n", CONSOLE_STYLE_INFO);
    irq_controller_unmask(0);

    while (1) {
        int c = input_read_char();
        if (c >= 0) {
            if (c == '\b') {
                console_backspace();
            } else {
                console_write_char((char)c, CONSOLE_STYLE_INFO);
            }
            continue;
        }

        __asm__ volatile("hlt");
    }
}
