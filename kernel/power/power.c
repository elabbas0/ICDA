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
    
    outb(0x80, 0x00);
}

void power_reboot(void) {
    uint8_t status;
    unsigned int spins;

    console_write("rebooting...\n", CONSOLE_STYLE_WARN);
    

    spins = 0;
    do {
        status = inb(0x64);
        spins++;
    } while ((status & 0x02U) && spins < 100000U);
    outb(0x64, 0xFE);
    for (spins = 0; spins < 1000000U; spins++) {
        io_delay();
        
        if ((spins & 0xFFFFFU) == 0U) {
            break;
        }
    }

    

    outb(0xCF9, 0x06);
    io_delay();
    io_delay();
    outb(0xCF9, 0x0E);
    for (spins = 0; spins < 1000000U; spins++) {
        io_delay();
    }

    
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
        

        if (fadt->length > (uint32_t)(FADT_PM1A_CNT_BLK_OFF + 2)) {
            pm1a_cnt = (uint16_t)(raw[FADT_PM1A_CNT_BLK_OFF] |
                                  ((uint16_t)raw[FADT_PM1A_CNT_BLK_OFF + 1] << 8));
        }
        if (fadt->length > (uint32_t)(FADT_PM1B_CNT_BLK_OFF + 2)) {
            pm1b_cnt = (uint16_t)(raw[FADT_PM1B_CNT_BLK_OFF] |
                                  ((uint16_t)raw[FADT_PM1B_CNT_BLK_OFF + 1] << 8));
        }
    }

    



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
        
        for (i = 0; i < 1000000U; i++) {
            io_delay();
        }
        
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

    

    outw(0x604, 0x2000);
    for (i = 0; i < 100000U; i++) {
        io_delay();
    }
    outw(0xB004, 0x2000);
    for (i = 0; i < 100000U; i++) {
        io_delay();
    }

    


    outb(0x501, 0x31);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
