#include "block.h"

#define BLOCK_MAX_DEVICES 8

static block_device_t *block_devices[BLOCK_MAX_DEVICES];
static uint32_t block_device_count = 0;

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

int block_register(block_device_t *device) {
    for (uint32_t i = 0; i < block_device_count; i++) {
        if (block_devices[i] == device) {
            return 0;
        }
    }
    if (!device || !device->read || block_device_count >= BLOCK_MAX_DEVICES) {
        return -1;
    }
    block_devices[block_device_count++] = device;
    return 0;
}

uint32_t block_count(void) {
    return block_device_count;
}

block_device_t *block_get(uint32_t index) {
    if (index >= block_device_count) {
        return 0;
    }
    return block_devices[index];
}

block_device_t *block_find(const char *name) {
    for (uint32_t i = 0; i < block_device_count; i++) {
        if (str_eq(block_devices[i]->name, name)) {
            return block_devices[i];
        }
    }
    return 0;
}
