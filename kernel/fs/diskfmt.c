#include "diskfmt.h"

#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "fat32.h"
#include "exfat.h"
#include "ntfs.h"
#include "persistfs.h"
#include "../memory/heap.h"

#define DISK_SECTOR_SIZE 512U
#define DISK_ALIGN_LBA 2048U
#define GPT_ENTRY_COUNT 128U
#define GPT_ENTRY_SIZE 128U
#define GPT_ENTRIES_SECTORS ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) / DISK_SECTOR_SIZE)

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

static void zero_bytes(uint8_t *dst, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) dst[i] = 0;
}

static void copy_bytes(char *dst, const char *src, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) dst[i] = src[i];
}

static uint32_t crc32_bytes(const uint8_t *data, uint32_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static void fill_guid(uint8_t guid[16], uint32_t seed) {
    for (uint32_t i = 0; i < 16; i++) {
        guid[i] = (uint8_t)((seed + i * 37U) & 0xFFU);
        seed = seed * 1664525U + 1013904223U;
    }
}

static void utf16_name(uint16_t out[36], const char *text) {
    uint32_t i = 0;
    while (i < 36) {
        out[i] = 0;
        i++;
    }
    i = 0;
    while (text && text[i] && i < 36) {
        out[i] = (uint16_t)(uint8_t)text[i];
        i++;
    }
}

static void zero_region(block_device_t *dev, uint64_t start_lba, uint32_t sectors) {
    uint8_t zeros[DISK_SECTOR_SIZE * 8];
    zero_bytes(zeros, sizeof(zeros));
    while (sectors) {
        uint32_t chunk = sectors > 8 ? 8 : sectors;
        (void)dev->write(dev->context, start_lba, chunk, zeros);
        start_lba += chunk;
        sectors -= chunk;
    }
}

static void write_mbr_partition(uint8_t *mbr, uint8_t type, uint32_t start_lba, uint32_t sector_count) {
    uint8_t *entry = mbr + 446;
    zero_bytes(entry, 64);
    entry[0] = 0x00;
    entry[1] = 0x20;
    entry[2] = 0x21;
    entry[3] = 0x00;
    entry[4] = type;
    entry[5] = 0xFE;
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    entry[8] = (uint8_t)(start_lba & 0xFF);
    entry[9] = (uint8_t)((start_lba >> 8) & 0xFF);
    entry[10] = (uint8_t)((start_lba >> 16) & 0xFF);
    entry[11] = (uint8_t)((start_lba >> 24) & 0xFF);
    entry[12] = (uint8_t)(sector_count & 0xFF);
    entry[13] = (uint8_t)((sector_count >> 8) & 0xFF);
    entry[14] = (uint8_t)((sector_count >> 16) & 0xFF);
    entry[15] = (uint8_t)((sector_count >> 24) & 0xFF);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

static void write_mbr_partition_slot(uint8_t *mbr, uint32_t slot, uint8_t type, uint32_t start_lba, uint32_t sector_count) {
    uint8_t *entry;
    if (slot >= 4) return;
    entry = mbr + 446 + slot * 16U;
    entry[0] = 0x00;
    entry[1] = 0x20;
    entry[2] = 0x21;
    entry[3] = 0x00;
    entry[4] = type;
    entry[5] = 0xFE;
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    entry[8] = (uint8_t)(start_lba & 0xFF);
    entry[9] = (uint8_t)((start_lba >> 8) & 0xFF);
    entry[10] = (uint8_t)((start_lba >> 16) & 0xFF);
    entry[11] = (uint8_t)((start_lba >> 24) & 0xFF);
    entry[12] = (uint8_t)(sector_count & 0xFF);
    entry[13] = (uint8_t)((sector_count >> 8) & 0xFF);
    entry[14] = (uint8_t)((sector_count >> 16) & 0xFF);
    entry[15] = (uint8_t)((sector_count >> 24) & 0xFF);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

static void write_protective_mbr(uint8_t *mbr, uint64_t sector_count) {
    uint32_t count32 = sector_count > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)(sector_count - 1U);
    zero_bytes(mbr, DISK_SECTOR_SIZE);
    write_mbr_partition_slot(mbr, 0, 0xEE, 1, count32);
}

static int write_gpt_layout(block_device_t *dev, const gpt_entry_t *entries, uint32_t entry_count) {
    uint8_t sector[DISK_SECTOR_SIZE];
    gpt_header_t *primary = (gpt_header_t *)sector;
    gpt_header_t backup;
    uint8_t *entry_bytes;
    uint64_t backup_header_lba;
    uint64_t backup_entries_lba;
    uint64_t first_usable_lba = 34;
    uint64_t last_usable_lba;
    uint32_t entries_crc;

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -30;
    if (dev->sector_count <= (GPT_ENTRIES_SECTORS * 2U + 34U)) return -31;

    backup_header_lba = dev->sector_count - 1U;
    backup_entries_lba = backup_header_lba - GPT_ENTRIES_SECTORS;
    last_usable_lba = backup_entries_lba - 1U;
    entry_bytes = (uint8_t *)kmalloc(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE);
    if (!entry_bytes) return -37;

    zero_region(dev, 0, 128);
    zero_bytes(entry_bytes, GPT_ENTRY_COUNT * GPT_ENTRY_SIZE);
    for (uint32_t i = 0; i < entry_count && i < GPT_ENTRY_COUNT; i++) {
        copy_bytes((char *)(entry_bytes + i * GPT_ENTRY_SIZE), (const char *)&entries[i], sizeof(gpt_entry_t));
    }
    entries_crc = crc32_bytes(entry_bytes, GPT_ENTRY_COUNT * GPT_ENTRY_SIZE);

    write_protective_mbr(sector, dev->sector_count);
    if (dev->write(dev->context, 0, 1, sector) != 0) {
        kfree(entry_bytes);
        return -32;
    }
    if (dev->write(dev->context, 2, GPT_ENTRIES_SECTORS, entry_bytes) != 0) {
        kfree(entry_bytes);
        return -33;
    }
    if (dev->write(dev->context, backup_entries_lba, GPT_ENTRIES_SECTORS, entry_bytes) != 0) {
        kfree(entry_bytes);
        return -34;
    }

    zero_bytes((uint8_t *)primary, DISK_SECTOR_SIZE);
    primary->signature[0] = 'E'; primary->signature[1] = 'F'; primary->signature[2] = 'I'; primary->signature[3] = ' ';
    primary->signature[4] = 'P'; primary->signature[5] = 'A'; primary->signature[6] = 'R'; primary->signature[7] = 'T';
    primary->revision = 0x00010000U;
    primary->header_size = 92U;
    primary->current_lba = 1U;
    primary->backup_lba = backup_header_lba;
    primary->first_usable_lba = first_usable_lba;
    primary->last_usable_lba = last_usable_lba;
    fill_guid(primary->disk_guid, (uint32_t)dev->sector_count);
    primary->partition_entries_lba = 2U;
    primary->partition_entry_count = GPT_ENTRY_COUNT;
    primary->partition_entry_size = GPT_ENTRY_SIZE;
    primary->partition_entries_crc32 = entries_crc;
    primary->header_crc32 = 0;
    primary->header_crc32 = crc32_bytes((const uint8_t *)primary, primary->header_size);
    if (dev->write(dev->context, 1, 1, primary) != 0) {
        kfree(entry_bytes);
        return -35;
    }

    copy_bytes((char *)&backup, (const char *)primary, sizeof(backup));
    backup.current_lba = backup_header_lba;
    backup.backup_lba = 1U;
    backup.partition_entries_lba = backup_entries_lba;
    backup.header_crc32 = 0;
    backup.header_crc32 = crc32_bytes((const uint8_t *)&backup, backup.header_size);
    zero_bytes(sector, DISK_SECTOR_SIZE);
    copy_bytes((char *)sector, (const char *)&backup, sizeof(backup));
    if (dev->write(dev->context, backup_header_lba, 1, sector) != 0) {
        kfree(entry_bytes);
        return -36;
    }
    kfree(entry_bytes);
    return 0;
}

static uint8_t fat32_sectors_per_cluster(uint32_t total_sectors) {
    if (total_sectors < 262144U) return 1;
    if (total_sectors < 1048576U) return 4;
    if (total_sectors < 8388608U) return 8;
    return 16;
}

static uint32_t fat32_calc_fat_sectors(uint32_t total_sectors, uint8_t spc, uint16_t reserved, uint8_t fats) {
    uint32_t fat_sectors = 1;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t data_sectors = total_sectors - reserved - fats * fat_sectors;
        uint32_t clusters = data_sectors / spc;
        uint32_t next = ((clusters + 2U) * 4U + (DISK_SECTOR_SIZE - 1U)) / DISK_SECTOR_SIZE;
        if (next == fat_sectors) break;
        fat_sectors = next;
    }
    return fat_sectors;
}

static int diskfmt_format_fat32_partition(block_device_t *dev, uint32_t part_start, uint32_t part_sectors) {
    uint8_t spc;
    uint16_t reserved = 32;
    uint8_t fats = 2;
    uint32_t fat_sectors;
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint64_t first_fat_lba;
    uint64_t second_fat_lba;
    uint64_t root_lba;
    uint8_t sector[DISK_SECTOR_SIZE];
    uint8_t fat[DISK_SECTOR_SIZE];

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -1;
    if (part_sectors <= 65536U) return -1;
    spc = fat32_sectors_per_cluster(part_sectors);
    fat_sectors = fat32_calc_fat_sectors(part_sectors, spc, reserved, fats);
    data_sectors = part_sectors - reserved - fats * fat_sectors;
    cluster_count = data_sectors / spc;
    if (cluster_count < 65525U) return -1;

    zero_bytes(sector, sizeof(sector));
    sector[0] = 0xEB; sector[1] = 0x58; sector[2] = 0x90;
    sector[3] = 'M'; sector[4] = 'S'; sector[5] = 'W'; sector[6] = 'I'; sector[7] = 'N'; sector[8] = '4'; sector[9] = '.'; sector[10] = '1';
    sector[11] = 0x00; sector[12] = 0x02;
    sector[13] = spc;
    sector[14] = (uint8_t)(reserved & 0xFF); sector[15] = (uint8_t)(reserved >> 8);
    sector[16] = fats;
    sector[21] = 0xF8;
    sector[24] = 0x3F; sector[25] = 0x00;
    sector[26] = 0xFF; sector[27] = 0x00;
    sector[28] = (uint8_t)(part_start & 0xFF);
    sector[29] = (uint8_t)((part_start >> 8) & 0xFF);
    sector[30] = (uint8_t)((part_start >> 16) & 0xFF);
    sector[31] = (uint8_t)((part_start >> 24) & 0xFF);
    sector[32] = (uint8_t)(part_sectors & 0xFF);
    sector[33] = (uint8_t)((part_sectors >> 8) & 0xFF);
    sector[34] = (uint8_t)((part_sectors >> 16) & 0xFF);
    sector[35] = (uint8_t)((part_sectors >> 24) & 0xFF);
    sector[36] = (uint8_t)(fat_sectors & 0xFF);
    sector[37] = (uint8_t)((fat_sectors >> 8) & 0xFF);
    sector[38] = (uint8_t)((fat_sectors >> 16) & 0xFF);
    sector[39] = (uint8_t)((fat_sectors >> 24) & 0xFF);
    sector[44] = 0x02;
    sector[48] = 0x01;
    sector[50] = 0x06;
    sector[64] = 0x80;
    sector[66] = 0x29;
    sector[67] = 0x34; sector[68] = 0x12; sector[69] = 0xCD; sector[70] = 0xAB;
    sector[71] = 0x49; sector[72] = 0x43; sector[73] = 0x44; sector[74] = 0x41;
    sector[75] = 0x20; sector[76] = 0x44; sector[77] = 0x49; sector[78] = 0x53; sector[79] = 0x4B; sector[80] = 0x20; sector[81] = 0x20;
    sector[82] = 'F'; sector[83] = 'A'; sector[84] = 'T'; sector[85] = '3'; sector[86] = '2'; sector[87] = ' '; sector[88] = ' '; sector[89] = ' ';
    sector[510] = 0x55; sector[511] = 0xAA;
    if (dev->write(dev->context, part_start, 1, sector) != 0) return -1;
    if (dev->write(dev->context, part_start + 6, 1, sector) != 0) return -1;

    zero_bytes(sector, sizeof(sector));
    sector[0] = 0x52; sector[1] = 0x52; sector[2] = 0x61; sector[3] = 0x41;
    sector[484] = 0x72; sector[485] = 0x72; sector[486] = 0x41; sector[487] = 0x61;
    sector[488] = 0xFF; sector[489] = 0xFF; sector[490] = 0xFF; sector[491] = 0xFF;
    sector[492] = 0x03;
    sector[510] = 0x55; sector[511] = 0xAA;
    if (dev->write(dev->context, part_start + 1, 1, sector) != 0) return -1;
    if (dev->write(dev->context, part_start + 7, 1, sector) != 0) return -1;

    first_fat_lba = part_start + reserved;
    second_fat_lba = first_fat_lba + fat_sectors;
    root_lba = part_start + reserved + (uint64_t)fats * fat_sectors;

    zero_region(dev, first_fat_lba, fat_sectors);
    zero_region(dev, second_fat_lba, fat_sectors);
    zero_region(dev, root_lba, spc);

    zero_bytes(fat, sizeof(fat));
    fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; fat[3] = 0x0F;
    fat[4] = 0xFF; fat[5] = 0xFF; fat[6] = 0xFF; fat[7] = 0x0F;
    fat[8] = 0xFF; fat[9] = 0xFF; fat[10] = 0xFF; fat[11] = 0x0F;
    if (dev->write(dev->context, first_fat_lba, 1, fat) != 0) return -1;
    if (dev->write(dev->context, second_fat_lba, 1, fat) != 0) return -1;

    return 0;
}

static int diskfmt_format_fat32(block_device_t *dev) {
    uint32_t part_start;
    uint32_t part_sectors;
    uint8_t sector[DISK_SECTOR_SIZE];

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -3;
    if (dev->sector_count <= DISK_ALIGN_LBA + 65536U) return -3;
    if (dev->sector_count > 0xFFFFFFFFULL) return -3;

    part_start = DISK_ALIGN_LBA;
    part_sectors = (uint32_t)dev->sector_count - part_start;

    zero_region(dev, 0, 64);
    zero_bytes(sector, sizeof(sector));
    write_mbr_partition(sector, 0x0C, part_start, part_sectors);
    if (dev->write(dev->context, 0, 1, sector) != 0) return -4;
    if (diskfmt_format_fat32_partition(dev, part_start, part_sectors) != 0) return -5;
    return 0;
}

static int diskfmt_format_exfat_partition(block_device_t *dev, uint32_t part_start, uint32_t part_sectors) {
    uint8_t sector[DISK_SECTOR_SIZE];
    uint32_t fat_offset = 128;
    uint32_t fat_length = 128;
    uint32_t cluster_heap_offset = 256;
    uint32_t cluster_count;

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -1;
    if (part_sectors <= cluster_heap_offset + 1024U) return -1;
    cluster_count = (part_sectors - cluster_heap_offset) / 8U;

    zero_region(dev, part_start, 32);
    zero_bytes(sector, sizeof(sector));
    sector[0] = 0xEB; sector[1] = 0x76; sector[2] = 0x90;
    sector[3] = 'E'; sector[4] = 'X'; sector[5] = 'F'; sector[6] = 'A'; sector[7] = 'T'; sector[8] = ' '; sector[9] = ' '; sector[10] = ' ';
    sector[64] = (uint8_t)(part_start & 0xFF);
    sector[65] = (uint8_t)((part_start >> 8) & 0xFF);
    sector[66] = (uint8_t)((part_start >> 16) & 0xFF);
    sector[67] = (uint8_t)((part_start >> 24) & 0xFF);
    sector[72] = (uint8_t)(part_sectors & 0xFF);
    sector[73] = (uint8_t)((part_sectors >> 8) & 0xFF);
    sector[74] = (uint8_t)((part_sectors >> 16) & 0xFF);
    sector[75] = (uint8_t)((part_sectors >> 24) & 0xFF);
    sector[80] = (uint8_t)(fat_offset & 0xFF);
    sector[81] = (uint8_t)((fat_offset >> 8) & 0xFF);
    sector[84] = (uint8_t)(fat_length & 0xFF);
    sector[85] = (uint8_t)((fat_length >> 8) & 0xFF);
    sector[88] = (uint8_t)(cluster_heap_offset & 0xFF);
    sector[89] = (uint8_t)((cluster_heap_offset >> 8) & 0xFF);
    sector[92] = (uint8_t)(cluster_count & 0xFF);
    sector[93] = (uint8_t)((cluster_count >> 8) & 0xFF);
    sector[94] = (uint8_t)((cluster_count >> 16) & 0xFF);
    sector[95] = (uint8_t)((cluster_count >> 24) & 0xFF);
    sector[96] = 0x02;
    sector[100] = 0x78; sector[101] = 0x56; sector[102] = 0x34; sector[103] = 0x12;
    sector[104] = 0x00; sector[105] = 0x01;
    sector[108] = 9;
    sector[109] = 3;
    sector[110] = 1;
    sector[111] = 0x80;
    sector[112] = 0xFF;
    sector[510] = 0x55; sector[511] = 0xAA;
    if (dev->write(dev->context, part_start, 1, sector) != 0) return -1;
    return 0;
}

static int diskfmt_format_exfat(block_device_t *dev) {
    uint32_t part_start;
    uint32_t part_sectors;
    uint8_t sector[DISK_SECTOR_SIZE];

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -13;
    if (dev->sector_count <= DISK_ALIGN_LBA + 131072U) return -13;
    if (dev->sector_count > 0xFFFFFFFFULL) return -13;

    part_start = DISK_ALIGN_LBA;
    part_sectors = (uint32_t)dev->sector_count - part_start;

    zero_region(dev, 0, 64);
    zero_bytes(sector, sizeof(sector));
    write_mbr_partition(sector, 0x07, part_start, part_sectors);
    if (dev->write(dev->context, 0, 1, sector) != 0) return -14;
    if (diskfmt_format_exfat_partition(dev, part_start, part_sectors) != 0) return -15;
    return 0;
}

static int diskfmt_clear_device(block_device_t *dev) {
    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -19;
    zero_region(dev, 0, 64);
    return 0;
}

static int diskfmt_init_mbr(block_device_t *dev) {
    uint8_t sector[DISK_SECTOR_SIZE];
    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -40;
    zero_region(dev, 0, 64);
    zero_bytes(sector, sizeof(sector));
    sector[510] = 0x55;
    sector[511] = 0xAA;
    return dev->write(dev->context, 0, 1, sector) == 0 ? 0 : -41;
}

static int diskfmt_init_gpt(block_device_t *dev) {
    gpt_entry_t *entries;
    int rc;

    entries = (gpt_entry_t *)kmalloc(sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);
    if (!entries) return -42;
    zero_bytes((uint8_t *)entries, sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);
    rc = write_gpt_layout(dev, entries, 0);
    kfree(entries);
    return rc;
}

static int diskfmt_layout_icda(block_device_t *dev) {
    uint32_t efi_start;
    uint32_t efi_sectors = 524288U;
    uint32_t system_start;
    uint32_t system_sectors;
    gpt_entry_t *entries;
    int rc;
    static const uint8_t efi_type_guid[16] = { 0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
    static const uint8_t basic_type_guid[16] = { 0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };

    if (!dev || !dev->write || dev->sector_size != DISK_SECTOR_SIZE) return -21;
    if (dev->sector_count <= DISK_ALIGN_LBA + efi_sectors + 131072U + PERSISTFS_RESERVED_SECTORS) return -21;

    efi_start = DISK_ALIGN_LBA;
    system_start = efi_start + efi_sectors;
    if (system_start & 0x7U) {
        system_start = (system_start + 7U) & ~7U;
    }
    if ((uint64_t)system_start >= dev->sector_count) return -21;
    system_sectors = (uint32_t)dev->sector_count - system_start - PERSISTFS_RESERVED_SECTORS;
    if (system_sectors <= 131072U) return -21;

    entries = (gpt_entry_t *)kmalloc(sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);
    if (!entries) return -25;
    zero_bytes((uint8_t *)entries, sizeof(gpt_entry_t) * GPT_ENTRY_COUNT);
    copy_bytes((char *)entries[0].type_guid, (const char *)efi_type_guid, sizeof(efi_type_guid));
    fill_guid(entries[0].unique_guid, 0x1000U);
    entries[0].first_lba = efi_start;
    entries[0].last_lba = (uint64_t)efi_start + efi_sectors - 1U;
    utf16_name(entries[0].name, "ICDA EFI");
    copy_bytes((char *)entries[1].type_guid, (const char *)basic_type_guid, sizeof(basic_type_guid));
    fill_guid(entries[1].unique_guid, 0x2000U);
    entries[1].first_lba = system_start;
    entries[1].last_lba = (uint64_t)system_start + system_sectors - 1U;
    utf16_name(entries[1].name, "ICDA System");

    rc = write_gpt_layout(dev, entries, 2);
    if (rc == 0) rc = diskfmt_format_fat32_partition(dev, efi_start, efi_sectors) == 0 ? 0 : -23;
    if (rc == 0) rc = diskfmt_format_fat32_partition(dev, system_start, system_sectors) == 0 ? 0 : -24;
    if (rc == 0) rc = 0;
    else if (rc > -23) rc = -22;
    kfree(entries);
    return rc;
}

int diskfmt_format_device(uint32_t device_index, diskfmt_fs_t fs_type) {
    block_device_t *dev = block_get(device_index);
    int runtime_device = persistfs_active_device();
    int rc;
    if (!dev || !dev->write) return -10;
    if (runtime_device >= 0 && device_index == (uint32_t)runtime_device) return -11;

    if (fs_type == DISKFMT_FS_FAT32) {
        rc = diskfmt_format_fat32(dev);
    } else if (fs_type == DISKFMT_FS_EXFAT) {
        rc = diskfmt_format_exfat(dev);
    } else if (fs_type == DISKFMT_LAYOUT_CLEAR) {
        rc = diskfmt_clear_device(dev);
    } else if (fs_type == DISKFMT_LAYOUT_ICDA) {
        rc = diskfmt_layout_icda(dev);
    } else if (fs_type == DISKFMT_LAYOUT_MBR) {
        rc = diskfmt_init_mbr(dev);
    } else if (fs_type == DISKFMT_LAYOUT_GPT) {
        rc = diskfmt_init_gpt(dev);
    } else {
        return -12;
    }

    if (rc != 0) {
        return rc;
    }

    (void)partition_scan_all();
    (void)fat32_mount_detected();
    (void)exfat_mount_detected();
    (void)ntfs_mount_detected();
    return 0;
}
