#ifndef POWER_H
#define POWER_H

/* Reboot the machine (8042 keyboard-controller reset). Never returns. */
void power_reboot(void);

/* Power off via ACPI S5 when a FADT is available; falls back to the
 * QEMU isa-debug-exit port (0x501) and finally a halt. Never returns. */
void power_shutdown(void);

#endif
