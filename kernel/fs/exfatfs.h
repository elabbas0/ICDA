#ifndef EXFATFS_H
#define EXFATFS_H

#include <stdint.h>
#include "../drivers/storage/block.h"

#define EXFATFS_NAME_MAX 256
#define EXFATFS_ATTR_RO  0x01
#define EXFATFS_ATTR_DIR 0x10

typedef struct {
    block_device_t *dev;
    uint64_t base_lba;
    uint64_t sectors;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t bitmap_cluster;
    uint64_t bitmap_bytes;
    uint32_t upcase_cluster;
    uint64_t upcase_bytes;
    int      writable;
    uint32_t next_free;
    const uint16_t *upcase;
    int64_t  fat_cache_sector;
    int      fat_dirty;
    uint8_t  fat_cache[512];
    int64_t  bmp_cache_sector;
    int      bmp_dirty;
    uint8_t  bmp_cache[512];
} exfat_t;

typedef struct {
    uint32_t cluster;
    int      nofat;
    uint64_t size;
    int      is_root;
    uint32_t parent_cluster;
    int      parent_nofat;
    uint64_t parent_size;
    int      parent_is_root;
    uint32_t set_slot;
} exfat_dir_t;

typedef struct {
    char        name[EXFATFS_NAME_MAX];
    uint16_t    attr;
    uint32_t    cluster;
    int         nofat;
    uint64_t    size;
    uint64_t    valid_size;
    uint32_t    set_slot;
    uint32_t    set_count;
    exfat_dir_t dir;
} exfat_entry_t;

typedef struct {
    uint32_t first;
    uint32_t cluster;
    uint64_t off;
} exfat_hint_t;

typedef int (*exfat_iter_fn)(const exfat_entry_t *entry, void *ctx);

int  exfat_mount(exfat_t *vol, block_device_t *dev, uint64_t start_lba, uint64_t sector_count);
int  exfat_load_upcase(exfat_t *vol, uint16_t **table_out);
void exfat_root_dir(const exfat_t *vol, exfat_dir_t *out);
void exfat_entry_dir(const exfat_entry_t *e, exfat_dir_t *out);
int  exfat_iterate(exfat_t *vol, const exfat_dir_t *dir, exfat_iter_fn fn, void *ctx);
int  exfat_lookup(exfat_t *vol, const char *path, exfat_entry_t *out);
int64_t exfat_read_range(exfat_t *vol, const exfat_entry_t *e, uint64_t off, void *buf, uint64_t len,
                         exfat_hint_t *hint);
int  exfat_write(exfat_t *vol, const char *path, const void *data, uint64_t size);
int  exfat_mkdir(exfat_t *vol, const char *path);
int  exfat_remove(exfat_t *vol, const char *path);
int  exfat_flush(exfat_t *vol);
int  exfat_write_at(exfat_t *vol, const char *path, uint64_t off, const void *data, uint64_t len);
int  exfat_truncate(exfat_t *vol, const char *path, uint64_t len);
int  exfat_usage(exfat_t *vol, uint32_t *free_clusters);

#endif
