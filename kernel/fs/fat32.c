#include "fat32.h"
#include "volumes.h"

int fat32_mount_detected(void) {
    return volumes_mount_all();
}

uint32_t fat32_mount_count(void) {
    return volumes_count(VOLUME_FAT32);
}

int fat32_mount_partition(uint32_t partition_index, const char *mount_path) {
    return volumes_mount_partition(partition_index, mount_path);
}
