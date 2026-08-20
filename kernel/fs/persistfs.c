#include "persistfs.h"
#include "install.h"
#include "vfs.h"
#include "../drivers/storage/ata.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
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
static int persistfs_live_boot = 0;
static int persistfs_active_device_index = -1;
static int persistfs_active_partition_index = -1;
static int persistfs_file_backend = 0;
static int persistfs_swap_partition_index = -1;

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

static uint32_t persistfs_start_lba_for_device(block_device_t *dev) {
    if (!dev || dev->sector_count <= PERSISTFS_RESERVED_SECTORS) {
        return 0;
    }
    return (uint32_t)(dev->sector_count - PERSISTFS_RESERVED_SECTORS);
}

static int persistfs_read_range(block_device_t *dev, uint32_t lba, uint64_t count, void *buffer) {
    uint64_t done = 0;

    if (!dev || !dev->read) {
        return -1;
    }
    while (done < count) {
        uint32_t chunk = (uint32_t)((count - done) > 255 ? 255 : (count - done));
        if (dev->read(dev->context, lba + (uint32_t)done, chunk, (uint8_t *)buffer + (done * PERSISTFS_SECTOR_SIZE)) != 0) {
            return -1;
        }
        done += chunk;
    }
    return 0;
}

static int persistfs_write_range(block_device_t *dev, uint32_t lba, uint64_t count, const void *buffer) {
    uint64_t done = 0;

    if (!dev || !dev->write) {
        return -1;
    }
    while (done < count) {
        uint32_t chunk = (uint32_t)((count - done) > 255 ? 255 : (count - done));
        if (dev->write(dev->context, lba + (uint32_t)done, chunk, (const uint8_t *)buffer + (done * PERSISTFS_SECTOR_SIZE)) != 0) {
            return -1;
        }
        done += chunk;
    }
    return 0;
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

int persistfs_export_image(char **buffer_out, uint64_t *size_out, uint64_t *entry_count_out) {
    uint32_t entry_count = 0;
    uint64_t payload_bytes;
    uint64_t total_bytes;
    uint64_t sectors;
    uint8_t *buffer;
    char path[PERSISTFS_PATH_CAP];
    persistfs_header_t *header;
    uint8_t *cursor;

    if (!buffer_out || !size_out) return -100;
    *buffer_out = 0;
    *size_out = 0;
    if (entry_count_out) *entry_count_out = 0;

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
    buffer = (uint8_t *)kmalloc((size_t)(sectors * PERSISTFS_SECTOR_SIZE));
    if (!buffer) {
        return -103;
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
                return -104;
            }
        }
    }
    *buffer_out = (char *)buffer;
    *size_out = sectors * PERSISTFS_SECTOR_SIZE;
    if (entry_count_out) {
        *entry_count_out = entry_count;
    }
    return 0;
}

int persistfs_import_image(const char *buffer_in, uint64_t size, uint64_t *entries_loaded_out) {
    persistfs_header_t *header;
    uint8_t *buffer = (uint8_t *)buffer_in;
    uint8_t *cursor;
    uint64_t loaded = 0;

    if (!buffer || size < sizeof(persistfs_header_t)) return -1;
    header = (persistfs_header_t *)buffer;
    if (header->magic != PERSISTFS_MAGIC || header->version != PERSISTFS_VERSION ||
        header->bytes_used < sizeof(persistfs_header_t) || header->bytes_used > size) {
        return -1;
    }

    cursor = buffer + sizeof(persistfs_header_t);
    for (uint32_t i = 0; i < header->entry_count; i++) {
        persistfs_entry_t entry;
        char path[PERSISTFS_PATH_CAP];
        const char *data = 0;

        if ((uint64_t)(cursor - buffer) + sizeof(entry) > header->bytes_used) {
            return -1;
        }
        copy_bytes((char *)&entry, (const char *)cursor, sizeof(entry));
        cursor += sizeof(entry);
        if (entry.path_len == 0 || entry.path_len >= sizeof(path)) {
            return -1;
        }
        if ((uint64_t)(cursor - buffer) + entry.path_len + entry.data_size > header->bytes_used) {
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
            return -1;
        }
        loaded++;
    }
    if (entries_loaded_out) {
        *entries_loaded_out = loaded;
    }
    return 0;
}

static int persistfs_sync_to_device(block_device_t *dev) {
    char *buffer = 0;
    uint64_t size = 0;

    if (!dev) {
        return -101;
    }
    if (persistfs_start_lba_for_device(dev) == 0) {
        return -106;
    }
    if (persistfs_export_image(&buffer, &size, 0) != 0) {
        return -103;
    }
    if ((size / PERSISTFS_SECTOR_SIZE) > PERSISTFS_RESERVED_SECTORS) {
        kfree(buffer);
        return -102;
    }
    if (persistfs_write_range(dev, persistfs_start_lba_for_device(dev), size / PERSISTFS_SECTOR_SIZE, buffer) != 0) {
        kfree(buffer);
        return -105;
    }
    kfree(buffer);
    return 0;
}

int persistfs_sync(void) {
    if (persistfs_file_backend) {
        const partition_info_t *part;
        char *buffer = 0;
        uint64_t size = 0;

        if (persistfs_active_partition_index < 0) return 0;
        part = partition_get((uint32_t)persistfs_active_partition_index);
        if (!part) return 0;
        if (persistfs_export_image(&buffer, &size, 0) != 0) return -1;
        if (system_install_write_root_bundle(part, buffer, size, persistfs_swap_partition_index) != 0) {
            kfree(buffer);
            return -1;
        }
        kfree(buffer);
        return 0;
    } else {
        block_device_t *dev;
        if (persistfs_active_device_index < 0) {
            return 0;
        }
        dev = block_get((uint32_t)persistfs_active_device_index);
        if (!dev) {
            return 0;
        }
        return persistfs_sync_to_device(dev);
    }
}

int persistfs_sync_device(uint32_t device_index) {
    block_device_t *dev = block_get(device_index);
    if (!dev || !dev->write) {
        return -1;
    }
    return persistfs_sync_to_device(dev);
}

int persistfs_init(void) {
    uint8_t sector[PERSISTFS_SECTOR_SIZE];
    persistfs_header_t *header = (persistfs_header_t *)sector;
    uint8_t *buffer = 0;
    uint64_t sectors;
    uint8_t *cursor;
    block_device_t *dev = 0;

    persistfs_available = 0;
    persistfs_entries_loaded = 0;
    persistfs_active_device_index = -1;
    persistfs_active_partition_index = -1;
    persistfs_file_backend = 0;
    persistfs_swap_partition_index = -1;

    if (persistfs_live_boot) {
        return -1;
    }

    if (partition_count() > 0) {
        for (uint32_t i = 0; i < partition_count(); i++) {
            const partition_info_t *part = partition_get(i);
            char *bundle = 0;
            uint64_t size = 0;
            int32_t swap_partition = -1;
            uint64_t loaded = 0;

            if (!part || part->fs_hint != PARTITION_FS_FAT32 || part->role != PARTITION_ROLE_SYSTEM) {
                continue;
            }
            if (system_install_read_root_bundle(part, &bundle, &size, &swap_partition) != 0) {
                continue;
            }
            if (persistfs_import_image(bundle, size, &loaded) != 0) {
                kfree(bundle);
                continue;
            }
            kfree(bundle);
            persistfs_available = 1;
            persistfs_entries_loaded = loaded;
            persistfs_active_partition_index = (int)i;
            persistfs_active_device_index = -1;
            persistfs_file_backend = 1;
            persistfs_swap_partition_index = swap_partition;
            vfs_set_sync_hook(persistfs_sync);
            return 0;
        }
    }

    if (block_count() == 0) {
        return -1;
    }
    dev = block_get(0);
    if (!dev) {
        return -1;
    }
    persistfs_available = 1;
    persistfs_active_device_index = 0;

    if (persistfs_start_lba_for_device(dev) == 0) {
        return -1;
    }
    if (persistfs_read_range(dev, persistfs_start_lba_for_device(dev), 1, sector) != 0) {
        return -1;
    }

    if (header->magic != PERSISTFS_MAGIC || header->version != PERSISTFS_VERSION ||
        header->bytes_used < sizeof(persistfs_header_t)) {
        vfs_set_sync_hook(persistfs_sync);
        persistfs_entries_loaded = 0;
        return persistfs_sync();
    }

    sectors = align_up(header->bytes_used, PERSISTFS_SECTOR_SIZE) / PERSISTFS_SECTOR_SIZE;
    if (sectors == 0 || sectors > PERSISTFS_RESERVED_SECTORS) {
        return -1;
    }

    buffer = (uint8_t *)kmalloc((size_t)(sectors * PERSISTFS_SECTOR_SIZE));
    if (!buffer) {
        return -1;
    }
    if (persistfs_read_range(dev, persistfs_start_lba_for_device(dev), sectors, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    header = (persistfs_header_t *)buffer;
    if (persistfs_import_image((const char *)buffer, sectors * PERSISTFS_SECTOR_SIZE, &persistfs_entries_loaded) != 0) {
        kfree(buffer);
        return -1;
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

void persistfs_set_live_mode(int enabled) {
    persistfs_live_boot = enabled ? 1 : 0;
    if (persistfs_live_boot) {
        persistfs_available = 0;
        persistfs_entries_loaded = 0;
        persistfs_active_device_index = -1;
    }
}

int persistfs_live_mode(void) {
    return persistfs_live_boot;
}

int persistfs_active_device(void) {
    return persistfs_active_device_index;
}

int persistfs_active_partition(void) {
    return persistfs_active_partition_index;
}
