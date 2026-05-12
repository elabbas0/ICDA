#include "ata.h"

#include <stdint.h>

#define ATA_IO_BASE       0x1F0
#define ATA_REG_DATA      0x00
#define ATA_REG_ERROR     0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0      0x03
#define ATA_REG_LBA1      0x04
#define ATA_REG_LBA2      0x05
#define ATA_REG_HDDEVSEL  0x06
#define ATA_REG_COMMAND   0x07
#define ATA_REG_STATUS    0x07

#define ATA_CTRL_BASE     0x3F6
#define ATA_REG_CONTROL   0x00
#define ATA_REG_ALTSTATUS 0x00

#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

static int ata_disk_present = 0;
static uint32_t ata_disk_sectors = 0;

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t ata_status(void) {
    return inb(ATA_CTRL_BASE + ATA_REG_ALTSTATUS);
}

static void ata_select_drive(uint32_t lba) {
    outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    io_wait();
}

static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(ata_status() & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = ata_status();
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
    }
    return -1;
}

static int ata_flush_cache(void) {
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}

int ata_init(void) {
    uint16_t identify[256];

    ata_disk_present = 0;
    ata_disk_sectors = 0;

    outb(ATA_CTRL_BASE + ATA_REG_CONTROL, 0x02);
    ata_select_drive(0);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA1, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA2, 0);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_IO_BASE + ATA_REG_STATUS) == 0) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    if (inb(ATA_IO_BASE + ATA_REG_LBA1) != 0 || inb(ATA_IO_BASE + ATA_REG_LBA2) != 0) {
        return -1;
    }

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_IO_BASE + ATA_REG_DATA);
    }

    ata_disk_sectors = ((uint32_t)identify[61] << 16) | identify[60];
    ata_disk_present = ata_disk_sectors != 0;
    return ata_disk_present ? 0 : -1;
}

int ata_present(void) {
    return ata_disk_present;
}

uint32_t ata_sector_count(void) {
    return ata_disk_sectors;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *out = (uint16_t *)buffer;

    if (!ata_disk_present || !buffer || count == 0) {
        return -1;
    }
    if (lba + count > ata_disk_sectors) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_drive(lba);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, count);
    outb(ATA_IO_BASE + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            out[sector * 256 + i] = inw(ATA_IO_BASE + ATA_REG_DATA);
        }
    }

    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *in = (const uint16_t *)buffer;

    if (!ata_disk_present || !buffer || count == 0) {
        return -1;
    }
    if (lba + count > ata_disk_sectors) {
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_drive(lba);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, count);
    outb(ATA_IO_BASE + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            outw(ATA_IO_BASE + ATA_REG_DATA, in[sector * 256 + i]);
        }
        io_wait();
    }

    return ata_flush_cache();
}
