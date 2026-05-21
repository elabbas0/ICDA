#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include "block.h"

typedef enum {
    PARTITION_KIND_UNKNOWN = 0,
    PARTITION_KIND_MBR,
    PARTITION_KIND_GPT
} partition_kind_t;

typedef enum {
    PARTITION_FS_UNKNOWN = 0,
    PARTITION_FS_FAT32,
    PARTITION_FS_EXFAT,
    PARTITION_FS_NTFS
} partition_fs_hint_t;

typedef enum {
    PARTITION_ROLE_UNKNOWN = 0,
    PARTITION_ROLE_EFI,
    PARTITION_ROLE_SYSTEM,
    PARTITION_ROLE_SWAP
} partition_role_t;

typedef struct {
    block_device_t *device;
    uint64_t start_lba;
    uint64_t sector_count;
    partition_kind_t kind;
    partition_role_t role;
    partition_fs_hint_t fs_hint;
    uint8_t mbr_type;
    uint64_t gpt_entries_lba;
    uint32_t gpt_entry_size;
    uint32_t gpt_entry_index;
    uint8_t gpt_type_guid[16];
    char name[48];
} partition_info_t;

int partition_scan_all(void);
uint32_t partition_count(void);
const partition_info_t *partition_get(uint32_t index);
const char *partition_fs_name(partition_fs_hint_t hint);
partition_kind_t partition_device_kind(uint32_t device_index);
const char *partition_kind_name(partition_kind_t kind);
const char *partition_role_name(partition_role_t role);

#endif
