#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>

int exfat_mount_detected(void);
uint32_t exfat_mount_count(void);
int exfat_mount_partition(uint32_t partition_index, const char *mount_path);

#endif
