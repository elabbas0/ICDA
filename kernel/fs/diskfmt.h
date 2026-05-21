#ifndef DISKFMT_H
#define DISKFMT_H

#include <stdint.h>
#include "../drivers/storage/partition.h"

typedef enum {
    DISKFMT_FS_FAT32 = 1,
    DISKFMT_FS_EXFAT = 2,
    DISKFMT_LAYOUT_CLEAR = 3,
    DISKFMT_LAYOUT_ICDA = 4,
    DISKFMT_LAYOUT_MBR = 5,
    DISKFMT_LAYOUT_GPT = 6
} diskfmt_fs_t;

int diskfmt_format_device(uint32_t device_index, diskfmt_fs_t fs_type);
int diskfmt_format_partition(uint32_t partition_index, diskfmt_fs_t fs_type);
int diskfmt_set_partition_role(uint32_t partition_index, partition_role_t role);

#endif
