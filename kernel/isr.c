#include "isr.h"
#include "pic.h"
#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"

// table of registered irq handlers
static irq_handler_t irq_handlers[16] = {0};

void irq_register(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// called from isr.asm when a cpu exception fires
void isr_handler(struct registers* regs) {
    fb_print("\n--- KERNEL EXCEPTION ---\n", 0x00FF0000, 0x00000000);
    fb_print("exception: ", 0x00FFFFFF, 0x00000000);

    if (regs->int_no < 32) {
        fb_print(exception_names[regs->int_no], 0x00FF0000, 0x00000000);
    }

    fb_print("\ninterrupt: ", 0x00FFFFFF, 0x00000000);
    fb_print_int((int)regs->int_no, 0x00FFFF00, 0x00000000);

    fb_print("\nerror code: ", 0x00FFFFFF, 0x00000000);
    fb_print_int((int)regs->err_code, 0x00FFFF00, 0x00000000);

    fb_print("\nrip: ", 0x00FFFFFF, 0x00000000);
    fb_print_hex((unsigned int)regs->rip, 0x00FFFF00, 0x00000000);

    fb_print("\n--- system halted ---\n", 0x00FF0000, 0x00000000);

    // halt forever
    __asm__ volatile ("cli; hlt");
}

// called from isr.asm when a hardware irq fires
void irq_handler(struct registers* regs) {
    int irq = (int)regs->int_no - 32;

    // call registered handler if one exists
    if (irq >= 0 && irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }

    // send end-of-interrupt to PIC
    pic_eoi(irq);
}