#ifndef PIC_H
#define PIC_H

#include <stdint.h>

// PIC ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

// end of interrupt command
#define PIC_EOI      0x20

void pic_init();
void pic_eoi(int irq);
void pic_mask(int irq);
void pic_unmask(int irq);

#endif