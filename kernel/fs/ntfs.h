#ifndef NTFS_H
#define NTFS_H

#include <stdint.h>

int ntfs_mount_detected(void);
uint32_t ntfs_mount_count(void);
int ntfs_mount_partition(uint32_t partition_index, const char *mount_path);

#endif
