#include "isr.h"
#include "irq_controller.h"
#include "../syscall/syscall.h"
#include "../drivers/display/framebuffer.h"

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
static isr_handler_t isr_handlers[32] = {0};

void isr_register(int vec, isr_handler_t handler) {
    if (vec >= 0 && vec < 32)
        isr_handlers[vec] = handler;
}

void irq_register(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// called from isr.asm when a cpu exception fires
void isr_handler(struct registers* regs) {
    uint64_t num = regs->int_no;

    // if a C handler is registered for this vector, call it and return
    if (num < 32 && isr_handlers[num]) {
        isr_handlers[num](regs);
        return;
    }

    fb_print("\n*** EXCEPTION: ", FB_WHITE, FB_RED);
    if (num < 32) {
        fb_print(exception_names[num], FB_WHITE, FB_RED);
    }

    fb_print(" ***\n", FB_WHITE, FB_RED);

    __asm__ volatile ("cli; hlt");
}

// called from isr.asm when a hardware irq fires
void irq_handler(struct registers* regs) {
    int irq = (int)regs->int_no - 32;

    // send EOI *before* the handler so context switches inside the handler
    // (e.g. schedule()) don't prevent the PIC from firing further interrupts
    irq_controller_eoi(irq);

    if (irq >= 0 && irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }
}

void syscall_handler(struct registers* regs) {
    regs->rax = syscall_dispatch(regs);
}
