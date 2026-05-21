#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

int fat32_mount_detected(void);
uint32_t fat32_mount_count(void);
int fat32_mount_partition(uint32_t partition_index, const char *mount_path);

#endif
