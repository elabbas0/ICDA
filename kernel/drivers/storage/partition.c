#include "partition.h"

#define PARTITION_MAX 32
#define SECTOR_SIZE 512U

typedef struct {
    uint8_t boot;
    uint8_t chs_start[3];
    uint8_t type;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed)) gpt_entry_t;

typedef struct {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
} __attribute__((packed)) gpt_header_t;

static partition_info_t partitions[PARTITION_MAX];
static uint32_t partitions_found = 0;

static int str_eq8(const char *a, const char *b) {
    for (uint32_t i = 0; i < 8; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0 && b[i] == 0) return 1;
    }
    return 1;
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

static void append_u32(char *dst, uint32_t cap, uint32_t value) {
    char tmp[16];
    uint32_t len = 0;
    uint32_t out = 0;
    while (dst[out]) out++;
    if (value == 0) {
        if (out + 1 < cap) {
            dst[out++] = '0';
            dst[out] = 0;
        }
        return;
    }
    while (value && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len && out + 1 < cap) {
        dst[out++] = tmp[--len];
    }
    dst[out] = 0;
}

static partition_fs_hint_t detect_fs_hint(block_device_t *device, uint64_t start_lba) {
    uint8_t sector[SECTOR_SIZE];
    if (!device || !device->read) return PARTITION_FS_UNKNOWN;
    if (device->read(device->context, start_lba, 1, sector) != 0) return PARTITION_FS_UNKNOWN;

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return PARTITION_FS_UNKNOWN;
    }
    if (sector[3] == 'N' && sector[4] == 'T' && sector[5] == 'F' && sector[6] == 'S') {
        return PARTITION_FS_NTFS;
    }
    if (sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' && sector[6] == 'A' && sector[7] == 'T') {
        return PARTITION_FS_EXFAT;
    }
    if ((sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' && sector[85] == '3' && sector[86] == '2') ||
        (sector[54] == 'F' && sector[55] == 'A' && sector[56] == 'T')) {
        return PARTITION_FS_FAT32;
    }
    return PARTITION_FS_UNKNOWN;
}

static void add_partition(block_device_t *device, uint64_t start_lba, uint64_t sector_count, partition_kind_t kind, uint8_t mbr_type, const char *name) {
    partition_info_t *part;
    if (partitions_found >= PARTITION_MAX || !device || sector_count == 0) return;
    part = &partitions[partitions_found++];
    part->device = device;
    part->start_lba = start_lba;
    part->sector_count = sector_count;
    part->kind = kind;
    part->mbr_type = mbr_type;
    part->fs_hint = detect_fs_hint(device, start_lba);
    if (name && *name) {
        copy_text(part->name, name, sizeof(part->name));
    } else {
        copy_text(part->name, "part", sizeof(part->name));
        append_u32(part->name, sizeof(part->name), partitions_found - 1);
    }
}

static void scan_gpt(block_device_t *device) {
    uint8_t sector[SECTOR_SIZE];
    gpt_header_t *header = (gpt_header_t *)sector;
    uint32_t entry_size;
    uint32_t per_sector;
    uint8_t entry_sector[SECTOR_SIZE];

    if (device->read(device->context, 1, 1, sector) != 0) return;
    if (!str_eq8(header->signature, "EFI PART")) return;
    if (header->partition_entry_size < sizeof(gpt_entry_t) || header->partition_entry_count == 0) return;

    entry_size = header->partition_entry_size;
    per_sector = SECTOR_SIZE / entry_size;
    if (per_sector == 0) return;

    for (uint32_t i = 0; i < header->partition_entry_count && partitions_found < PARTITION_MAX; i++) {
        uint64_t lba = header->partition_entries_lba + (i / per_sector);
        uint32_t off = (i % per_sector) * entry_size;
        gpt_entry_t *entry;
        char name[48];
        uint32_t out = 0;

        if (device->read(device->context, lba, 1, entry_sector) != 0) return;
        entry = (gpt_entry_t *)(entry_sector + off);
        if (entry->first_lba == 0 || entry->last_lba < entry->first_lba) continue;
        if (entry->type_guid[0] == 0 && entry->type_guid[1] == 0 && entry->type_guid[2] == 0 && entry->type_guid[3] == 0) continue;

        for (uint32_t j = 0; j < 36 && out + 1 < sizeof(name); j++) {
            uint16_t ch = entry->name[j];
            if (ch == 0) break;
            name[out++] = (ch >= 32 && ch <= 126) ? (char)ch : '_';
        }
        name[out] = 0;
        add_partition(device, entry->first_lba, entry->last_lba - entry->first_lba + 1, PARTITION_KIND_GPT, 0, name);
    }
}

static void scan_mbr(block_device_t *device) {
    uint8_t sector[SECTOR_SIZE];
    mbr_entry_t *entries = (mbr_entry_t *)(sector + 446);
    int protective_gpt = 0;

    if (device->read(device->context, 0, 1, sector) != 0) return;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return;

    for (uint32_t i = 0; i < 4; i++) {
        if (entries[i].type == 0xEE) {
            protective_gpt = 1;
            break;
        }
    }
    if (protective_gpt) {
        scan_gpt(device);
        return;
    }

    for (uint32_t i = 0; i < 4 && partitions_found < PARTITION_MAX; i++) {
        char name[16];
        if (entries[i].type == 0 || entries[i].sector_count == 0) continue;
        copy_text(name, "mbr", sizeof(name));
        append_u32(name, sizeof(name), i);
        add_partition(device, entries[i].lba_start, entries[i].sector_count, PARTITION_KIND_MBR, entries[i].type, name);
    }
}

int partition_scan_all(void) {
    partitions_found = 0;
    for (uint32_t i = 0; i < block_count(); i++) {
        block_device_t *device = block_get(i);
        if (!device) continue;
        scan_mbr(device);
    }
    return 0;
}

uint32_t partition_count(void) {
    return partitions_found;
}

const partition_info_t *partition_get(uint32_t index) {
    if (index >= partitions_found) return 0;
    return &partitions[index];
}

const char *partition_fs_name(partition_fs_hint_t hint) {
    switch (hint) {
        case PARTITION_FS_FAT32: return "fat32";
        case PARTITION_FS_EXFAT: return "exfat";
        case PARTITION_FS_NTFS: return "ntfs";
        default: return "unknown";
    }
}
