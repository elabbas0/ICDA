#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(void *context, uint64_t lba, uint32_t count, void *buffer);
typedef int (*block_write_fn)(void *context, uint64_t lba, uint32_t count, const void *buffer);

struct block_device {
    const char *name;
    void *context;
    uint32_t sector_size;
    uint64_t sector_count;
    block_read_fn read;
    block_write_fn write;
};

int block_register(block_device_t *device);
uint32_t block_count(void);
block_device_t *block_get(uint32_t index);
block_device_t *block_find(const char *name);

#endif
