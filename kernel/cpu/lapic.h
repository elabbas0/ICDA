#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

int lapic_init(uint64_t madt_lapic_phys);






void lapic_disable(void);
void lapic_eoi(void);
void lapic_start_timer(uint8_t vector);
void lapic_stop_timer(void);
uint32_t lapic_id(void);
uint64_t lapic_physical_base(void);
void lapic_enable_ap(void);
void lapic_virtual_wire(void);
void lapic_send_ipi(uint32_t apic_id, uint32_t low);
void lapic_timer_periodic(uint8_t vector, uint32_t count);
uint32_t lapic_calibrate(void (*wait_ticks)(uint64_t));

#endif
