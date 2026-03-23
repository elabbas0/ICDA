#include "pic.h"

// write a byte to an I/O port
static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// read a byte from an I/O port
static uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// small delay for old hardware
static void io_wait() {
    outb(0x80, 0);
}

void pic_init() {
    // save current masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // start initialization sequence
    outb(PIC1_COMMAND, 0x11); io_wait();
    outb(PIC2_COMMAND, 0x11); io_wait();

    // remap irqs: master starts at 32, slave at 40
    outb(PIC1_DATA, 0x20); io_wait();   // master offset = 32
    outb(PIC2_DATA, 0x28); io_wait();   // slave offset  = 40

    // tell master there's a slave at IRQ2
    outb(PIC1_DATA, 0x04); io_wait();
    // tell slave its cascade identity
    outb(PIC2_DATA, 0x02); io_wait();

    // 8086 mode
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    // restore saved masks
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_eoi(int irq) {
    // if irq came from slave PIC, notify slave too
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    uint8_t mask = inb(port) | (1 << irq);
    outb(port, mask);
}

void pic_unmask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    uint8_t mask = inb(port) & ~(1 << irq);
    outb(port, mask);
}