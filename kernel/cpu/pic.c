#include "pic.h"

// write a byte to an I/O port
static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void pic_remap(uint8_t master_offset, uint8_t slave_offset) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, master_offset);
    io_wait();
    outb(PIC2_DATA, slave_offset);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

int pic_init(void) {
    pic_remap(0x20, 0x28);
    /* Mask everything except the slave cascade line: the slave PIC is
     * wired into master IRQ2, so if that line is masked no IRQ from
     * the slave (8-15, e.g. PS/2 mouse IRQ12) can ever reach the CPU. */
    outb(PIC1_DATA, 0xFF & ~(1U << 2));
    outb(PIC2_DATA, 0xFF);
    return 0;
}

void pic_eoi(int irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask_irq(int irq) {
    uint16_t port;
    uint8_t bit;
    uint8_t mask;

    if (irq < 0 || irq > 15) {
        return;
    }

    if (irq < 8) {
        port = PIC1_DATA;
        bit = (uint8_t)irq;
    } else {
        port = PIC2_DATA;
        bit = (uint8_t)(irq - 8);
    }

    mask = inb(port);
    mask |= (uint8_t)(1U << bit);
    outb(port, mask);
}

void pic_unmask_irq(int irq) {
    uint16_t port;
    uint8_t bit;
    uint8_t mask;

    if (irq < 0 || irq > 15) {
        return;
    }

    if (irq < 8) {
        port = PIC1_DATA;
        bit = (uint8_t)irq;
    } else {
        port = PIC2_DATA;
        bit = (uint8_t)(irq - 8);
    }

    mask = inb(port);
    mask &= (uint8_t)~(1U << bit);
    outb(port, mask);
}

void pic_disable(void) {
    pic_remap(0x20, 0x28);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
