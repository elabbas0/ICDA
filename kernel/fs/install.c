#include "install.h"

#include "boot_assets.h"
#include "initramfs.h"
#include "persistfs.h"
#include "vfs.h"

#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../drivers/serial/serial.h"
#include "../memory/heap.h"

#define FAT32_SECTOR_SIZE 512U
#define FAT32_ATTR_DIR    0x10U
#define FAT32_ATTR_FILE   0x20U
#define FAT32_EOC         0x0FFFFFFFU

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
    uint32_t max_cluster;
} fat32_install_volume_t;

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
} __attribute__((packed)) fat32_dirent_t;

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

static uint64_t append_text(char *buf, uint64_t out, uint64_t cap, const char *text);
static uint64_t append_uint(char *buf, uint64_t out, uint64_t cap, uint64_t value);

static void install_log(const char *text) {
    serial_write("[install] ");
    serial_write(text);
    serial_write("\n");
}

static void install_log_u64(const char *label, uint64_t value) {
    char buf[64];
    uint64_t out = 0;
    buf[0] = '\0';
    out = append_text(buf, out, sizeof(buf), label);
    out = append_uint(buf, out, sizeof(buf), value);
    install_log(buf);
}

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static void zero_bytes(uint8_t *dst, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = 0;
    }
}

static uint64_t append_text(char *buf, uint64_t out, uint64_t cap, const char *text) {
    uint64_t i = 0;
    while (text && text[i] && out + 1 < cap) {
        buf[out++] = text[i++];
    }
    if (out < cap) {
        buf[out] = '\0';
    }
    return out;
}

static uint64_t append_uint(char *buf, uint64_t out, uint64_t cap, uint64_t value) {
    char tmp[32];
    uint64_t len = 0;

    if (value == 0) {
        if (out + 1 < cap) {
            buf[out++] = '0';
            buf[out] = '\0';
        }
        return out;
    }

    while (value && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len && out + 1 < cap) {
        buf[out++] = tmp[--len];
    }
    if (out < cap) {
        buf[out] = '\0';
    }
    return out;
}

static int str_prefix(const char *text, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int ensure_layout(void) {
    (void)vfs_mkdir(vfs_root(), "/system");
    (void)vfs_mkdir(vfs_root(), "/system/install");
    (void)vfs_mkdir(vfs_root(), "/home");
    (void)vfs_mkdir(vfs_root(), "/apps");
    (void)vfs_mkdir(vfs_root(), "/bin");
    (void)vfs_mkdir(vfs_root(), "/etc");
    (void)vfs_mkdir(vfs_root(), "/usr");
    (void)vfs_mkdir(vfs_root(), "/usr/share");
    (void)vfs_mkdir(vfs_root(), "/usr/share/audio");
    (void)vfs_mkdir(vfs_root(), "/volumes");
    return 0;
}

static int fat32_read_sectors(const fat32_install_volume_t *vol, uint64_t rel_lba, uint32_t count, void *buffer) {
    return vol->part->device->read(vol->part->device->context, vol->part->start_lba + rel_lba, count, buffer);
}

static int fat32_write_sectors(const fat32_install_volume_t *vol, uint64_t rel_lba, uint32_t count, const void *buffer) {
    return vol->part->device->write(vol->part->device->context, vol->part->start_lba + rel_lba, count, buffer);
}

static uint64_t fat32_cluster_lba(const fat32_install_volume_t *vol, uint32_t cluster) {
    return vol->data_lba + (uint64_t)(cluster - 2U) * vol->sectors_per_cluster;
}

static int fat32_install_load(const partition_info_t *part, fat32_install_volume_t *vol) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t data_sectors;
    uint32_t cluster_count;

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

    data_sectors = (uint32_t)part->sector_count - reserved_sectors - fat_count * fat_size_sectors;
    cluster_count = data_sectors / sectors_per_cluster;

    vol->part = part;
    vol->bytes_per_sector = bytes_per_sector;
    vol->sectors_per_cluster = sectors_per_cluster;
    vol->reserved_sectors = reserved_sectors;
    vol->fat_count = fat_count;
    vol->fat_size_sectors = fat_size_sectors;
    vol->root_cluster = root_cluster;
    vol->fat_lba = reserved_sectors;
    vol->data_lba = reserved_sectors + (uint64_t)fat_count * fat_size_sectors;
    vol->max_cluster = cluster_count + 1U;
    return 0;
}

static uint32_t fat32_get_fat_entry(const fat32_install_volume_t *vol, uint32_t cluster) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t fat_offset = cluster * 4U;
    uint64_t sector_index = vol->fat_lba + (fat_offset / FAT32_SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    uint32_t value;

    if (fat32_read_sectors(vol, sector_index, 1, sector) != 0) return FAT32_EOC;
    value = *(uint32_t *)(sector + entry_offset) & 0x0FFFFFFFU;
    return value;
}

static int fat32_set_fat_entry(const fat32_install_volume_t *vol, uint32_t cluster, uint32_t value) {
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t fat_offset = cluster * 4U;
    uint64_t sector_index = fat_offset / FAT32_SECTOR_SIZE;
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;

    for (uint8_t fat = 0; fat < vol->fat_count; fat++) {
        uint64_t lba = vol->fat_lba + sector_index + (uint64_t)fat * vol->fat_size_sectors;
        if (fat32_read_sectors(vol, lba, 1, sector) != 0) return -1;
        *(uint32_t *)(sector + entry_offset) = (*(uint32_t *)(sector + entry_offset) & 0xF0000000U) | (value & 0x0FFFFFFFU);
        if (fat32_write_sectors(vol, lba, 1, sector) != 0) return -1;
    }
    return 0;
}

static int fat32_alloc_chain(const fat32_install_volume_t *vol, uint32_t clusters_needed, uint32_t *first_cluster_out) {
    uint32_t first = 0;
    uint32_t prev = 0;

    if (!first_cluster_out || clusters_needed == 0) return -1;
    for (uint32_t cluster = 3; cluster <= vol->max_cluster && clusters_needed; cluster++) {
        if (fat32_get_fat_entry(vol, cluster) != 0) {
            continue;
        }
        if (fat32_set_fat_entry(vol, cluster, FAT32_EOC) != 0) {
            return -1;
        }
        if (!first) {
            first = cluster;
        }
        if (prev && fat32_set_fat_entry(vol, prev, cluster) != 0) {
            return -1;
        }
        prev = cluster;
        clusters_needed--;
    }

    if (clusters_needed != 0 || !first) {
        return -1;
    }
    *first_cluster_out = first;
    return 0;
}

static int fat32_write_chain(const fat32_install_volume_t *vol, uint32_t first_cluster, const char *data, uint64_t size) {
    uint32_t cluster = first_cluster;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer;
    uint64_t remaining = size;

    if (cluster < 2) return -1;
    buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;

    while (cluster >= 2 && cluster < FAT32_EOC && remaining > 0) {
        uint32_t take = remaining > cluster_bytes ? cluster_bytes : (uint32_t)remaining;
        zero_bytes(buffer, cluster_bytes);
        if (take) {
            copy_bytes((char *)buffer, data, take);
            data += take;
            remaining -= take;
        }
        if (fat32_write_sectors(vol, fat32_cluster_lba(vol, cluster), vol->sectors_per_cluster, buffer) != 0) {
            kfree(buffer);
            return -1;
        }
        cluster = fat32_get_fat_entry(vol, cluster);
    }

    kfree(buffer);
    return remaining == 0 ? 0 : -1;
}

static void fat32_short_name(const char *name, char out[11]) {
    uint32_t i = 0;
    uint32_t j = 0;
    for (i = 0; i < 11; i++) out[i] = ' ';
    i = 0;
    while (name && name[i] && name[i] != '.' && j < 8) {
        char ch = name[i++];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out[j++] = ch;
    }
    if (name && name[i] == '.') {
        i++;
        j = 8;
        while (name[i] && j < 11) {
            char ch = name[i++];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            out[j++] = ch;
        }
    }
}

static int fat32_find_free_dirent(const fat32_install_volume_t *vol, uint32_t dir_cluster, uint32_t *slot_cluster_out, uint32_t *slot_index_out) {
    uint32_t cluster = dir_cluster;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer;

    buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (fat32_read_sectors(vol, fat32_cluster_lba(vol, cluster), vol->sectors_per_cluster, buffer) != 0) {
            kfree(buffer);
            return -1;
        }
        for (uint32_t i = 0; i < cluster_bytes / sizeof(fat32_dirent_t); i++) {
            fat32_dirent_t *entry = (fat32_dirent_t *)(buffer + i * sizeof(fat32_dirent_t));
            if ((uint8_t)entry->name[0] == 0x00 || (uint8_t)entry->name[0] == 0xE5) {
                *slot_cluster_out = cluster;
                *slot_index_out = i;
                kfree(buffer);
                return 0;
            }
        }
        cluster = fat32_get_fat_entry(vol, cluster);
    }
    kfree(buffer);
    return -1;
}

static int fat32_write_dirent(const fat32_install_volume_t *vol, uint32_t dir_cluster, const fat32_dirent_t *entry_in) {
    uint32_t slot_cluster = 0;
    uint32_t slot_index = 0;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer;

    if (fat32_find_free_dirent(vol, dir_cluster, &slot_cluster, &slot_index) != 0) return -1;
    install_log_u64("dirent slot cluster=", slot_cluster);
    install_log_u64("dirent slot index=", slot_index);
    buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;
    if (fat32_read_sectors(vol, fat32_cluster_lba(vol, slot_cluster), vol->sectors_per_cluster, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    copy_bytes((char *)(buffer + slot_index * sizeof(fat32_dirent_t)), (const char *)entry_in, sizeof(fat32_dirent_t));
    if (fat32_write_sectors(vol, fat32_cluster_lba(vol, slot_cluster), vol->sectors_per_cluster, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    kfree(buffer);
    return 0;
}

static int fat32_make_dirent(const char *name, uint8_t attr, uint32_t first_cluster, uint32_t size, fat32_dirent_t *out) {
    if (!name || !out) return -1;
    zero_bytes((uint8_t *)out, sizeof(*out));
    fat32_short_name(name, out->name);
    out->attr = attr;
    out->first_cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFFU);
    out->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFFU);
    out->file_size = size;
    return 0;
}

static int fat32_create_dir(const fat32_install_volume_t *vol, uint32_t parent_cluster, const char *name, uint32_t *out_cluster) {
    uint32_t child_cluster = 0;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer;
    fat32_dirent_t dirent;
    fat32_dirent_t dot;
    fat32_dirent_t dotdot;

    if (fat32_alloc_chain(vol, 1, &child_cluster) != 0) return -1;
    buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;
    zero_bytes(buffer, cluster_bytes);
    fat32_make_dirent(".", FAT32_ATTR_DIR, child_cluster, 0, &dot);
    fat32_make_dirent("..", FAT32_ATTR_DIR, parent_cluster, 0, &dotdot);
    copy_bytes((char *)(buffer + 0 * sizeof(fat32_dirent_t)), (const char *)&dot, sizeof(dot));
    copy_bytes((char *)(buffer + 1 * sizeof(fat32_dirent_t)), (const char *)&dotdot, sizeof(dotdot));
    if (fat32_write_sectors(vol, fat32_cluster_lba(vol, child_cluster), vol->sectors_per_cluster, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    kfree(buffer);
    fat32_make_dirent(name, FAT32_ATTR_DIR, child_cluster, 0, &dirent);
    if (fat32_write_dirent(vol, parent_cluster, &dirent) != 0) return -1;
    if (out_cluster) *out_cluster = child_cluster;
    return 0;
}

static int fat32_create_file(const fat32_install_volume_t *vol, uint32_t parent_cluster, const char *name, const char *data, uint64_t size) {
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint32_t first_cluster = 0;
    uint32_t clusters_needed = 0;
    fat32_dirent_t dirent;

    if (size > 0) {
        clusters_needed = (uint32_t)((size + cluster_bytes - 1U) / cluster_bytes);
        if (fat32_alloc_chain(vol, clusters_needed, &first_cluster) != 0) return -1;
        if (fat32_write_chain(vol, first_cluster, data, size) != 0) return -1;
    }
    fat32_make_dirent(name, FAT32_ATTR_FILE, first_cluster, (uint32_t)size, &dirent);
    return fat32_write_dirent(vol, parent_cluster, &dirent);
}

static int fat32_install_boot_partition(const partition_info_t *part) {
    fat32_install_volume_t vol;
    uint32_t efi_cluster = 0;
    uint32_t boot_in_efi_cluster = 0;
    uint32_t boot_root_cluster = 0;
    uint32_t grub_cluster = 0;
    static const char startup_nsh[] = "\\EFI\\BOOT\\BOOTX64.EFI\r\n";

    install_log("boot-part: begin");
    if (boot_asset_efi_start == 0 || boot_asset_efi_size() == 0 ||
        boot_asset_grub_cfg_start == 0 || boot_asset_grub_cfg_size() == 0 ||
        boot_asset_kernel_bin_start == 0 || boot_asset_kernel_bin_size() == 0) {
        install_log("boot-part: boot assets missing");
        return -31;
    }
    if (fat32_install_load(part, &vol) != 0) return -32;
    if (fat32_create_dir(&vol, vol.root_cluster, "EFI", &efi_cluster) != 0) return -33;
    if (fat32_create_dir(&vol, efi_cluster, "BOOT", &boot_in_efi_cluster) != 0) return -34;
    if (fat32_create_dir(&vol, vol.root_cluster, "BOOT", &boot_root_cluster) != 0) return -35;
    if (fat32_create_dir(&vol, boot_root_cluster, "GRUB", &grub_cluster) != 0) return -36;
    if (fat32_create_file(&vol, vol.root_cluster, "STARTUP.NSH", startup_nsh, sizeof(startup_nsh) - 1) != 0) return -36;
    if (fat32_create_file(&vol, boot_in_efi_cluster, "BOOTX64.EFI", boot_asset_efi_start, boot_asset_efi_size()) != 0) return -37;
    if (fat32_create_file(&vol, grub_cluster, "GRUB.CFG", boot_asset_grub_cfg_start, boot_asset_grub_cfg_size()) != 0) return -38;
    if (fat32_create_file(&vol, boot_root_cluster, "KERNEL.BIN", boot_asset_kernel_bin_start, boot_asset_kernel_bin_size()) != 0) return -39;
    install_log("boot-part: done");
    return 0;
}

static int system_install_core(uint64_t *files_installed, uint64_t *bytes_installed) {
    uint64_t count = initramfs_file_count();
    uint64_t total_bytes = 0;
    uint64_t installed_files = 0;
    uint64_t manifest_cap = 512;
    uint64_t out = 0;
    char *manifest = 0;
    char status[256];

    install_log("core: begin");
    if (ensure_layout() != 0) {
        install_log("core: ensure_layout failed");
        return -21;
    }

    manifest = (char *)kmalloc((size_t)manifest_cap);
    if (!manifest) {
        install_log("core: manifest alloc failed");
        return -22;
    }
    manifest[0] = '\0';
    out = append_text(manifest, out, manifest_cap, "installed files:\n");

    for (uint64_t i = 0; i < count; i++) {
        const initramfs_file_t *file = initramfs_file_at(i);
        uint64_t needed;
        if (!file || !file->path) {
            kfree(manifest);
            install_log("core: bad initramfs entry");
            return -23;
        }
        if (str_prefix(file->path, "/usr/share/audio/")) {
            continue;
        }

        if (vfs_import_node(file->path, VFS_NODE_FILE, 0, file->data ? file->data : "", file->size, 0, 0, 0) != 0) {
            kfree(manifest);
            install_log("core: vfs_import_node failed");
            return -24;
        }
        installed_files++;
        total_bytes += file->size;

        needed = str_len(file->path) + 32;
        while (out + needed + 1 >= manifest_cap) {
            char *next;
            uint64_t next_cap = manifest_cap * 2;
            next = (char *)kmalloc((size_t)next_cap);
            if (!next) {
                kfree(manifest);
                install_log("core: manifest grow failed");
                return -25;
            }
            copy_bytes(next, manifest, out + 1);
            kfree(manifest);
            manifest = next;
            manifest_cap = next_cap;
        }

        out = append_text(manifest, out, manifest_cap, "  ");
        out = append_text(manifest, out, manifest_cap, file->path);
        out = append_text(manifest, out, manifest_cap, " bytes=");
        out = append_uint(manifest, out, manifest_cap, file->size);
        out = append_text(manifest, out, manifest_cap, "\n");
    }

    out = 0;
    status[0] = '\0';
    out = append_text(status, out, sizeof(status), "installed=yes\nfiles=");
    out = append_uint(status, out, sizeof(status), installed_files);
    out = append_text(status, out, sizeof(status), "\nbytes=");
    out = append_uint(status, out, sizeof(status), total_bytes);
    out = append_text(status, out, sizeof(status), "\nmode=core-system-writable-overlay\n");
    out = append_text(status, out, sizeof(status), "excluded=/usr/share/audio/*\n");

    if (vfs_write(vfs_root(), "/system/install/state.txt", status, str_len(status)) != 0) {
        kfree(manifest);
        install_log("core: state.txt write failed");
        return -26;
    }
    if (vfs_write(vfs_root(), "/system/install/manifest.txt", manifest, str_len(manifest)) != 0) {
        kfree(manifest);
        install_log("core: manifest.txt write failed");
        return -27;
    }
    kfree(manifest);

    if (vfs_sync() != 0) {
        install_log("core: vfs_sync failed");
        return -28;
    }

    if (files_installed) {
        *files_installed = installed_files;
    }
    if (bytes_installed) {
        *bytes_installed = total_bytes;
    }
    install_log("core: done");
    return 0;
}

int system_install_present(void) {
    vfs_stat_t st;
    return vfs_stat(vfs_root(), "/system/install/state.txt", &st) == 0;
}

int system_install_run(uint64_t *files_installed, uint64_t *bytes_installed) {
    return system_install_core(files_installed, bytes_installed);
}

int system_install_device(uint32_t device_index, uint64_t *files_installed, uint64_t *bytes_installed) {
    block_device_t *dev = block_get(device_index);
    const partition_info_t *boot_part = 0;
    const partition_info_t *system_part = 0;
    uint64_t files = 0;
    uint64_t bytes = 0;

    install_log("device: begin");
    if (!dev) {
        install_log("device: no such device");
        return -10;
    }

    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        if (!part || part->device != dev || part->fs_hint != PARTITION_FS_FAT32) {
            continue;
        }
        if (!boot_part) {
            boot_part = part;
        } else if (!system_part) {
            system_part = part;
            break;
        }
    }

    if (!boot_part || !system_part) {
        install_log("device: missing fat32 partitions");
        return -11;
    }
    if (system_install_core(&files, &bytes) != 0) {
        install_log("device: core failed");
        return -12;
    }
    if (fat32_install_boot_partition(boot_part) != 0) {
        install_log("device: boot partition install failed");
        return -13;
    }
    {
        int sync_rc = persistfs_sync_device(device_index);
        if (sync_rc != 0) {
        install_log("device: persistfs sync failed");
        return sync_rc;
        }
    }
    if (files_installed) {
        *files_installed = files;
    }
    if (bytes_installed) {
        *bytes_installed = bytes;
    }
    install_log("device: done");
    return 0;
}
