#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

struct registers;

typedef struct {
    int32_t abs_x;
    int32_t abs_y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;   /* bit 0=left, bit 1=right, bit 2=middle */
} mouse_event_t;

void    mouse_init(void);
void    mouse_set_screen(int w, int h);
void    mouse_irq(struct registers *regs);
int     mouse_read_event(mouse_event_t *out);   /* 0=got event, -1=empty */
int32_t mouse_abs_x(void);
int32_t mouse_abs_y(void);
uint8_t mouse_buttons(void);

#endif
