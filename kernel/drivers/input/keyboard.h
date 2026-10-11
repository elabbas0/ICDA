#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

struct registers;

void keyboard_init(void);
void keyboard_irq(struct registers *regs);
void keyboard_pump(void);
void keyboard_inject_scancode(uint8_t scancode);   /* a USB keyboard, set-1 code */
int keyboard_has_char(void);
int keyboard_read_char(void);

#endif
