#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// each IDT entry is 16 bytes in 64-bit mode
struct idt_entry {
    uint16_t offset_low;    // lower 16 bits of handler address
    uint16_t selector;      // code segment selector
    uint8_t  ist;           // interrupt stack table (0 = none)
    uint8_t  flags;         // type and attributes
    uint16_t offset_mid;    // middle 16 bits of handler address
    uint32_t offset_high;   // upper 32 bits of handler address
    uint32_t zero;          // reserved
} __attribute__((packed));

// IDT pointer loaded via lidt
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// flags
#define IDT_PRESENT     (1 << 7)
#define IDT_RING0       (0 << 5)
#define IDT_RING3       (3 << 5)
#define IDT_INTERRUPT   0x0E    // interrupt gate - disables interrupts on entry
#define IDT_TRAP        0x0F    // trap gate - keeps interrupts enabled

void idt_init();
void idt_set_entry(int index, uint64_t handler, uint8_t flags);

#endif