#include "fat32.h"
#include "vfs.h"
#include "fatfs.h"
#include "../drivers/storage/partition.h"
#include "../memory/heap.h"

#define FAT32_PATH_CAP       512U
#define FAT32_MOUNT_MAX      16U
#define FAT32_IMPORT_MAX     (32ULL * 1024ULL * 1024ULL)
#define FAT32_DEPTH_MAX      16U
#define FAT32_VFS_NAME_MAX   63U

typedef struct {
    int             used;
    int             writable;
    char            path[64];
    block_device_t *dev;
    uint64_t        start;
    uint64_t        sectors;
} fat32_mount_t;

static fat32_mount_t mounts[FAT32_MOUNT_MAX];
static uint32_t mounted_fat32 = 0;

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) len++;
    return len;
}

static void copy_text(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void append_text(char *dst, const char *src, uint32_t cap) {
    uint32_t len = (uint32_t)str_len(dst);
    if (len >= cap) return;
    copy_text(dst + len, src, cap - len);
}

static void append_u32(char *dst, uint32_t cap, uint32_t value) {
    char tmp[12];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && n < sizeof(tmp));
    while (n) {
        char one[2] = { tmp[--n], 0 };
        append_text(dst, one, cap);
    }
}

static int fat32_external(int op, uint8_t mount_id, const char *path, const char *data, uint64_t size) {
    fat32_mount_t *m;
    fatfs_t *vol;
    uint64_t prefix;
    const char *rel;
    int rc = -1;
    if (mount_id == 0 || mount_id > FAT32_MOUNT_MAX) return 0;
    m = &mounts[mount_id - 1];
    if (!m->used) return 0;
    if (!m->writable) return -1;
    prefix = str_len(m->path);
    for (uint64_t i = 0; i < prefix; i++) {
        if (path[i] != m->path[i]) return -1;
    }
    rel = path + prefix;
    if (*rel != '/') return -1;
    vol = (fatfs_t *)kmalloc(sizeof(fatfs_t));
    if (!vol) return -1;
    if (fatfs_mount(vol, m->dev, m->start, m->sectors) == 0) {
        if (op == VFS_EXT_MKDIR) rc = fatfs_mkdir(vol, rel);
        else if (op == VFS_EXT_WRITE) rc = fatfs_write(vol, rel, data, size);
        else if (op == VFS_EXT_REMOVE) rc = fatfs_remove(vol, rel);
    }
    kfree(vol);
    return rc == 0 ? 0 : -1;
}

typedef struct {
    fatfs_t *vol;
    char     path[FAT32_PATH_CAP];
    uint32_t depth;
    uint8_t  readonly;
} import_ctx_t;

static int import_dir(fatfs_t *vol, uint32_t cluster, const char *path, uint32_t depth, uint8_t readonly);

static int import_cb(const fatfs_entry_t *e, void *p) {
    import_ctx_t *ctx = (import_ctx_t *)p;
    char path[FAT32_PATH_CAP];
    if (str_len(e->name) > FAT32_VFS_NAME_MAX) return 0;
    copy_text(path, ctx->path, sizeof(path));
    append_text(path, "/", sizeof(path));
    append_text(path, e->name, sizeof(path));
    if (str_len(path) + 1 >= sizeof(path)) return 0;
    if (e->attr & FATFS_ATTR_DIR) {
        if (vfs_import_node(path, VFS_NODE_DIR, ctx->readonly, 0, 0, 0, 0, 0) != 0) return 0;
        if (e->cluster >= 2) (void)import_dir(ctx->vol, e->cluster, path, ctx->depth + 1, ctx->readonly);
    } else if (e->size <= FAT32_IMPORT_MAX) {
        char *data = 0;
        if (fatfs_read_entry(ctx->vol, e, &data) == 0) {
            (void)vfs_import_node(path, VFS_NODE_FILE, ctx->readonly || (e->attr & FATFS_ATTR_RO), data, e->size,
                                  0, 0, 0);
            kfree(data);
        }
    } else {
        (void)vfs_import_node(path, VFS_NODE_FILE, 1, "", 0, 0, 0, 0);
    }
    return 0;
}

static int import_dir(fatfs_t *vol, uint32_t cluster, const char *path, uint32_t depth, uint8_t readonly) {
    import_ctx_t *ctx;
    int rc;
    if (depth > FAT32_DEPTH_MAX) return 0;
    ctx = (import_ctx_t *)kmalloc(sizeof(import_ctx_t));
    if (!ctx) return -1;
    ctx->vol = vol;
    ctx->depth = depth;
    ctx->readonly = readonly;
    copy_text(ctx->path, path, sizeof(ctx->path));
    rc = fatfs_iterate(vol, cluster, import_cb, ctx);
    kfree(ctx);
    return rc < 0 ? -1 : 0;
}

static int fat32_mount_partition_info(const partition_info_t *part, const char *mount_path) {
    fatfs_t *vol;
    fat32_mount_t *m = 0;
    uint8_t id = 0;
    int rc;
    if (!part || !mount_path || !*mount_path || part->fs_hint != PARTITION_FS_FAT32) return -1;
    for (uint32_t i = 0; i < FAT32_MOUNT_MAX; i++) {
        if (!mounts[i].used) {
            m = &mounts[i];
            id = (uint8_t)(i + 1);
            break;
        }
    }
    if (!m) return -1;
    vol = (fatfs_t *)kmalloc(sizeof(fatfs_t));
    if (!vol) return -1;
    if (fatfs_mount_part(vol, part) != 0) {
        kfree(vol);
        return -1;
    }
    m->used = 1;
    m->dev = part->device;
    m->start = part->start_lba;
    m->sectors = part->sector_count;
    m->writable = part->device->write && part->role != PARTITION_ROLE_EFI && part->role != PARTITION_ROLE_SYSTEM;
    copy_text(m->path, mount_path, sizeof(m->path));
    (void)vfs_detach_tree(mount_path);
    if (vfs_import_node(mount_path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) != 0 || vfs_set_mount(mount_path, id) != 0) {
        m->used = 0;
        kfree(vol);
        return -1;
    }
    rc = import_dir(vol, vol->root_cluster, mount_path, 0, (uint8_t)!m->writable);
    kfree(vol);
    return rc;
}

int fat32_mount_detected(void) {
    vfs_set_external_hook(fat32_external);
    for (uint32_t i = 0; i < FAT32_MOUNT_MAX; i++) {
        if (mounts[i].used) (void)vfs_detach_tree(mounts[i].path);
        mounts[i].used = 0;
    }
    mounted_fat32 = 0;
    (void)vfs_mkdir(vfs_root(), "/volumes");
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        char mount_path[64];
        if (!part || part->fs_hint != PARTITION_FS_FAT32) continue;
        copy_text(mount_path, "/volumes/fat32-", sizeof(mount_path));
        append_u32(mount_path, sizeof(mount_path), mounted_fat32);
        if (fat32_mount_partition_info(part, mount_path) == 0) mounted_fat32++;
    }
    return 0;
}

uint32_t fat32_mount_count(void) {
    return mounted_fat32;
}

int fat32_mount_partition(uint32_t partition_index, const char *mount_path) {
    const partition_info_t *part = partition_get(partition_index);
    vfs_set_external_hook(fat32_external);
    return fat32_mount_partition_info(part, mount_path);
}
