#ifndef KEYBOARD_H
#define KEYBOARD_H

struct registers;

void keyboard_init(void);
void keyboard_irq(struct registers *regs);
void keyboard_pump(void);
int keyboard_has_char(void);
int keyboard_read_char(void);

#endif
