#include "ata.h"
#include "block.h"

#include <stdint.h>

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_SECCOUNT0   0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_COMMAND     0x07
#define ATA_REG_STATUS      0x07

#define ATA_REG_CONTROL     0x00
#define ATA_REG_ALTSTATUS   0x00

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

#define ATA_MAX_DEVICES 4

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t drive_select;
    uint32_t sector_count;
    uint8_t present;
    char name[8];
    block_device_t block;
} ata_device_t;

static ata_device_t ata_devices[ATA_MAX_DEVICES];
static uint32_t ata_devices_present = 0;

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

static uint8_t ata_status(const ata_device_t *dev) {
    return inb(dev->ctrl_base + ATA_REG_ALTSTATUS);
}

static void ata_select_drive(const ata_device_t *dev, uint32_t lba) {
    outb(dev->io_base + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | dev->drive_select | ((lba >> 24) & 0x0F)));
    io_wait();
}

static int ata_wait_not_busy(const ata_device_t *dev) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(ata_status(dev) & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(const ata_device_t *dev) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = ata_status(dev);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
    }
    return -1;
}

static int ata_flush_cache_dev(const ata_device_t *dev) {
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(dev);
}

static int ata_identify_device(ata_device_t *dev) {
    uint16_t identify[256];

    outb(dev->ctrl_base + ATA_REG_CONTROL, 0x02);
    ata_select_drive(dev, 0);
    outb(dev->io_base + ATA_REG_SECCOUNT0, 0);
    outb(dev->io_base + ATA_REG_LBA0, 0);
    outb(dev->io_base + ATA_REG_LBA1, 0);
    outb(dev->io_base + ATA_REG_LBA2, 0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(dev->io_base + ATA_REG_STATUS) == 0) {
        return -1;
    }
    if (ata_wait_not_busy(dev) != 0) {
        return -1;
    }
    if (inb(dev->io_base + ATA_REG_LBA1) != 0 || inb(dev->io_base + ATA_REG_LBA2) != 0) {
        return -1;
    }
    if (ata_wait_drq(dev) != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        identify[i] = inw(dev->io_base + ATA_REG_DATA);
    }

    dev->sector_count = ((uint32_t)identify[61] << 16) | identify[60];
    if (dev->sector_count == 0) {
        return -1;
    }
    dev->present = 1;
    return 0;
}

static int ata_device_read(ata_device_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *out = (uint16_t *)buffer;

    if (!dev || !dev->present || !buffer || count == 0) {
        return -1;
    }
    if ((uint64_t)lba + count > dev->sector_count) {
        return -1;
    }
    if (ata_wait_not_busy(dev) != 0) {
        return -1;
    }

    ata_select_drive(dev, lba);
    outb(dev->io_base + ATA_REG_SECCOUNT0, count);
    outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq(dev) != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            out[sector * 256 + i] = inw(dev->io_base + ATA_REG_DATA);
        }
    }
    return 0;
}

static int ata_device_write(ata_device_t *dev, uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *in = (const uint16_t *)buffer;

    if (!dev || !dev->present || !buffer || count == 0) {
        return -1;
    }
    if ((uint64_t)lba + count > dev->sector_count) {
        return -1;
    }
    if (ata_wait_not_busy(dev) != 0) {
        return -1;
    }

    ata_select_drive(dev, lba);
    outb(dev->io_base + ATA_REG_SECCOUNT0, count);
    outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (uint8_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq(dev) != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            outw(dev->io_base + ATA_REG_DATA, in[sector * 256 + i]);
        }
        io_wait();
    }

    return ata_flush_cache_dev(dev);
}

static int ata_block_read(void *context, uint64_t lba, uint32_t count, void *buffer) {
    ata_device_t *dev = (ata_device_t *)context;
    if (count == 0 || count > 255 || lba > 0xFFFFFFFFULL) {
        return -1;
    }
    return ata_device_read(dev, (uint32_t)lba, (uint8_t)count, buffer);
}

static int ata_block_write(void *context, uint64_t lba, uint32_t count, const void *buffer) {
    ata_device_t *dev = (ata_device_t *)context;
    if (count == 0 || count > 255 || lba > 0xFFFFFFFFULL) {
        return -1;
    }
    return ata_device_write(dev, (uint32_t)lba, (uint8_t)count, buffer);
}

static void ata_init_slot(ata_device_t *dev, uint16_t io_base, uint16_t ctrl_base, uint8_t drive_select, uint32_t index) {
    dev->io_base = io_base;
    dev->ctrl_base = ctrl_base;
    dev->drive_select = drive_select;
    dev->sector_count = 0;
    dev->present = 0;
    dev->name[0] = 'a';
    dev->name[1] = 't';
    dev->name[2] = 'a';
    dev->name[3] = (char)('0' + index);
    dev->name[4] = '\0';
    dev->block.name = dev->name;
    dev->block.context = dev;
    dev->block.sector_size = 512;
    dev->block.sector_count = 0;
    dev->block.read = ata_block_read;
    dev->block.write = ata_block_write;
}

int ata_init(void) {
    ata_devices_present = 0;

    ata_init_slot(&ata_devices[0], ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0x00, 0);
    ata_init_slot(&ata_devices[1], ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0x10, 1);
    ata_init_slot(&ata_devices[2], ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0x00, 2);
    ata_init_slot(&ata_devices[3], ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0x10, 3);

    for (uint32_t i = 0; i < ATA_MAX_DEVICES; i++) {
        if (ata_identify_device(&ata_devices[i]) == 0) {
            ata_devices[i].block.sector_count = ata_devices[i].sector_count;
            (void)block_register(&ata_devices[i].block);
            ata_devices_present++;
        }
    }

    return ata_devices_present > 0 ? 0 : -1;
}

int ata_present(void) {
    return ata_devices_present > 0;
}

uint32_t ata_device_count(void) {
    return ata_devices_present;
}

uint32_t ata_sector_count(void) {
    return ata_devices[0].present ? ata_devices[0].sector_count : 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    if (!ata_devices[0].present) {
        return -1;
    }
    return ata_device_read(&ata_devices[0], lba, count, buffer);
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if (!ata_devices[0].present) {
        return -1;
    }
    return ata_device_write(&ata_devices[0], lba, count, buffer);
}

block_device_t *ata_block_device(void) {
    return ata_devices[0].present ? &ata_devices[0].block : 0;
}
