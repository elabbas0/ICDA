#ifndef IRQ_CONTROLLER_H
#define IRQ_CONTROLLER_H

int irq_controller_init(void *multiboot_info);
void irq_controller_eoi(int irq);
void irq_controller_mask(int irq);
void irq_controller_unmask(int irq);
const char *irq_controller_name(void);

#endif
