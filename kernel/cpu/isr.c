#include "isr.h"
#include "irq_controller.h"
#include "../syscall/syscall.h"
#include "../drivers/console/console.h"
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

static void fb_print_hex64(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    buf[18] = '\0';
    for (int i = 17; i >= 2; i--) {
        int n = (int)(v & 0xF);
        buf[i] = (char)(n < 10 ? ('0' + n) : ('a' + n - 10));
        v >>= 4;
    }
    fb_print(buf, FB_WHITE, FB_RED);
}

static void print_exception_frame(struct registers *regs, uint64_t num) {
    console_write("\n*** EXCEPTION: ", CONSOLE_STYLE_ERROR);
    if (num < 32) {
        console_write(exception_names[num], CONSOLE_STYLE_ERROR);
    } else {
        console_write("unknown", CONSOLE_STYLE_ERROR);
    }
    console_write(" ***\n", CONSOLE_STYLE_ERROR);
    console_write("  RIP: ", CONSOLE_STYLE_ERROR);
    console_write_hex64(regs->rip, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  RSP: ", CONSOLE_STYLE_ERROR);
    console_write_hex64(regs->rsp, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  ERR: ", CONSOLE_STYLE_ERROR);
    console_write_hex64(regs->err_code, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  CS: ", CONSOLE_STYLE_ERROR);
    console_write_hex64(regs->cs, CONSOLE_STYLE_ERROR);
    console_write("  RFLAGS: ", CONSOLE_STYLE_ERROR);
    console_write_hex64(regs->rflags, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);

    fb_print("\n*** EXCEPTION: ", FB_WHITE, FB_RED);
    if (num < 32) {
        fb_print(exception_names[num], FB_WHITE, FB_RED);
    } else {
        fb_print("unknown", FB_WHITE, FB_RED);
    }
    fb_print(" ***\n", FB_WHITE, FB_RED);
    fb_print("  RIP: ", FB_WHITE, FB_RED);
    fb_print_hex64(regs->rip);
    fb_print("\n", FB_WHITE, FB_RED);
    fb_print("  RSP: ", FB_WHITE, FB_RED);
    fb_print_hex64(regs->rsp);
    fb_print("\n", FB_WHITE, FB_RED);
    fb_print("  ERR: ", FB_WHITE, FB_RED);
    fb_print_hex64(regs->err_code);
    fb_print("\n", FB_WHITE, FB_RED);
    fb_print("  CS: ", FB_WHITE, FB_RED);
    fb_print_hex64(regs->cs);
    fb_print("  RFLAGS: ", FB_WHITE, FB_RED);
    fb_print_hex64(regs->rflags);
    fb_print("\n", FB_WHITE, FB_RED);
}

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

    print_exception_frame(regs, num);

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
