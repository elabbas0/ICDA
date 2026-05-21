#ifndef INSTALL_H
#define INSTALL_H

#include <stdint.h>
#include "../drivers/storage/partition.h"

int system_install_run(uint64_t *files_installed, uint64_t *bytes_installed);
int system_install_device(uint32_t device_index, uint64_t *files_installed, uint64_t *bytes_installed);
int system_install_partitions(uint32_t efi_partition_index, uint32_t root_partition_index, int32_t swap_partition_index,
                              uint64_t *files_installed, uint64_t *bytes_installed);
int system_install_present(void);
int system_install_write_root_bundle(const partition_info_t *part, const char *bundle, uint64_t size, int32_t swap_partition_index);
int system_install_read_root_bundle(const partition_info_t *part, char **bundle_out, uint64_t *size_out, int32_t *swap_partition_index_out);

#endif
