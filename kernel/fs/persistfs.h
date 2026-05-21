#ifndef PERSISTFS_H
#define PERSISTFS_H

#include <stdint.h>

#define PERSISTFS_RESERVED_SECTORS 32768U

int persistfs_init(void);
int persistfs_sync(void);
int persistfs_sync_device(uint32_t device_index);
int persistfs_export_image(char **buffer_out, uint64_t *size_out, uint64_t *entry_count_out);
int persistfs_import_image(const char *buffer, uint64_t size, uint64_t *entries_loaded_out);
void persistfs_set_live_mode(int enabled);
int persistfs_live_mode(void);
int persistfs_active_device(void);
int persistfs_active_partition(void);
int persistfs_present(void);
uint64_t persistfs_loaded_entries(void);

#endif
