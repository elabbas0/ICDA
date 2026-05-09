#include "irq_controller.h"
#include "pic.h"

typedef struct {
    void (*init)(void);
    void (*eoi)(int irq);
    void (*mask)(int irq);
    void (*unmask)(int irq);
    const char *name;
} irq_controller_ops_t;

static irq_controller_ops_t pic_ops = {
    .init = pic_init,
    .eoi = pic_eoi,
    .mask = pic_mask,
    .unmask = pic_unmask,
    .name = "8259 PIC"
};

static irq_controller_ops_t *active_controller = &pic_ops;

void irq_controller_init(void) {
    active_controller->init();
}

void irq_controller_eoi(int irq) {
    active_controller->eoi(irq);
}

void irq_controller_mask(int irq) {
    active_controller->mask(irq);
}

void irq_controller_unmask(int irq) {
    active_controller->unmask(irq);
}

const char *irq_controller_name(void) {
    return active_controller->name;
}
