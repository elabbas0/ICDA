#include "persistfs.h"
#include "vfs.h"
#include "../drivers/storage/ata.h"
#include "../memory/heap.h"

#define PERSISTFS_MAGIC   0x31534641444349ULL
#define PERSISTFS_VERSION 1U
#define PERSISTFS_SECTOR_SIZE 512U
#define PERSISTFS_PATH_CAP 256U

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t bytes_used;
    uint32_t reserved;
} __attribute__((packed)) persistfs_header_t;

typedef struct {
    uint16_t path_len;
    uint8_t type;
    uint8_t readonly;
    uint32_t data_size;
    uint64_t inode;
    uint64_t created;
    uint64_t modified;
} __attribute__((packed)) persistfs_entry_t;

static int persistfs_available = 0;
static uint64_t persistfs_entries_loaded = 0;

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
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

static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static int path_push(char *path, uint64_t cap, const char *name) {
    uint64_t len = str_len(path);
    uint64_t part = str_len(name);

    if (len == 0) {
        if (cap < 2 + part) {
            return -1;
        }
        path[0] = '/';
        copy_bytes(path + 1, name, part);
        path[1 + part] = '\0';
        return 0;
    }

    if (len + 1 + part + 1 > cap) {
        return -1;
    }
    path[len] = '/';
    copy_bytes(path + len + 1, name, part);
    path[len + 1 + part] = '\0';
    return 0;
}

static void path_pop(char *path, uint64_t old_len) {
    path[old_len] = '\0';
}

static uint64_t persistfs_measure_node(vfs_node_t *node, char *path, uint64_t cap, uint32_t *entry_count) {
    uint64_t total = 0;
    uint64_t old_len = str_len(path);

    if (!node) {
        return 0;
    }
    if (path_push(path, cap, vfs_node_name(node)) != 0) {
        return 0;
    }

    if (!vfs_node_readonly(node)) {
        uint64_t path_len = str_len(path);
        total += sizeof(persistfs_entry_t) + path_len;
        if (vfs_node_type(node) == VFS_NODE_FILE) {
            total += vfs_node_size(node);
        }
        (*entry_count)++;
    }

    if (vfs_node_type(node) == VFS_NODE_DIR) {
        uint64_t count = vfs_child_count(node);
        for (uint64_t i = 0; i < count; i++) {
            total += persistfs_measure_node(vfs_child_at(node, i), path, cap, entry_count);
        }
    }

    path_pop(path, old_len);
    return total;
}

static uint8_t *persistfs_write_node(uint8_t *cursor, vfs_node_t *node, char *path, uint64_t cap) {
    uint64_t old_len = str_len(path);

    if (!node) {
        return cursor;
    }
    if (path_push(path, cap, vfs_node_name(node)) != 0) {
        return 0;
    }

    if (!vfs_node_readonly(node)) {
        persistfs_entry_t entry;
        uint64_t path_len = str_len(path);

        entry.path_len = (uint16_t)path_len;
        entry.type = vfs_node_type(node);
        entry.readonly = vfs_node_readonly(node);
        entry.data_size = (uint32_t)(vfs_node_type(node) == VFS_NODE_FILE ? vfs_node_size(node) : 0);
        entry.inode = vfs_node_inode(node);
        entry.created = vfs_node_created(node);
        entry.modified = vfs_node_modified(node);

        copy_bytes((char *)cursor, (const char *)&entry, sizeof(entry));
        cursor += sizeof(entry);
        copy_bytes((char *)cursor, path, path_len);
        cursor += path_len;
        if (entry.data_size) {
            copy_bytes((char *)cursor, vfs_node_data(node), entry.data_size);
            cursor += entry.data_size;
        }
    }

    if (vfs_node_type(node) == VFS_NODE_DIR) {
        uint64_t count = vfs_child_count(node);
        for (uint64_t i = 0; i < count; i++) {
            cursor = persistfs_write_node(cursor, vfs_child_at(node, i), path, cap);
            if (!cursor) {
                return 0;
            }
        }
    }

    path_pop(path, old_len);
    return cursor;
}

int persistfs_sync(void) {
    uint32_t entry_count = 0;
    uint64_t payload_bytes;
    uint64_t total_bytes;
    uint64_t sectors;
    uint8_t *buffer;
    uint8_t *cursor;
    char path[PERSISTFS_PATH_CAP];
    persistfs_header_t *header;

    if (!persistfs_available) {
        return -1;
    }

    path[0] = '\0';
    payload_bytes = 0;
    {
        uint64_t root_children = vfs_child_count(vfs_root());
        for (uint64_t i = 0; i < root_children; i++) {
            payload_bytes += persistfs_measure_node(vfs_child_at(vfs_root(), i), path, sizeof(path), &entry_count);
        }
    }

    total_bytes = sizeof(persistfs_header_t) + payload_bytes;
    sectors = align_up(total_bytes, PERSISTFS_SECTOR_SIZE) / PERSISTFS_SECTOR_SIZE;
    if (sectors == 0) {
        sectors = 1;
    }
    if (sectors > ata_sector_count()) {
        return -1;
    }

    buffer = (uint8_t *)kmalloc((size_t)(sectors * PERSISTFS_SECTOR_SIZE));
    if (!buffer) {
        return -1;
    }
    zero_bytes(buffer, sectors * PERSISTFS_SECTOR_SIZE);

    header = (persistfs_header_t *)buffer;
    header->magic = PERSISTFS_MAGIC;
    header->version = PERSISTFS_VERSION;
    header->entry_count = entry_count;
    header->bytes_used = (uint32_t)total_bytes;
    header->reserved = 0;

    cursor = buffer + sizeof(persistfs_header_t);
    path[0] = '\0';
    {
        uint64_t root_children = vfs_child_count(vfs_root());
        for (uint64_t i = 0; i < root_children; i++) {
            cursor = persistfs_write_node(cursor, vfs_child_at(vfs_root(), i), path, sizeof(path));
            if (!cursor) {
                kfree(buffer);
                return -1;
            }
        }
    }

    if (ata_write_sectors(0, (uint8_t)sectors, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    kfree(buffer);
    return 0;
}

int persistfs_init(void) {
    uint8_t sector[PERSISTFS_SECTOR_SIZE];
    persistfs_header_t *header = (persistfs_header_t *)sector;
    uint8_t *buffer = 0;
    uint64_t sectors;
    uint8_t *cursor;

    persistfs_available = 0;
    persistfs_entries_loaded = 0;

    if (ata_init() != 0 || !ata_present()) {
        return -1;
    }
    persistfs_available = 1;

    if (ata_read_sectors(0, 1, sector) != 0) {
        return -1;
    }

    if (header->magic != PERSISTFS_MAGIC || header->version != PERSISTFS_VERSION ||
        header->bytes_used < sizeof(persistfs_header_t)) {
        vfs_set_sync_hook(persistfs_sync);
        persistfs_entries_loaded = 0;
        return persistfs_sync();
    }

    sectors = align_up(header->bytes_used, PERSISTFS_SECTOR_SIZE) / PERSISTFS_SECTOR_SIZE;
    if (sectors == 0 || sectors > ata_sector_count()) {
        return -1;
    }

    buffer = (uint8_t *)kmalloc((size_t)(sectors * PERSISTFS_SECTOR_SIZE));
    if (!buffer) {
        return -1;
    }
    if (ata_read_sectors(0, (uint8_t)sectors, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    header = (persistfs_header_t *)buffer;
    cursor = buffer + sizeof(persistfs_header_t);
    for (uint32_t i = 0; i < header->entry_count; i++) {
        persistfs_entry_t entry;
        char path[PERSISTFS_PATH_CAP];
        const char *data = 0;

        if ((uint64_t)(cursor - buffer) + sizeof(entry) > header->bytes_used) {
            kfree(buffer);
            return -1;
        }
        copy_bytes((char *)&entry, (const char *)cursor, sizeof(entry));
        cursor += sizeof(entry);
        if (entry.path_len == 0 || entry.path_len >= sizeof(path)) {
            kfree(buffer);
            return -1;
        }
        if ((uint64_t)(cursor - buffer) + entry.path_len + entry.data_size > header->bytes_used) {
            kfree(buffer);
            return -1;
        }
        copy_bytes(path, (const char *)cursor, entry.path_len);
        path[entry.path_len] = '\0';
        cursor += entry.path_len;
        if (entry.type == VFS_NODE_FILE) {
            data = (const char *)cursor;
            cursor += entry.data_size;
        }

        if (vfs_import_node(path, entry.type, entry.readonly, data, entry.data_size,
                            entry.inode, entry.created, entry.modified) != 0) {
            kfree(buffer);
            return -1;
        }
        persistfs_entries_loaded++;
    }

    kfree(buffer);
    vfs_set_sync_hook(persistfs_sync);
    return 0;
}

int persistfs_present(void) {
    return persistfs_available;
}

uint64_t persistfs_loaded_entries(void) {
    return persistfs_entries_loaded;
}
