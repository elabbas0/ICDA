#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

int lapic_init(uint64_t madt_lapic_phys);
/* Turn the local APIC off entirely.  The legacy 8259 PIC path needs this:
 * UEFI firmware leaves the LAPIC enabled, and an enabled-but-unprogrammed
 * LAPIC masks LINT0 (the pin the PIC output feeds), so no PIC interrupt
 * (PIT timer IRQ0, keyboard IRQ1, PS/2 mouse IRQ12) ever reaches the CPU.
 * Clearing IA32_APIC_BASE.EN routes external interrupts through the legacy
 * INTR pin again. */
void lapic_disable(void);
void lapic_eoi(void);
void lapic_start_timer(uint8_t vector);
void lapic_stop_timer(void);
uint32_t lapic_id(void);
uint64_t lapic_physical_base(void);

#endif
