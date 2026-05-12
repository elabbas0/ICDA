#ifndef ATA_H
#define ATA_H

#include <stdint.h>

int ata_init(void);
int ata_present(void);
uint32_t ata_sector_count(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif
