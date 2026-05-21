#include "install.h"

#include "boot_assets.h"
#include "diskfmt.h"
#include "initramfs.h"
#include "persistfs.h"
#include "vfs.h"

#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"
#include "../drivers/display/vga.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../memory/heap.h"

#define FAT32_SECTOR_SIZE 512U
#define FAT32_ATTR_DIR    0x10U
#define FAT32_ATTR_FILE   0x20U
#define FAT32_EOC         0x0FFFFFFFU
#define ICDA_ROOT_BUNDLE_NAME "ICDAROOT.BIN"
#define ICDA_ROOT_CFG_NAME    "ICDACFG.TXT"

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
static void install_progress(const char *stage, const char *detail, uint64_t current, uint64_t total);
static void install_progress_done(void);

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static int install_console_cols(void) {
    if (fb_available()) {
        return fb_columns();
    }
    return VGA_WIDTH;
}

static int install_console_rows(void) {
    if (fb_available()) {
        return fb_rows();
    }
    return VGA_HEIGHT;
}

static void install_fill_line(char *dst, uint64_t cap, char fill) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    for (; i + 1 < cap; i++) {
        dst[i] = fill;
    }
    dst[i] = '\0';
}

static void install_write_line(int col, int row, int width, const char *text, console_style_t style) {
    char line[160];
    uint64_t len = str_len(text);
    uint64_t copy = min_u64((uint64_t)width, len);

    if (width <= 0) return;
    install_fill_line(line, sizeof(line), ' ');
    if ((uint64_t)width >= sizeof(line)) {
        width = (int)sizeof(line) - 1;
    }
    for (uint64_t i = 0; i < copy; i++) {
        line[i] = text[i];
    }
    line[width] = '\0';
    console_set_cursor(col, row);
    console_write(line, style);
}

static void install_progress(const char *stage, const char *detail, uint64_t current, uint64_t total) {
    char line[160];
    int cols = install_console_cols();
    int rows = install_console_rows();
    int width = cols > 72 ? 72 : cols - 4;
    int left;
    int top;
    int bar_width;
    uint64_t filled;

    if (width < 32 || rows < 10) {
        return;
    }

    left = (cols - width) / 2;
    top = (rows - 7) / 2;
    bar_width = width - 16;
    if (bar_width < 10) bar_width = 10;

    console_clear();

    install_fill_line(line, sizeof(line), '=');
    line[width] = '\0';
    install_write_line(left, top + 0, width, line, CONSOLE_STYLE_MUTED);

    install_fill_line(line, sizeof(line), ' ');
    line[0] = '['; line[1] = ' ';
    {
        uint64_t out = 2;
        out = append_text(line, out, sizeof(line), "ICDA Installer");
        if (stage && stage[0]) {
            out = append_text(line, out, sizeof(line), "  ");
            out = append_text(line, out, sizeof(line), stage);
        }
    }
    line[width - 2] = ' ';
    line[width - 1] = ']';
    line[width] = '\0';
    install_write_line(left, top + 1, width, line, CONSOLE_STYLE_INFO);

    install_fill_line(line, sizeof(line), ' ');
    if (detail && detail[0]) {
        append_text(line, 0, sizeof(line), detail);
    }
    line[width] = '\0';
    install_write_line(left, top + 2, width, line, CONSOLE_STYLE_ACCENT);

    install_fill_line(line, sizeof(line), ' ');
    line[0] = '[';
    line[bar_width + 1] = ']';
    line[bar_width + 2] = ' ';
    if (total == 0) {
        total = 1;
    }
    if (current > total) {
        current = total;
    }
    filled = (current * (uint64_t)bar_width) / total;
    for (int i = 0; i < bar_width; i++) {
        line[1 + i] = ((uint64_t)i < filled) ? '#' : '-';
    }
    {
        uint64_t out = (uint64_t)(bar_width + 3);
        out = append_uint(line, out, sizeof(line), (current * 100U) / total);
        out = append_text(line, out, sizeof(line), "%");
    }
    line[width] = '\0';
    install_write_line(left, top + 4, width, line, CONSOLE_STYLE_OK);

    install_fill_line(line, sizeof(line), ' ');
    append_text(line, 0, sizeof(line), "Installing system files and boot assets...");
    line[width] = '\0';
    install_write_line(left, top + 6, width, line, CONSOLE_STYLE_MUTED);
}

static void install_progress_done(void) {
    console_clear();
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

static int parse_uint64(const char *text, uint64_t *out) {
    uint64_t value = 0;
    uint64_t i = 0;
    if (!text || !*text || !out) return 0;
    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (uint64_t)(text[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    *out = value;
    return 1;
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

static int fat32_set_contiguous_chain(const fat32_install_volume_t *vol, uint32_t first_cluster, uint32_t clusters_needed) {
    uint32_t last_cluster = first_cluster + clusters_needed - 1U;
    uint32_t first_sector = (first_cluster * 4U) / FAT32_SECTOR_SIZE;
    uint32_t last_sector = (last_cluster * 4U) / FAT32_SECTOR_SIZE;
    uint8_t sector[FAT32_SECTOR_SIZE];

    for (uint8_t fat = 0; fat < vol->fat_count; fat++) {
        uint64_t fat_base = vol->fat_lba + (uint64_t)fat * vol->fat_size_sectors;
        for (uint32_t sector_index = first_sector; sector_index <= last_sector; sector_index++) {
            if (fat32_read_sectors(vol, fat_base + sector_index, 1, sector) != 0) {
                return -1;
            }
            for (uint32_t offset = 0; offset < FAT32_SECTOR_SIZE; offset += 4) {
                uint32_t cluster = sector_index * (FAT32_SECTOR_SIZE / 4U) + (offset / 4U);
                uint32_t value;
                if (cluster < first_cluster || cluster > last_cluster) {
                    continue;
                }
                value = (cluster == last_cluster) ? FAT32_EOC : (cluster + 1U);
                *(uint32_t *)(sector + offset) =
                    (*(uint32_t *)(sector + offset) & 0xF0000000U) | (value & 0x0FFFFFFFU);
            }
            if (fat32_write_sectors(vol, fat_base + sector_index, 1, sector) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int fat32_alloc_chain(const fat32_install_volume_t *vol, uint32_t clusters_needed, uint32_t *first_cluster_out) {
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    uint8_t sector[FAT32_SECTOR_SIZE];
    uint32_t entries_per_sector = FAT32_SECTOR_SIZE / 4U;

    if (!first_cluster_out || clusters_needed == 0) return -1;
    for (uint32_t sector_index = 0; sector_index < vol->fat_size_sectors; sector_index++) {
        if (fat32_read_sectors(vol, vol->fat_lba + sector_index, 1, sector) != 0) {
            return -1;
        }
        for (uint32_t entry_index = 0; entry_index < entries_per_sector; entry_index++) {
            uint32_t cluster = sector_index * entries_per_sector + entry_index;
            uint32_t value;

            if (cluster < 3 || cluster > vol->max_cluster) {
                continue;
            }

            value = (*(uint32_t *)(sector + entry_index * 4U)) & 0x0FFFFFFFU;
            if (value != 0) {
                run_start = 0;
                run_len = 0;
                continue;
            }
            if (run_start == 0) {
                run_start = cluster;
            }
            run_len++;
            if (run_len == clusters_needed) {
                if (fat32_set_contiguous_chain(vol, run_start, clusters_needed) != 0) {
                    return -1;
                }
                *first_cluster_out = run_start;
                return 0;
            }
        }
    }
    return -1;
}

static int fat32_write_chain(const fat32_install_volume_t *vol, uint32_t first_cluster, const char *data, uint64_t size, const char *label) {
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint32_t max_batch_sectors = 128U;
    uint32_t max_batch_bytes;
    uint8_t *buffer;
    uint64_t remaining = size;
    uint64_t written = 0;
    uint64_t next_report = 0;
    uint64_t lba;

    if (first_cluster < 2) return -1;
    max_batch_bytes = max_batch_sectors * FAT32_SECTOR_SIZE;
    if (max_batch_bytes < cluster_bytes) {
        max_batch_bytes = cluster_bytes;
    }
    buffer = (uint8_t *)kmalloc(max_batch_bytes);
    if (!buffer) return -1;

    lba = fat32_cluster_lba(vol, first_cluster);
    while (remaining > 0) {
        uint32_t take = remaining > max_batch_bytes ? (uint32_t)max_batch_bytes : (uint32_t)remaining;
        uint32_t sectors = (take + FAT32_SECTOR_SIZE - 1U) / FAT32_SECTOR_SIZE;
        uint32_t write_bytes = sectors * FAT32_SECTOR_SIZE;

        zero_bytes(buffer, write_bytes);
        copy_bytes((char *)buffer, data, take);
        if (fat32_write_sectors(vol, lba, sectors, buffer) != 0) {
            kfree(buffer);
            return -1;
        }
        data += take;
        remaining -= take;
        written += take;
        lba += sectors;
        if (label && (written >= next_report || remaining == 0)) {
            install_progress("Writing boot files", label, written, size);
            next_report = written + (256U * 1024U);
        }
    }

    kfree(buffer);
    return 0;
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

static int fat32_find_dirent_named(const fat32_install_volume_t *vol, uint32_t dir_cluster, const char *name,
                                   uint32_t *slot_cluster_out, uint32_t *slot_index_out, fat32_dirent_t *entry_out) {
    uint32_t cluster = dir_cluster;
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer;
    char short_name[11];

    fat32_short_name(name, short_name);
    buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (fat32_read_sectors(vol, fat32_cluster_lba(vol, cluster), vol->sectors_per_cluster, buffer) != 0) {
            kfree(buffer);
            return -1;
        }
        for (uint32_t i = 0; i < cluster_bytes / sizeof(fat32_dirent_t); i++) {
            fat32_dirent_t *entry = (fat32_dirent_t *)(buffer + i * sizeof(fat32_dirent_t));
            int match = 1;
            if ((uint8_t)entry->name[0] == 0x00) break;
            if ((uint8_t)entry->name[0] == 0xE5 || entry->attr == 0x0F) continue;
            for (uint32_t j = 0; j < 11; j++) {
                if (entry->name[j] != short_name[j]) {
                    match = 0;
                    break;
                }
            }
            if (!match) continue;
            if (slot_cluster_out) *slot_cluster_out = cluster;
            if (slot_index_out) *slot_index_out = i;
            if (entry_out) copy_bytes((char *)entry_out, (const char *)entry, sizeof(*entry_out));
            kfree(buffer);
            return 0;
        }
        cluster = fat32_get_fat_entry(vol, cluster);
    }
    kfree(buffer);
    return -1;
}

static int fat32_write_dirent_at(const fat32_install_volume_t *vol, uint32_t slot_cluster, uint32_t slot_index, const fat32_dirent_t *entry_in) {
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint8_t *buffer = (uint8_t *)kmalloc(cluster_bytes);
    if (!buffer) return -1;
    if (fat32_read_sectors(vol, fat32_cluster_lba(vol, slot_cluster), vol->sectors_per_cluster, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    copy_bytes((char *)(buffer + slot_index * sizeof(fat32_dirent_t)), (const char *)entry_in, sizeof(*entry_in));
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

static int fat32_create_file(const fat32_install_volume_t *vol, uint32_t parent_cluster, const char *name, const char *data, uint64_t size, const char *progress_label) {
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint32_t first_cluster = 0;
    uint32_t clusters_needed = 0;
    fat32_dirent_t dirent;

    if (size > 0) {
        clusters_needed = (uint32_t)((size + cluster_bytes - 1U) / cluster_bytes);
        if (fat32_alloc_chain(vol, clusters_needed, &first_cluster) != 0) return -1;
        if (fat32_write_chain(vol, first_cluster, data, size, progress_label) != 0) return -1;
    }
    fat32_make_dirent(name, FAT32_ATTR_FILE, first_cluster, (uint32_t)size, &dirent);
    return fat32_write_dirent(vol, parent_cluster, &dirent);
}

static int fat32_upsert_file(const fat32_install_volume_t *vol, uint32_t parent_cluster, const char *name,
                             const char *data, uint64_t size, const char *progress_label) {
    uint32_t cluster_bytes = vol->sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint32_t clusters_needed = 0;
    uint32_t first_cluster = 0;
    fat32_dirent_t dirent;
    uint32_t slot_cluster = 0;
    uint32_t slot_index = 0;

    if (size > 0) {
        clusters_needed = (uint32_t)((size + cluster_bytes - 1U) / cluster_bytes);
        if (fat32_alloc_chain(vol, clusters_needed, &first_cluster) != 0) return -1;
        if (fat32_write_chain(vol, first_cluster, data, size, progress_label) != 0) return -1;
    }
    fat32_make_dirent(name, FAT32_ATTR_FILE, first_cluster, (uint32_t)size, &dirent);
    if (fat32_find_dirent_named(vol, parent_cluster, name, &slot_cluster, &slot_index, 0) == 0) {
        return fat32_write_dirent_at(vol, slot_cluster, slot_index, &dirent);
    }
    return fat32_write_dirent(vol, parent_cluster, &dirent);
}

static int fat32_install_boot_partition(const partition_info_t *part) {
    fat32_install_volume_t vol;
    uint32_t efi_cluster = 0;
    uint32_t boot_in_efi_cluster = 0;
    uint32_t boot_root_cluster = 0;
    uint32_t grub_cluster = 0;
    static const char startup_nsh[] = "\\EFI\\BOOT\\BOOTX64.EFI\r\n";

    if (boot_asset_efi_start == 0 || boot_asset_efi_size() == 0 ||
        boot_asset_grub_cfg_start == 0 || boot_asset_grub_cfg_size() == 0 ||
        boot_asset_kernel_bin_start == 0 || boot_asset_kernel_bin_size() == 0) {
        return -31;
    }
    install_progress("Preparing boot disk", "Opening EFI partition", 0, 7);
    if (fat32_install_load(part, &vol) != 0) return -32;
    install_progress("Preparing boot disk", "Creating EFI directory", 1, 7);
    if (fat32_create_dir(&vol, vol.root_cluster, "EFI", &efi_cluster) != 0) return -33;
    install_progress("Preparing boot disk", "Creating BOOT directory", 2, 7);
    if (fat32_create_dir(&vol, efi_cluster, "BOOT", &boot_in_efi_cluster) != 0) return -34;
    install_progress("Preparing boot disk", "Creating /BOOT", 3, 7);
    if (fat32_create_dir(&vol, vol.root_cluster, "BOOT", &boot_root_cluster) != 0) return -35;
    install_progress("Preparing boot disk", "Creating /BOOT/GRUB", 4, 7);
    if (fat32_create_dir(&vol, boot_root_cluster, "GRUB", &grub_cluster) != 0) return -36;
    install_progress("Preparing boot disk", "Writing STARTUP.NSH", 5, 7);
    if (fat32_create_file(&vol, vol.root_cluster, "STARTUP.NSH", startup_nsh, sizeof(startup_nsh) - 1, 0) != 0) return -36;
    install_progress("Writing boot files", "BOOTX64.EFI", 0, boot_asset_efi_size());
    if (fat32_create_file(&vol, boot_in_efi_cluster, "BOOTX64.EFI", boot_asset_efi_start, boot_asset_efi_size(), "BOOTX64.EFI") != 0) return -37;
    install_progress("Writing boot files", "GRUB.CFG", 0, boot_asset_grub_cfg_size());
    if (fat32_create_file(&vol, grub_cluster, "GRUB.CFG", boot_asset_grub_cfg_start, boot_asset_grub_cfg_size(), "GRUB.CFG") != 0) return -38;
    install_progress("Writing boot files", "KERNEL.BIN", 0, boot_asset_kernel_bin_size());
    if (fat32_create_file(&vol, boot_root_cluster, "KERNEL.BIN", boot_asset_kernel_bin_start, boot_asset_kernel_bin_size(), "KERNEL.BIN") != 0) return -39;
    install_progress("Preparing boot disk", "Boot partition complete", 7, 7);
    return 0;
}

int system_install_write_root_bundle(const partition_info_t *part, const char *bundle, uint64_t size, int32_t swap_partition_index) {
    fat32_install_volume_t vol;
    char cfg[64];
    uint64_t out = 0;

    if (!part || part->fs_hint != PARTITION_FS_FAT32 || !bundle || size == 0) return -41;
    if (fat32_install_load(part, &vol) != 0) return -42;
    install_progress("Preparing root partition", "Writing ICDA bundle", 0, size);
    if (fat32_upsert_file(&vol, vol.root_cluster, ICDA_ROOT_BUNDLE_NAME, bundle, size, ICDA_ROOT_BUNDLE_NAME) != 0) return -43;

    cfg[0] = '\0';
    out = append_text(cfg, out, sizeof(cfg), "swap=");
    if (swap_partition_index >= 0) {
        out = append_uint(cfg, out, sizeof(cfg), (uint64_t)swap_partition_index);
    } else {
        out = append_text(cfg, out, sizeof(cfg), "-1");
    }
    out = append_text(cfg, out, sizeof(cfg), "\n");
    if (fat32_upsert_file(&vol, vol.root_cluster, ICDA_ROOT_CFG_NAME, cfg, out, 0) != 0) return -44;
    return 0;
}

int system_install_read_root_bundle(const partition_info_t *part, char **bundle_out, uint64_t *size_out, int32_t *swap_partition_index_out) {
    fat32_install_volume_t vol;
    fat32_dirent_t entry;
    char *bundle = 0;
    char *cfg = 0;
    uint64_t cfg_size = 0;
    uint32_t cluster = 0;
    uint32_t slot_cluster = 0;
    uint32_t slot_index = 0;

    if (!part || !bundle_out || !size_out || part->fs_hint != PARTITION_FS_FAT32) return -1;
    *bundle_out = 0;
    *size_out = 0;
    if (swap_partition_index_out) *swap_partition_index_out = -1;
    if (fat32_install_load(part, &vol) != 0) return -1;
    if (fat32_find_dirent_named(&vol, vol.root_cluster, ICDA_ROOT_BUNDLE_NAME, &slot_cluster, &slot_index, &entry) != 0) return -1;

    cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
    if (entry.file_size == 0 || cluster < 2) return -1;
    bundle = (char *)kmalloc(entry.file_size);
    if (!bundle) return -1;
    {
        uint32_t current = cluster;
        uint32_t cluster_bytes = vol.sectors_per_cluster * FAT32_SECTOR_SIZE;
        uint8_t *temp = (uint8_t *)kmalloc(cluster_bytes);
        uint64_t remaining = entry.file_size;
        uint64_t written = 0;
        if (!temp) {
            kfree(bundle);
            return -1;
        }
        while (current >= 2 && current < FAT32_EOC && remaining > 0) {
            uint32_t take = remaining > cluster_bytes ? cluster_bytes : (uint32_t)remaining;
            if (fat32_read_sectors(&vol, fat32_cluster_lba(&vol, current), vol.sectors_per_cluster, temp) != 0) {
                kfree(temp);
                kfree(bundle);
                return -1;
            }
            copy_bytes(bundle + written, (const char *)temp, take);
            written += take;
            remaining -= take;
            current = fat32_get_fat_entry(&vol, current);
        }
        kfree(temp);
        if (remaining != 0) {
            kfree(bundle);
            return -1;
        }
    }
    *bundle_out = bundle;
    *size_out = entry.file_size;

    if (swap_partition_index_out && fat32_find_dirent_named(&vol, vol.root_cluster, ICDA_ROOT_CFG_NAME, 0, 0, &entry) == 0) {
        cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
        if (entry.file_size > 0 && cluster >= 2) {
            cfg = (char *)kmalloc(entry.file_size + 1);
            if (!cfg) return 0;
            {
                uint32_t current = cluster;
                uint32_t cluster_bytes = vol.sectors_per_cluster * FAT32_SECTOR_SIZE;
                uint8_t *temp = (uint8_t *)kmalloc(cluster_bytes);
                uint64_t remaining = entry.file_size;
                uint64_t written = 0;
                if (!temp) {
                    kfree(cfg);
                    return 0;
                }
                while (current >= 2 && current < FAT32_EOC && remaining > 0) {
                    uint32_t take = remaining > cluster_bytes ? cluster_bytes : (uint32_t)remaining;
                    if (fat32_read_sectors(&vol, fat32_cluster_lba(&vol, current), vol.sectors_per_cluster, temp) != 0) {
                        break;
                    }
                    copy_bytes(cfg + written, (const char *)temp, take);
                    written += take;
                    remaining -= take;
                    current = fat32_get_fat_entry(&vol, current);
                }
                kfree(temp);
                cfg_size = written;
            }
            cfg[cfg_size] = '\0';
            if (cfg_size >= 6 && cfg[0] == 's' && cfg[1] == 'w' && cfg[2] == 'a' && cfg[3] == 'p' && cfg[4] == '=') {
                int32_t value = -1;
                uint64_t parsed = 0;
                if (cfg[5] == '-' && cfg[6] == '1') {
                    value = -1;
                } else if (parse_uint64(cfg + 5, &parsed)) {
                    value = (int32_t)parsed;
                }
                *swap_partition_index_out = value;
            }
            kfree(cfg);
        }
    }
    return 0;
}

static int system_install_core(uint64_t *files_installed, uint64_t *bytes_installed) {
    uint64_t count = initramfs_file_count();
    uint64_t installable_count = 0;
    uint64_t total_bytes = 0;
    uint64_t installed_files = 0;
    uint64_t manifest_cap = 512;
    uint64_t out = 0;
    char *manifest = 0;
    char status[256];

    if (ensure_layout() != 0) {
        return -21;
    }

    manifest = (char *)kmalloc((size_t)manifest_cap);
    if (!manifest) {
        return -22;
    }
    manifest[0] = '\0';
    out = append_text(manifest, out, manifest_cap, "installed files:\n");

    for (uint64_t i = 0; i < count; i++) {
        const initramfs_file_t *file = initramfs_file_at(i);
        if (file && file->path && !str_prefix(file->path, "/usr/share/audio/")) {
            installable_count++;
        }
    }
    if (installable_count == 0) {
        installable_count = 1;
    }

    for (uint64_t i = 0; i < count; i++) {
        const initramfs_file_t *file = initramfs_file_at(i);
        uint64_t needed;
        if (!file || !file->path) {
            kfree(manifest);
            return -23;
        }
        if (str_prefix(file->path, "/usr/share/audio/")) {
            continue;
        }

        if (vfs_import_node(file->path, VFS_NODE_FILE, 0, file->data ? file->data : "", file->size, 0, 0, 0) != 0) {
            kfree(manifest);
            return -24;
        }
        installed_files++;
        total_bytes += file->size;
        install_progress("Preparing system", file->path, installed_files, installable_count);

        needed = str_len(file->path) + 32;
        while (out + needed + 1 >= manifest_cap) {
            char *next;
            uint64_t next_cap = manifest_cap * 2;
            next = (char *)kmalloc((size_t)next_cap);
            if (!next) {
                kfree(manifest);
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
        return -26;
    }
    if (vfs_write(vfs_root(), "/system/install/manifest.txt", manifest, str_len(manifest)) != 0) {
        kfree(manifest);
        return -27;
    }
    kfree(manifest);

    if (vfs_sync() != 0) {
        return -28;
    }

    if (files_installed) {
        *files_installed = installed_files;
    }
    if (bytes_installed) {
        *bytes_installed = total_bytes;
    }
    return 0;
}

int system_install_present(void) {
    vfs_stat_t st;
    return vfs_stat(vfs_root(), "/system/install/state.txt", &st) == 0;
}

int system_install_run(uint64_t *files_installed, uint64_t *bytes_installed) {
    return system_install_core(files_installed, bytes_installed);
}

int system_install_partitions(uint32_t efi_partition_index, uint32_t root_partition_index, int32_t swap_partition_index,
                              uint64_t *files_installed, uint64_t *bytes_installed) {
    const partition_info_t *boot_part = partition_get(efi_partition_index);
    const partition_info_t *root_part = partition_get(root_partition_index);
    const partition_info_t *swap_part = swap_partition_index >= 0 ? partition_get((uint32_t)swap_partition_index) : 0;
    uint64_t files = 0;
    uint64_t bytes = 0;
    char *bundle = 0;
    uint64_t bundle_size = 0;
    uint64_t entry_count = 0;
    uint64_t total_bytes_written = 0;
    uint64_t total_files_written = 0;

    if (!boot_part || !root_part) {
        return -10;
    }
    if (boot_part->fs_hint != PARTITION_FS_FAT32 || root_part->fs_hint != PARTITION_FS_FAT32) {
        return -11;
    }
    if (efi_partition_index == root_partition_index ||
        (swap_partition_index >= 0 && ((uint32_t)swap_partition_index == efi_partition_index || (uint32_t)swap_partition_index == root_partition_index))) {
        return -12;
    }
    if (swap_partition_index >= 0 && !swap_part) {
        return -13;
    }
    (void)diskfmt_set_partition_role(efi_partition_index, PARTITION_ROLE_EFI);
    (void)diskfmt_set_partition_role(root_partition_index, PARTITION_ROLE_SYSTEM);
    if (swap_partition_index >= 0) {
        (void)diskfmt_set_partition_role((uint32_t)swap_partition_index, PARTITION_ROLE_SWAP);
    }
    install_progress("Preparing system", "Seeding installed files", 0, 1);
    if (system_install_core(&files, &bytes) != 0) {
        return -14;
    }
    total_files_written += files;
    total_bytes_written += bytes;
    install_progress("Preparing root partition", "Packing ICDA system bundle", 0, 1);
    if (persistfs_export_image(&bundle, &bundle_size, &entry_count) != 0) {
        return -15;
    }
    if (system_install_write_root_bundle(root_part, bundle, bundle_size, swap_partition_index) != 0) {
        kfree(bundle);
        return -16;
    }
    total_files_written += 2;
    total_bytes_written += bundle_size;
    total_bytes_written += 16;
    kfree(bundle);
    install_progress("Preparing boot disk", "Writing EFI boot files", 0, 1);
    if (fat32_install_boot_partition(boot_part) != 0) {
        return -17;
    }
    total_files_written += 4;
    total_bytes_written += (uint64_t)boot_asset_efi_size();
    total_bytes_written += (uint64_t)boot_asset_grub_cfg_size();
    total_bytes_written += (uint64_t)boot_asset_kernel_bin_size();
    total_bytes_written += 23;
    install_progress("Finalizing install", "Syncing installed state", 0, 1);
    install_progress("Finalizing install", "Done", 1, 1);
    install_progress_done();
    if (files_installed) {
        *files_installed = total_files_written;
    }
    if (bytes_installed) {
        *bytes_installed = total_bytes_written;
    }
    return 0;
}

int system_install_device(uint32_t device_index, uint64_t *files_installed, uint64_t *bytes_installed) {
    block_device_t *dev = block_get(device_index);
    uint32_t efi_partition = UINT32_MAX;
    uint32_t root_partition = UINT32_MAX;
    int32_t swap_partition = -1;

    if (!dev) {
        return -10;
    }
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        if (!part || part->device != dev) continue;
        if (part->role == PARTITION_ROLE_EFI && efi_partition == UINT32_MAX) {
            efi_partition = i;
        } else if (part->role == PARTITION_ROLE_SYSTEM && root_partition == UINT32_MAX) {
            root_partition = i;
        } else if (part->role == PARTITION_ROLE_SWAP && swap_partition < 0) {
            swap_partition = (int32_t)i;
        }
    }
    if (efi_partition == UINT32_MAX || root_partition == UINT32_MAX) {
        return -11;
    }
    return system_install_partitions(efi_partition, root_partition, swap_partition, files_installed, bytes_installed);
}
