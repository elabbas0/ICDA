#include "isr.h"
#include "idt.h"
#include "irq_controller.h"
#include "lapic.h"
#include "../diag/bootstage.h"
#include "../syscall/syscall.h"
#include "../drivers/audio/speaker.h"
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
static irq_handler_t msi_handlers[MSI_VEC_COUNT] = {0};

static void *msi_stub_for(int vector) {
    switch (vector) {
    case 64: return msi64;
    case 65: return msi65;
    case 66: return msi66;
    case 67: return msi67;
    case 68: return msi68;
    case 69: return msi69;
    case 70: return msi70;
    case 71: return msi71;
    case 72: return msi72;
    case 73: return msi73;
    case 74: return msi74;
    case 75: return msi75;
    case 76: return msi76;
    case 77: return msi77;
    case 78: return msi78;
    case 79: return msi79;
    default: return 0;
    }
}

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
    console_write("  BOOT STAGE: S", CONSOLE_STYLE_ERROR);
    console_write_dec64((uint64_t)bootstage_current(), CONSOLE_STYLE_ERROR);
    console_write(" ", CONSOLE_STYLE_ERROR);
    console_write(bootstage_label(), CONSOLE_STYLE_ERROR);
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
    fb_print("  BOOT STAGE: S", FB_WHITE, FB_RED);
    fb_print_hex64((uint64_t)bootstage_current());
    fb_print(" ", FB_WHITE, FB_RED);
    fb_print(bootstage_label(), FB_WHITE, FB_RED);
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

int irq_register_msi(int vector, irq_handler_t handler) {
    void *stub;
    if (vector < MSI_VEC_BASE || vector > MSI_VEC_LAST) {
        return -1;
    }
    if (!handler) {
        return -1;
    }
    /* MSI needs a live local APIC for the message address and EOI.
     * Lazily bring it up; legacy PIC routing is left untouched. */
    if (irq_controller_force_apic() != 0) {
        return -1;
    }
    stub = msi_stub_for(vector);
    if (!stub) {
        return -1;
    }
    msi_handlers[vector - MSI_VEC_BASE] = handler;
    idt_set_entry_ist(vector, (uint64_t)stub,
                       (uint8_t)(IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT), 0);
    return 0;
}

// called from isr.asm when a cpu exception fires
void isr_handler(struct registers* regs) {
    uint64_t num = regs->int_no;

    // if a C handler is registered for this vector, call it and return
    if (num < 32 && isr_handlers[num]) {
        isr_handlers[num](regs);
        return;
    }

    speaker_stop();
    print_exception_frame(regs, num);

    __asm__ volatile ("cli; hlt");
}

// called from isr.asm when a hardware irq fires
void irq_handler(struct registers* regs) {
    int irq = (int)regs->int_no - 32;

    if (regs->int_no >= (uint64_t)MSI_VEC_BASE &&
        regs->int_no <= (uint64_t)MSI_VEC_LAST) {
        int idx = (int)regs->int_no - MSI_VEC_BASE;
        if (idx >= 0 && idx < MSI_VEC_COUNT && msi_handlers[idx]) {
            msi_handlers[idx](regs);
        }
        /* MSI is edge-triggered via the local APIC; EOI directly so the
         * legacy PIC path (still active for IRQ0-15) is untouched. */
        lapic_eoi();
        return;
    }

    if (irq >= 0 && irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }

    irq_controller_eoi(irq);
}

void syscall_handler(struct registers* regs) {
    regs->rax = syscall_dispatch(regs);
}
