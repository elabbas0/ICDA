#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

int lapic_init(uint64_t madt_lapic_phys);
void lapic_eoi(void);
void lapic_start_timer(uint8_t vector);
void lapic_stop_timer(void);
uint32_t lapic_id(void);
uint64_t lapic_physical_base(void);

#endif
