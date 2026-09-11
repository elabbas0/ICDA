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
 * PM1b_CNT_BLK at byte 68 (ACPI 1.0+ layout: 36-byte header, then
 * FirmwareCtrl/Dsdt, ..., PM1a_EVT_BLK@56, PM1b_EVT_BLK@60,
 * PM1a_CNT_BLK@64, PM1b_CNT_BLK@68).
 * SLP_TYPa/b for S5 come from the DSDT _S5 package (AML). Parsing AML
 * is out of scope here; QEMU and the vast majority of firmwares use
 * SLP_TYP=5 for S5, so we keep 5 with this comment (matches the old
 * behavior, now with the mandatory SLP_EN bit). */
#define FADT_PM1A_CNT_BLK_OFF 64
#define FADT_PM1B_CNT_BLK_OFF 68
#define ACPI_PM_SCI_EN 0x0001U
#define ACPI_PM_SLP_EN (1U << 13)
#define ACPI_S5_SLP_TYP 5

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_delay(void) {
    /* Port 0x80 is the conventional CMOS-delay sink: harmless write. */
    outb(0x80, 0x00);
}

void power_reboot(void) {
    uint8_t status;
    unsigned int spins;

    console_write("rebooting...\n", CONSOLE_STYLE_WARN);
    /* 1. 8042 keyboard-controller reset pulse (port 0x64, cmd 0xFE).
     * Bound the busy-wait so a missing controller cannot hang us. */
    spins = 0;
    do {
        status = inb(0x64);
        spins++;
    } while ((status & 0x02U) && spins < 100000U);
    outb(0x64, 0xFE);
    for (spins = 0; spins < 1000000U; spins++) {
        io_delay();
        /* Give the controller a chance; if we are still alive, move on. */
        if ((spins & 0xFFFFFU) == 0U) {
            break;
        }
    }

    /* 2. Reset Control Register (port 0xCF9): 0x06 = reset+init,
     * then 0x0E = reset+full. Small delays between writes. */
    outb(0xCF9, 0x06);
    io_delay();
    io_delay();
    outb(0xCF9, 0x0E);
    for (spins = 0; spins < 1000000U; spins++) {
        io_delay();
    }

    /* 3. Triple fault as a last resort: load an empty IDT and fault. */
    {
        struct __attribute__((packed)) {
            uint16_t limit;
            uint64_t base;
        } empty_idt = { 0, 0 };
        __asm__ volatile("lidt %0" :: "m"(empty_idt) : "memory");
        __asm__ volatile("int3");
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void power_shutdown(void) {
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    uint16_t pm1a_cnt = 0;
    uint16_t pm1b_cnt = 0;
    uint32_t i;

    console_write("shutting down...\n", CONSOLE_STYLE_WARN);

    if (fadt) {
        const uint8_t *raw = (const uint8_t *)fadt;
        /* Bounds-checked FADT reads: both CNT blocks are 4-byte
         * little-endian port numbers. */
        if (fadt->length > (uint32_t)(FADT_PM1A_CNT_BLK_OFF + 2)) {
            pm1a_cnt = (uint16_t)(raw[FADT_PM1A_CNT_BLK_OFF] |
                                  ((uint16_t)raw[FADT_PM1A_CNT_BLK_OFF + 1] << 8));
        }
        if (fadt->length > (uint32_t)(FADT_PM1B_CNT_BLK_OFF + 2)) {
            pm1b_cnt = (uint16_t)(raw[FADT_PM1B_CNT_BLK_OFF] |
                                  ((uint16_t)raw[FADT_PM1B_CNT_BLK_OFF + 1] << 8));
        }
    }

    /* 1. ACPI S5 first (correct on real HW): SLP_TYP=5 in bits 12:10
     * plus the mandatory SLP_EN (bit 13). Preserve the SCI_EN bit
     * from the current register contents like firmware expects.
     * Write PM1b too when present. */
    if (pm1a_cnt) {
        uint16_t cur_a = inw(pm1a_cnt);
        uint16_t val_a =
            (uint16_t)(((uint16_t)ACPI_S5_SLP_TYP << 10) | ACPI_PM_SLP_EN |
                       (cur_a & ACPI_PM_SCI_EN));
        outw(pm1a_cnt, val_a);
        if (pm1b_cnt) {
            uint16_t cur_b = inw(pm1b_cnt);
            uint16_t val_b =
                (uint16_t)(((uint16_t)ACPI_S5_SLP_TYP << 10) | ACPI_PM_SLP_EN |
                           (cur_b & ACPI_PM_SCI_EN));
            outw(pm1b_cnt, val_b);
        }
        /* Give the power state a moment to take effect. */
        for (i = 0; i < 1000000U; i++) {
            io_delay();
        }
        /* Retry once in case the first write raced firmware. */
        outw(pm1a_cnt, val_a);
        if (pm1b_cnt) {
            uint16_t cur_b = inw(pm1b_cnt);
            uint16_t val_b =
                (uint16_t)(((uint16_t)ACPI_S5_SLP_TYP << 10) | ACPI_PM_SLP_EN |
                           (cur_b & ACPI_PM_SCI_EN));
            outw(pm1b_cnt, val_b);
        }
        for (i = 0; i < 1000000U; i++) {
            io_delay();
        }
    }

    /* 2. QEMU fallbacks (harmless on real HW: unassigned ports).
     * 0x604/0xB004 with 0x2000 is the QEMU poweroff sequence. */
    outw(0x604, 0x2000);
    for (i = 0; i < 100000U; i++) {
        io_delay();
    }
    outw(0xB004, 0x2000);
    for (i = 0; i < 100000U; i++) {
        io_delay();
    }

    /* 3. QEMU isa-debug-exit: writing 0x31 to port 0x501 powers off the
     * VM when -device isa-debug-exit is present. Harmless on real
     * hardware (port usually unassigned). */
    outb(0x501, 0x31);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
