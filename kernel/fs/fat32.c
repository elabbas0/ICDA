#include "fat32.h"
#include "vfs.h"
#include "../drivers/storage/partition.h"
#include "../memory/heap.h"

#define FAT32_SECTOR_SIZE 512U
#define FAT32_PATH_CAP 256U
#define FAT32_MAX_CLUSTERS_PER_FILE 4096U

typedef struct {
    const partition_info_t *part;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
} fat32_volume_t;

typedef struct {
    char name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) fat_dirent_t;

static uint32_t mounted_fat32 = 0;

static void zero_bytes(uint8_t *dst, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) dst[i] = 0;
}

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) len++;
    return len;
}

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

static void append_u32(char *dst, uint32_t cap, uint32_t value) {
    char tmp[16];
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
        char out[2] = { tmp[--len], 0 };
        append_text(dst, out, cap);
    }
}

static int fat32_read_sectors(const fat32_volume_t *vol, uint64_t rel_lba, uint32_t count, void *buffer) {
    return vol->part->device->read(vol->part->device->context, vol->part->start_lba + rel_lba, count, buffer);
}

static uint64_t cluster_to_lba(const fat32_volume_t *vol, uint32_t cluster) {
    return vol->data_lba + (uint64_t)(cluster - 2) * vol->sectors_per_cluster;
}

static int fat32_load_volume(const partition_info_t *part, fat32_volume_t *vol) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;

    if (!part || !vol || part->fs_hint != PARTITION_FS_FAT32) return -1;
    if (part->device->read(part->device->context, part->start_lba, 1, sector) != 0) return -1;
    bytes_per_sector = *(uint16_t *)&sector[11];
    sectors_per_cluster = sector[13];
    reserved_sectors = *(uint16_t *)&sector[14];
    fat_count = sector[16];
    fat_size_sectors = *(uint32_t *)&sector[36];
    root_cluster = *(uint32_t *)&sector[44];

    if (bytes_per_sector != FAT32_SECTOR_SIZE || sectors_per_cluster == 0 || fat_count == 0 || fat_size_sectors == 0 || root_cluster < 2) {
        return -1;
    }

    vol->part = part;
    vol->bytes_per_sector = bytes_per_sector;
    vol->sectors_per_cluster = sectors_per_cluster;
    vol->reserved_sectors = reserved_sectors;
    vol->fat_count = fat_count;
    vol->fat_size_sectors = fat_size_sectors;
    vol->root_cluster = root_cluster;
    vol->fat_lba = reserved_sectors;
    vol->data_lba = reserved_sectors + (uint64_t)fat_count * fat_size_sectors;
    return 0;
}

static uint32_t fat32_next_cluster(const fat32_volume_t *vol, uint32_t cluster) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t fat_offset = cluster * 4U;
    uint64_t sector_index = vol->fat_lba + (fat_offset / FAT32_SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    uint32_t value;

    if (fat32_read_sectors(vol, sector_index, 1, sector) != 0) return 0x0FFFFFFFU;
    value = *(uint32_t *)(sector + entry_offset) & 0x0FFFFFFFU;
    return value;
}

static int fat32_build_name(const fat_dirent_t *entry, char *out, uint32_t cap) {
    uint32_t pos = 0;
    uint32_t i;

    if (!entry || !out || cap == 0) return -1;
    if (entry->name[0] == 0x00 || (uint8_t)entry->name[0] == 0xE5) return -1;
    if (entry->attr == 0x0F) return -1;

    for (i = 0; i < 8 && entry->name[i] != ' '; i++) {
        if (pos + 1 >= cap) return -1;
        out[pos++] = entry->name[i];
    }
    if (!(entry->attr & 0x10) && entry->name[8] != ' ') {
        if (pos + 1 >= cap) return -1;
        out[pos++] = '.';
        for (i = 8; i < 11 && entry->name[i] != ' '; i++) {
            if (pos + 1 >= cap) return -1;
            out[pos++] = entry->name[i];
        }
    }
    out[pos] = 0;
    return 0;
}

static uint32_t fat32_entry_cluster(const fat_dirent_t *entry) {
    return ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;
}

static int fat32_read_file(const fat32_volume_t *vol, const fat_dirent_t *entry, char **data_out) {
    uint32_t cluster = fat32_entry_cluster(entry);
    uint32_t file_size = entry->file_size;
    uint8_t *data;
    uint8_t *cursor;
    uint32_t cluster_count = 0;
    uint32_t cluster_bytes;

    if (!data_out) return -1;
    *data_out = 0;
    if (file_size == 0) {
        data = (uint8_t *)kmalloc(1);
        if (!data) return -1;
        data[0] = 0;
        *data_out = (char *)data;
        return 0;
    }
    cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    data = (uint8_t *)kmalloc(file_size + 1);
    if (!data) return -1;
    cursor = data;
    while (cluster >= 2 && cluster < 0x0FFFFFF8U && file_size > 0 && cluster_count < FAT32_MAX_CLUSTERS_PER_FILE) {
        uint8_t sector[FAT32_SECTOR_SIZE * 8];
        uint32_t take = file_size < cluster_bytes ? file_size : cluster_bytes;
        if (vol->sectors_per_cluster > 8) {
            kfree(data);
            return -1;
        }
        if (fat32_read_sectors(vol, cluster_to_lba(vol, cluster), vol->sectors_per_cluster, sector) != 0) {
            kfree(data);
            return -1;
        }
        for (uint32_t i = 0; i < take; i++) cursor[i] = sector[i];
        cursor += take;
        file_size -= take;
        cluster = fat32_next_cluster(vol, cluster);
        cluster_count++;
    }
    data[entry->file_size] = 0;
    *data_out = (char *)data;
    return 0;
}

static int fat32_import_dir(const fat32_volume_t *vol, uint32_t cluster, const char *mount_path, uint32_t depth) {
    uint32_t cluster_count = 0;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;

    if (depth > 12 || cluster < 2) return 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8U && cluster_count < FAT32_MAX_CLUSTERS_PER_FILE) {
        uint8_t *buf;
        uint32_t entry_count;

        buf = (uint8_t *)kmalloc(cluster_bytes);
        if (!buf) return -1;
        if (fat32_read_sectors(vol, cluster_to_lba(vol, cluster), vol->sectors_per_cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }

        entry_count = cluster_bytes / sizeof(fat_dirent_t);
        for (uint32_t i = 0; i < entry_count; i++) {
            fat_dirent_t *entry = (fat_dirent_t *)(buf + i * sizeof(fat_dirent_t));
            char name[32];
            char path[FAT32_PATH_CAP];
            uint32_t child_cluster;

            if ((uint8_t)entry->name[0] == 0x00) break;
            if (fat32_build_name(entry, name, sizeof(name)) != 0) continue;
            if ((name[0] == '.' && name[1] == 0) || (name[0] == '.' && name[1] == '.' && name[2] == 0)) continue;

            copy_text(path, mount_path, sizeof(path));
            if (str_len(path) == 0) copy_text(path, "/", sizeof(path));
            if (str_len(path) > 1) append_text(path, "/", sizeof(path));
            append_text(path, name, sizeof(path));

            child_cluster = fat32_entry_cluster(entry);
            if (entry->attr & 0x10) {
                if (vfs_import_node(path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) != 0) {
                    kfree(buf);
                    return -1;
                }
                if (child_cluster >= 2 && fat32_import_dir(vol, child_cluster, path, depth + 1) != 0) {
                    kfree(buf);
                    return -1;
                }
            } else {
                char *data = 0;
                if (fat32_read_file(vol, entry, &data) != 0) {
                    continue;
                }
                if (vfs_import_node(path, VFS_NODE_FILE, 1, data, entry->file_size, 0, 0, 0) != 0) {
                    kfree(data);
                    kfree(buf);
                    return -1;
                }
                kfree(data);
            }
        }

        kfree(buf);
        cluster = fat32_next_cluster(vol, cluster);
        cluster_count++;
    }
    return 0;
}

static int fat32_mount_partition_info(const partition_info_t *part, const char *mount_path) {
    fat32_volume_t vol;

    if (!part || !mount_path || !*mount_path || part->fs_hint != PARTITION_FS_FAT32) {
        return -1;
    }
    if (fat32_load_volume(part, &vol) != 0) {
        return -1;
    }
    if (vfs_import_node(mount_path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) != 0) {
        return -1;
    }
    if (fat32_import_dir(&vol, vol.root_cluster, mount_path, 0) != 0) {
        return -1;
    }
    return 0;
}

int fat32_mount_detected(void) {
    mounted_fat32 = 0;
    (void)vfs_mkdir(vfs_root(), "/volumes");
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        char mount_path[64];

        if (!part || part->fs_hint != PARTITION_FS_FAT32) continue;
        copy_text(mount_path, "/volumes/fat32-", sizeof(mount_path));
        append_u32(mount_path, sizeof(mount_path), mounted_fat32);
        if (fat32_mount_partition_info(part, mount_path) == 0) {
            mounted_fat32++;
        }
    }
    return 0;
}

uint32_t fat32_mount_count(void) {
    return mounted_fat32;
}

int fat32_mount_partition(uint32_t partition_index, const char *mount_path) {
    const partition_info_t *part = partition_get(partition_index);
    return fat32_mount_partition_info(part, mount_path);
}
