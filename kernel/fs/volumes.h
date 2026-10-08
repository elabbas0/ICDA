#ifndef VOLUMES_H
#define VOLUMES_H

#include <stdint.h>
#include "../drivers/storage/partition.h"

#define VOLUME_FAT32 1
#define VOLUME_EXFAT 2
#define VOLUME_NTFS  3

int      volumes_mount_all(void);
uint32_t volumes_count(int fs);
int      volumes_mount_partition(uint32_t partition_index, const char *mount_path);
/* 1 if an absolute path is inside the volume mounted from partition p */
int      volumes_path_is_on(const char *path, const partition_info_t *p);
int      volumes_is_writable(uint32_t index, int fs);

#endif
