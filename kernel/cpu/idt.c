#include "idt.h"
#include "isr.h"

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   ip;

void idt_set_entry(int index, uint64_t handler, uint8_t flags) {
    idt[index].offset_low  = handler & 0xFFFF;
    idt[index].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].selector    = 0x08;      // kernel code segment
    idt[index].ist         = 0;
    idt[index].flags       = flags;
    idt[index].zero        = 0;
}

extern void idt_flush(uint64_t idt_ptr);

void idt_init() {
    ip.limit = sizeof(idt) - 1;
    ip.base  = (uint64_t)&idt;

    // cpu exceptions 0-31
    idt_set_entry(0,  (uint64_t)isr0,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(1,  (uint64_t)isr1,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(2,  (uint64_t)isr2,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(3,  (uint64_t)isr3,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(4,  (uint64_t)isr4,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(5,  (uint64_t)isr5,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(6,  (uint64_t)isr6,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(7,  (uint64_t)isr7,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(8,  (uint64_t)isr8,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(9,  (uint64_t)isr9,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(10, (uint64_t)isr10, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(11, (uint64_t)isr11, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(12, (uint64_t)isr12, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(13, (uint64_t)isr13, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(14, (uint64_t)isr14, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(15, (uint64_t)isr15, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(16, (uint64_t)isr16, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(17, (uint64_t)isr17, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(18, (uint64_t)isr18, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(19, (uint64_t)isr19, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(20, (uint64_t)isr20, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(21, (uint64_t)isr21, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(22, (uint64_t)isr22, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(23, (uint64_t)isr23, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(24, (uint64_t)isr24, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(25, (uint64_t)isr25, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(26, (uint64_t)isr26, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(27, (uint64_t)isr27, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(28, (uint64_t)isr28, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(29, (uint64_t)isr29, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(30, (uint64_t)isr30, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(31, (uint64_t)isr31, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);

    // hardware irqs 32-47
    idt_set_entry(32, (uint64_t)irq0,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(33, (uint64_t)irq1,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(34, (uint64_t)irq2,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(35, (uint64_t)irq3,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(36, (uint64_t)irq4,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(37, (uint64_t)irq5,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(38, (uint64_t)irq6,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(39, (uint64_t)irq7,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(40, (uint64_t)irq8,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(41, (uint64_t)irq9,  IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(42, (uint64_t)irq10, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(43, (uint64_t)irq11, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(44, (uint64_t)irq12, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(45, (uint64_t)irq13, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(46, (uint64_t)irq14, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);
    idt_set_entry(47, (uint64_t)irq15, IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT);

    idt_flush((uint64_t)&ip);
}