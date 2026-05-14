#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "block.h"

int nvme_init(void);
uint32_t nvme_device_count(void);
block_device_t *nvme_block_device(uint32_t index);

#endif
