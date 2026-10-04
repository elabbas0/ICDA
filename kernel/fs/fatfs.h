#ifndef FATFS_H
#define FATFS_H

#include <stdint.h>
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"

#define FATFS_NAME_MAX 256
#define FATFS_ATTR_RO  0x01
#define FATFS_ATTR_DIR 0x10

typedef struct {
    block_device_t *dev;
    uint64_t base_lba;
    uint64_t sectors;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t reserved;
    uint32_t fat_count;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t cluster_count;
    uint32_t fsinfo_sector;
    uint32_t next_free;
    int64_t  cache_sector;
    int      cache_dirty;
    int      touched;
    uint8_t  cache[512];
} fatfs_t;

typedef struct {
    char     name[FATFS_NAME_MAX];
    uint8_t  attr;
    uint32_t cluster;
    uint32_t size;
    uint32_t dir_cluster;
    uint32_t first_slot;
    uint32_t short_slot;
    uint8_t  raw[11];
} fatfs_entry_t;

typedef int (*fatfs_iter_fn)(const fatfs_entry_t *entry, void *ctx);

int fatfs_mount(fatfs_t *vol, block_device_t *dev, uint64_t start_lba, uint64_t sector_count);
int fatfs_mount_part(fatfs_t *vol, const partition_info_t *part);
int fatfs_iterate(fatfs_t *vol, uint32_t dir_cluster, fatfs_iter_fn fn, void *ctx);
int fatfs_lookup(fatfs_t *vol, const char *path, fatfs_entry_t *out);
int fatfs_mkdir(fatfs_t *vol, const char *path);
int fatfs_write(fatfs_t *vol, const char *path, const void *data, uint64_t size);
int fatfs_read(fatfs_t *vol, const char *path, char **data_out, uint64_t *size_out);
int fatfs_read_entry(fatfs_t *vol, const fatfs_entry_t *entry, char **data_out);
int fatfs_remove(fatfs_t *vol, const char *path);
int fatfs_usage(fatfs_t *vol, uint32_t *free_clusters, uint32_t *highest_used);

typedef struct {
    uint32_t first;
    uint32_t cluster;
    uint64_t off;
} fatfs_hint_t;

int64_t fatfs_read_range(fatfs_t *vol, const fatfs_entry_t *entry, uint64_t off, void *buf, uint64_t len,
                         fatfs_hint_t *hint);
int fatfs_flush(fatfs_t *vol);
int fatfs_write_at(fatfs_t *vol, const char *path, uint64_t off, const void *data, uint64_t len);
int fatfs_truncate(fatfs_t *vol, const char *path, uint64_t len);

#endif
