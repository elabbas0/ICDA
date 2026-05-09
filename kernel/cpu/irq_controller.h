#ifndef IRQ_CONTROLLER_H
#define IRQ_CONTROLLER_H

void irq_controller_init(void);
void irq_controller_eoi(int irq);
void irq_controller_mask(int irq);
void irq_controller_unmask(int irq);
const char *irq_controller_name(void);

#endif
