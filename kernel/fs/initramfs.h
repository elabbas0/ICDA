#ifndef INITRAMFS_H
#define INITRAMFS_H

#include <stdint.h>

typedef struct {
    const char *path;
    const char *data;
    uint64_t size;
} initramfs_file_t;

int initramfs_init(void);
int initramfs_populate(void);
const initramfs_file_t *initramfs_find(const char *path);
const initramfs_file_t *initramfs_file_at(uint64_t index);
uint64_t initramfs_file_count(void);
uint64_t initramfs_total_bytes(void);

#endif
