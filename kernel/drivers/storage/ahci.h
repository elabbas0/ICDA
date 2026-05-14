#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include "block.h"

int ahci_init(void);
uint32_t ahci_device_count(void);
block_device_t *ahci_block_device(uint32_t index);

#endif
