#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

#include "../firmware/acpi.h"

int ioapic_init(const struct acpi_madt *madt);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id, int masked);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id);

#endif
