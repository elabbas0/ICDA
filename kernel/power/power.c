#include "power.h"
#include "../firmware/acpi.h"
#include "../memory/vmm.h"
#include "../drivers/console/console.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* FADT (FACP) field offsets used for S5: PM1a_CNT_BLK is at byte 64,
 * SLP_TYPa/b for S5 come from the DSDT _S5 package.  QEMU and most
 * firmwares use SLP_TYP = 5 for S5; we write SLP_TYP << 10 | SCI_EN. */
#define FADT_PM1A_CNT_BLK_OFF 64
#define ACPI_PM_SCI_EN  0x0001
#define ACPI_S5_SLP_TYP 5

void power_reboot(void) {
    uint8_t status;

    console_write("rebooting...\n", CONSOLE_STYLE_WARN);
    /* Wait for the keyboard controller to be ready, then pulse reset. */
    do {
        status = inb(0x64);
    } while (status & 0x02);
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void power_shutdown(void) {
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    uint16_t pm1a_cnt = 0;

    console_write("shutting down...\n", CONSOLE_STYLE_WARN);

    if (fadt && fadt->length > FADT_PM1A_CNT_BLK_OFF + 2) {
        const uint8_t *raw = (const uint8_t *)fadt;
        pm1a_cnt = (uint16_t)(raw[FADT_PM1A_CNT_BLK_OFF] |
                              ((uint16_t)raw[FADT_PM1A_CNT_BLK_OFF + 1] << 8));
    }

    if (pm1a_cnt) {
        /* S5: SLP_TYP=5 in bits 12-14, SCI_EN set.  QEMU and real
         * firmwares power down the machine on this write. */
        outw(pm1a_cnt, (uint16_t)((ACPI_S5_SLP_TYP << 10) | ACPI_PM_SCI_EN));
        /* Give the power state a moment to take effect. */
        for (volatile int i = 0; i < 1000000; i++) {
        }
    }

    /* QEMU isa-debug-exit: writing 0x31 to port 0x501 powers off the VM.
     * Harmless on real hardware (port usually unassigned). */
    outb(0x501, 0x31);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
