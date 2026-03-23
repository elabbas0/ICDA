#include "isr.h"
#include "pic.h"
#include "drivers/display/framebuffer.h"

const char *exception_names[32] = {
    "division by zero",       "debug",
    "non-maskable interrupt", "breakpoint",
    "overflow",               "bound range exceeded",
    "invalid opcode",         "device not available",
    "double fault",           "coprocessor segment overrun",
    "invalid TSS",            "segment not present",
    "stack-segment fault",    "general protection fault",
    "page fault",             "reserved",
    "x87 floating point",     "alignment check",
    "machine check",          "SIMD floating point",
    "virtualization",         "reserved",
    "reserved",               "reserved",
    "reserved",               "reserved",
    "reserved",               "reserved",
    "reserved",               "reserved",
    "security exception",     "reserved"
};
static irq_handler_t irq_handlers[16] = {0};

void irq_register(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// called from isr.asm when a cpu exception fires
void isr_handler(struct registers* regs) {

    fb_print("\n*** EXCEPTION: ", FB_WHITE, FB_RED);

    uint64_t num = regs->int_no;
    if (num < 32) {
        fb_print(exception_names[num], FB_WHITE, FB_RED);
    }

    fb_print(" ***\n", FB_WHITE, FB_RED);

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