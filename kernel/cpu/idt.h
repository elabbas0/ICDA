#ifndef IDT_H
#define IDT_H

#include <stdint.h>


struct idt_entry {
    uint16_t offset_low;    
    uint16_t selector;      
    uint8_t  ist;           
    uint8_t  flags;         
    uint16_t offset_mid;    
    uint32_t offset_high;   
    uint32_t zero;          
} __attribute__((packed));


struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));


#define IDT_PRESENT     (1 << 7)
#define IDT_RING0       (0 << 5)
#define IDT_RING3       (3 << 5)
#define IDT_INTERRUPT   0x0E    
#define IDT_TRAP        0x0F    

void idt_init();
void idt_set_entry(int index, uint64_t handler, uint8_t flags);

#endif