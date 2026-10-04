#include "ntfs.h"
#include "volumes.h"

int ntfs_mount_detected(void) {
    return 0;
}

uint32_t ntfs_mount_count(void) {
    return volumes_count(VOLUME_NTFS);
}

int ntfs_mount_partition(uint32_t partition_index, const char *mount_path) {
    return volumes_mount_partition(partition_index, mount_path);
}
