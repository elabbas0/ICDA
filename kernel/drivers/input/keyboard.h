#ifndef KEYBOARD_H
#define KEYBOARD_H

struct registers;

void keyboard_init(void);
void keyboard_irq(struct registers *regs);
void keyboard_pump(void);
int keyboard_has_char(void);
int keyboard_read_char(void);

/* USB HID bridge (Phase 1c): push decoded USB keys into the SAME PS/2
 * queue so tty/vt/WM see USB keys identically. Safe from IRQ or poll
 * context; drops when full, wakes input waiters on push. */
void keyboard_usb_push(char c);
void keyboard_usb_push_seq(const char *seq);

#endif
