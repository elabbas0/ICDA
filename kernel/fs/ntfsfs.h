#ifndef NTFSFS_H
#define NTFSFS_H

#include <stdint.h>
#include "../drivers/storage/block.h"

#define NTFS_ROOT_RECORD 5ULL
#define NTFS_RUNS_MAX    512U

typedef struct {
    uint64_t vcn;
    uint64_t lcn;
    uint64_t len;
    int      sparse;
} ntfs_run_t;

typedef struct {
    block_device_t *dev;
    uint64_t base_lba;
    uint64_t sectors;
    uint32_t bytes_per_sector;
    uint32_t cluster_bytes;
    uint32_t record_bytes;
    uint32_t index_bytes;
    uint64_t mft_size;
    ntfs_run_t mft_runs[NTFS_RUNS_MAX];
    uint32_t mft_run_count;
} ntfs_t;

typedef int (*ntfs_dir_fn)(const char *name, uint64_t record, int is_dir, uint64_t size, void *ctx);

int     ntfs_mount(ntfs_t *vol, block_device_t *dev, uint64_t start_lba, uint64_t sector_count);
int     ntfs_list_dir(ntfs_t *vol, uint64_t record, ntfs_dir_fn fn, void *ctx);
int64_t ntfs_read_file(ntfs_t *vol, uint64_t record, uint64_t off, void *buf, uint64_t len);
int64_t ntfs_file_size(ntfs_t *vol, uint64_t record);

#endif
