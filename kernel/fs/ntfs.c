#include "ntfs.h"
#include "vfs.h"
#include "../drivers/storage/partition.h"

#define NTFS_PATH_CAP 128U
#define NTFS_INFO_CAP 256U

static uint32_t mounted_ntfs = 0;

static void copy_text(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void append_text(char *dst, const char *src, uint32_t cap) {
    uint32_t out = 0;
    while (dst[out]) out++;
    for (uint32_t i = 0; src && src[i] && out + 1 < cap; i++) {
        dst[out++] = src[i];
    }
    dst[out] = 0;
}

static void append_u64(char *dst, uint32_t cap, uint64_t value) {
    char tmp[32];
    uint32_t len = 0;
    if (value == 0) {
        append_text(dst, "0", cap);
        return;
    }
    while (value && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len) {
        char ch[2] = { tmp[--len], 0 };
        append_text(dst, ch, cap);
    }
}

static int ntfs_import_placeholder(const partition_info_t *part, uint32_t index) {
    char mount_path[NTFS_PATH_CAP];
    char info_path[NTFS_PATH_CAP];
    char info[NTFS_INFO_CAP];

    copy_text(mount_path, "/volumes/ntfs-", sizeof(mount_path));
    append_u64(mount_path, sizeof(mount_path), index);
    if (vfs_import_node(mount_path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) != 0) {
        return -1;
    }

    copy_text(info_path, mount_path, sizeof(info_path));
    append_text(info_path, "/_fsinfo.txt", sizeof(info_path));

    info[0] = 0;
    append_text(info, "filesystem: ntfs\n", sizeof(info));
    append_text(info, "mode: read-only mount placeholder\n", sizeof(info));
    append_text(info, "device: ", sizeof(info));
    append_text(info, part->device && part->device->name ? part->device->name : "disk", sizeof(info));
    append_text(info, "\nstart_lba: ", sizeof(info));
    append_u64(info, sizeof(info), part->start_lba);
    append_text(info, "\nsectors: ", sizeof(info));
    append_u64(info, sizeof(info), part->sector_count);
    append_text(info, "\npartition: ", sizeof(info));
    append_text(info, part->name, sizeof(info));
    append_text(info, "\n", sizeof(info));

    return vfs_import_node(info_path, VFS_NODE_FILE, 1, info, 0, 0, 0, 0);
}

int ntfs_mount_detected(void) {
    mounted_ntfs = 0;
    (void)vfs_mkdir(vfs_root(), "/volumes");
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        if (!part || part->fs_hint != PARTITION_FS_NTFS) continue;
        if (ntfs_import_placeholder(part, mounted_ntfs) == 0) {
            mounted_ntfs++;
        }
    }
    return 0;
}

uint32_t ntfs_mount_count(void) {
    return mounted_ntfs;
}

int ntfs_mount_partition(uint32_t partition_index, const char *mount_path) {
    (void)partition_index;
    (void)mount_path;
    return -1;
}
