#include "vfs.h"
#include "../memory/heap.h"
#include "../proc/sched.h"

#define VFS_FLUSH_QUIET_TICKS 100ULL
#define VFS_FLUSH_MAX_TICKS   500ULL

struct vfs_node {
    char *name;
    uint64_t inode;
    uint64_t created;
    uint64_t modified;
    uint64_t size;
    uint8_t type;
    uint8_t readonly;
    uint8_t mount_id;
    uint8_t lazy;
    uint64_t cap;
    uint8_t lazy_dir;
    uint64_t ext_ref;
    char *data;
    struct vfs_node *parent;
    struct vfs_node *first_child;
    struct vfs_node *next_sibling;
};

static vfs_node_t *vfs_root_node = 0;
static uint64_t vfs_next_inode = 1;
static uint64_t vfs_tick = 1;
static const uint64_t VFS_NAME_CAP = 63;
static int (*vfs_sync_hook)(void) = 0;
static vfs_external_fn vfs_external_hook = 0;
static vfs_loader_fn vfs_loader_hook = 0;
static vfs_dir_loader_fn vfs_dir_loader_hook = 0;
static void ensure_children(vfs_node_t *dir);

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static int valid_name(const char *name) {
    uint64_t len = 0;

    if (!name || !*name) {
        return 0;
    }
    if (str_eq(name, ".") || str_eq(name, "..")) {
        return 0;
    }

    while (name[len]) {
        if (name[len] == '/') {
            return 0;
        }
        len++;
        if (len > VFS_NAME_CAP) {
            return 0;
        }
    }

    return 1;
}

static char *dup_cstr(const char *text) {
    uint64_t len = str_len(text);
    char *out = (char *)kmalloc((size_t)(len + 1));
    if (!out) {
        return 0;
    }
    for (uint64_t i = 0; i < len; i++) {
        out[i] = text[i];
    }
    out[len] = '\0';
    return out;
}

static vfs_node_t *new_node(const char *name, uint8_t type, uint8_t readonly) {
    vfs_node_t *node = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    if (!node) {
        return 0;
    }

    node->name = dup_cstr(name);
    if (!node->name) {
        kfree(node);
        return 0;
    }

    node->inode = vfs_next_inode++;
    node->created = vfs_tick++;
    node->modified = node->created;
    node->type = type;
    node->readonly = readonly;
    return node;
}

static vfs_node_t *find_child(vfs_node_t *dir, const char *name) {
    vfs_node_t *child;

    if (!dir || dir->type != VFS_NODE_DIR) {
        return 0;
    }
    ensure_children(dir);

    child = dir->first_child;
    while (child) {
        if (str_eq(child->name, name)) {
            return child;
        }
        child = child->next_sibling;
    }

    return 0;
}

static void append_child(vfs_node_t *dir, vfs_node_t *child) {
    vfs_node_t *tail;

    child->parent = dir;
    if (!child->mount_id) child->mount_id = dir->mount_id;
    if (!dir->first_child) {
        dir->first_child = child;
        return;
    }

    tail = dir->first_child;
    while (tail->next_sibling) {
        tail = tail->next_sibling;
    }
    tail->next_sibling = child;
}

static const char *skip_slash(const char *path) {
    while (*path == '/') {
        path++;
    }
    return path;
}

static const char *next_component(const char *path, char *part, uint64_t part_cap) {
    uint64_t len = 0;

    path = skip_slash(path);
    while (*path && *path != '/') {
        if (len + 1 < part_cap) {
            part[len++] = *path;
        }
        path++;
    }
    part[len] = '\0';
    return path;
}

static int component_was_truncated(const char *path, uint64_t part_cap) {
    uint64_t len = 0;

    path = skip_slash(path);
    while (*path && *path != '/') {
        len++;
        path++;
        if (len >= part_cap) {
            return 1;
        }
    }

    return 0;
}

static vfs_node_t *resolve_parent(vfs_node_t *cwd, const char *path, char *leaf, uint64_t leaf_cap, int create_dirs) {
    vfs_node_t *node = (*path == '/') ? vfs_root_node : cwd;
    char part[64];

    if (!node || !path || !*path) {
        return 0;
    }

    path = skip_slash(path);
    if (!*path) {
        return 0;
    }

    while (*path) {
        if (component_was_truncated(path, sizeof(part))) {
            return 0;
        }

        const char *next = next_component(path, part, sizeof(part));

        next = skip_slash(next);
        if (!*next) {
            uint64_t i = 0;
            while (part[i] && i + 1 < leaf_cap) {
                leaf[i] = part[i];
                i++;
            }
            leaf[i] = '\0';
            return node;
        }

        if (str_eq(part, ".")) {
            path = next;
            continue;
        }
        if (str_eq(part, "..")) {
            if (node->parent) {
                node = node->parent;
            }
            path = next;
            continue;
        }
        if (!valid_name(part)) {
            return 0;
        }

        vfs_node_t *child = find_child(node, part);
        if (!child && create_dirs) {
            child = new_node(part, VFS_NODE_DIR, 0);
            if (!child) {
                return 0;
            }
            append_child(node, child);
        }
        if (!child || child->type != VFS_NODE_DIR) {
            return 0;
        }

        node = child;
        path = next;
    }

    return 0;
}

int vfs_init(void) {
    vfs_root_node = new_node("", VFS_NODE_DIR, 0);
    return vfs_root_node ? 0 : -1;
}

vfs_node_t *vfs_root(void) {
    return vfs_root_node;
}

void vfs_set_sync_hook(int (*hook)(void)) {
    vfs_sync_hook = hook;
}

static int vfs_dirty;
static uint64_t vfs_dirty_first;
static uint64_t vfs_dirty_last;

int vfs_sync(void) {
    uint64_t now = sched_ticks();
    if (!vfs_sync_hook) {
        return 0;
    }
    if (!vfs_dirty) {
        vfs_dirty = 1;
        vfs_dirty_first = now;
    }
    vfs_dirty_last = now;
    return 0;
}

int vfs_flush(int force) {
    uint64_t now = sched_ticks();
    if (!vfs_dirty || !vfs_sync_hook) {
        return 0;
    }
    if (!force && now - vfs_dirty_last < VFS_FLUSH_QUIET_TICKS && now - vfs_dirty_first < VFS_FLUSH_MAX_TICKS) {
        return 0;
    }
    vfs_dirty = 0;
    return vfs_sync_hook();
}

vfs_node_t *vfs_resolve(vfs_node_t *cwd, const char *path) {
    vfs_node_t *node = (*path == '/') ? vfs_root_node : cwd;
    char part[64];

    if (!node || !path || !*path) {
        return 0;
    }

    path = skip_slash(path);
    if (!*path) {
        return node;
    }

    while (*path) {
        if (component_was_truncated(path, sizeof(part))) {
            return 0;
        }

        path = next_component(path, part, sizeof(part));

        if (str_eq(part, ".")) {
            path = skip_slash(path);
            continue;
        }
        if (str_eq(part, "..")) {
            if (node->parent) {
                node = node->parent;
            }
            path = skip_slash(path);
            continue;
        }
        if (!valid_name(part)) {
            return 0;
        }

        node = find_child(node, part);
        if (!node) {
            return 0;
        }
        path = skip_slash(path);
    }

    return node;
}

int vfs_getcwd(vfs_node_t *node, char *buf, size_t size) {
    const char *parts[32];
    uint64_t count = 0;
    uint64_t out = 0;

    if (!node || !buf || size == 0) {
        return -1;
    }

    if (node == vfs_root_node) {
        if (size < 2) {
            return -1;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    while (node && node != vfs_root_node && count < 32) {
        parts[count++] = node->name;
        node = node->parent;
    }

    for (int64_t i = (int64_t)count - 1; i >= 0; i--) {
        uint64_t len = str_len(parts[i]);
        if (out + len + 1 >= size) {
            return -1;
        }
        buf[out++] = '/';
        for (uint64_t j = 0; j < len; j++) {
            buf[out++] = parts[i][j];
        }
    }

    buf[out] = '\0';
    return 0;
}

void vfs_set_external_hook(vfs_external_fn fn) {
    vfs_external_hook = fn;
}

static int build_path(vfs_node_t *dir, const char *leaf, char *path, uint64_t cap) {
    uint64_t n;
    if (vfs_getcwd(dir, path, cap) != 0) return -1;
    n = str_len(path);
    if (n == 0 || path[n - 1] != '/') {
        if (n + 1 >= cap) return -1;
        path[n++] = '/';
    }
    if (n + str_len(leaf) + 1 > cap) return -1;
    copy_bytes(path + n, leaf, str_len(leaf) + 1);
    return 0;
}

void vfs_set_loader(vfs_loader_fn fn) {
    vfs_loader_hook = fn;
}

void vfs_set_dir_loader(vfs_dir_loader_fn fn) {
    vfs_dir_loader_hook = fn;
}

static void ensure_children(vfs_node_t *dir) {
    char path[512];
    if (!dir || !dir->lazy_dir || !vfs_dir_loader_hook) return;
    dir->lazy_dir = 0;
    if (vfs_getcwd(dir, path, sizeof(path)) != 0) return;
    (void)vfs_dir_loader_hook(dir->mount_id, path, dir->ext_ref);
}

int64_t vfs_node_read_at(vfs_node_t *node, uint64_t off, char *buf, uint64_t len) {
    char path[512];
    if (!node || node->type != VFS_NODE_FILE) return -1;
    if (off >= node->size) return 0;
    if (len > node->size - off) len = node->size - off;
    if (!node->lazy) {
        copy_bytes(buf, node->data + off, len);
        return (int64_t)len;
    }
    if (!vfs_loader_hook || build_path(node->parent, node->name, path, sizeof(path)) != 0) return -1;
    return vfs_loader_hook(node->mount_id, path, node->ext_ref, off, buf, len);
}

uint8_t vfs_node_is_lazy(vfs_node_t *node) {
    return node ? node->lazy : 0;
}

static int ensure_loaded(vfs_node_t *node) {
    char *buf;
    int64_t got;
    if (!node || !node->lazy) return 0;
    if (node->size > VFS_LAZY_LOAD_MAX) return -1;
    buf = (char *)kmalloc((size_t)(node->size + 1));
    if (!buf) return -1;
    got = node->size ? vfs_node_read_at(node, 0, buf, node->size) : 0;
    if (got < 0 || (uint64_t)got != node->size) {
        kfree(buf);
        return -1;
    }
    buf[node->size] = 0;
    node->data = buf;
    node->cap = node->size;
    node->lazy = 0;
    return 0;
}

static int external_op(int op, vfs_node_t *dir, const char *leaf, const char *data, uint64_t size) {
    char path[512];
    uint64_t n;
    if (!dir || !dir->mount_id || !vfs_external_hook) return 0;
    if (vfs_getcwd(dir, path, sizeof(path)) != 0) return -1;
    n = str_len(path);
    if (n == 0 || path[n - 1] != '/') {
        if (n + 1 >= sizeof(path)) return -1;
        path[n++] = '/';
    }
    if (n + str_len(leaf) + 1 > sizeof(path)) return -1;
    copy_bytes(path + n, leaf, str_len(leaf) + 1);
    return vfs_external_hook(op, dir->mount_id, path, data, size, 0);
}

int vfs_mkdir(vfs_node_t *cwd, const char *path) {
    char leaf[64];
    vfs_node_t *parent = resolve_parent(cwd, path, leaf, sizeof(leaf), 0);
    vfs_node_t *node;

    if (!parent || !valid_name(leaf) || find_child(parent, leaf)) {
        return -1;
    }
    if (external_op(VFS_EXT_MKDIR, parent, leaf, 0, 0) != 0) {
        return -1;
    }

    node = new_node(leaf, VFS_NODE_DIR, 0);
    if (!node) {
        return -1;
    }
    append_child(parent, node);
    parent->modified = vfs_tick++;
    return vfs_sync();
}

int vfs_create(vfs_node_t *cwd, const char *path) {
    char leaf[64];
    vfs_node_t *parent = resolve_parent(cwd, path, leaf, sizeof(leaf), 0);
    vfs_node_t *node;


    if (!parent || !valid_name(leaf)) {
        return -1;
    }

    node = find_child(parent, leaf);
    if (node) {
        return node->type == VFS_NODE_FILE ? 0 : -1;
    }

    if (external_op(VFS_EXT_WRITE, parent, leaf, "", 0) != 0) {
        return -1;
    }
    node = new_node(leaf, VFS_NODE_FILE, 0);
    if (!node) {
        return -1;
    }
    append_child(parent, node);
    parent->modified = vfs_tick++;
    return vfs_sync();
}

static int subtree_has_readonly(vfs_node_t *node) {
    if (node->readonly) {
        return 1;
    }
    for (vfs_node_t *c = node->first_child; c; c = c->next_sibling) {
        if (subtree_has_readonly(c)) {
            return 1;
        }
    }
    return 0;
}

int vfs_remove(vfs_node_t *cwd, const char *path) {
    vfs_node_t *node = vfs_resolve(cwd, path);
    vfs_node_t *parent;
    vfs_node_t **link;

    if (!node || node == vfs_root_node || !node->parent || subtree_has_readonly(node)) {
        return -1;
    }
    for (vfs_node_t *p = cwd; p; p = p->parent) {
        if (p == node) {
            return -1;
        }
    }

    parent = node->parent;
    if (node->mount_id && external_op(VFS_EXT_REMOVE, parent, node->name, 0, 0) != 0) {
        return -1;
    }
    link = &parent->first_child;
    while (*link && *link != node) {
        link = &(*link)->next_sibling;
    }
    if (!*link) {
        return -1;
    }
    *link = node->next_sibling;
    node->next_sibling = 0;
    parent->modified = vfs_tick++;
    return vfs_sync();
}

int vfs_write(vfs_node_t *cwd, const char *path, const char *data, uint64_t size) {
    vfs_node_t *node = vfs_resolve(cwd, path);
    char *next;


    if (!node) {
        int crc = vfs_create(cwd, path);
        if (crc != 0) {
            return -1;
        }
        node = vfs_resolve(cwd, path);
    }

    if (!node || node->type != VFS_NODE_FILE || node->readonly) {
        return -1;
    }
    if (node->mount_id && external_op(VFS_EXT_WRITE, node->parent, node->name, data, size) != 0) {
        return -1;
    }

    next = (char *)kmalloc((size_t)(size + 1));
    if (!next) {
        return -1;
    }
    if (size) {
        copy_bytes(next, data, size);
    }
    next[size] = '\0';

    if (node->data) {
        kfree(node->data);
    }
    node->data = next;
    node->cap = size;
    node->size = size;
    node->lazy = 0;
    node->modified = vfs_tick++;
    return vfs_sync();
}

static int node_reserve(vfs_node_t *node, uint64_t need) {
    uint64_t cap = node->cap ? node->cap : 64;
    char *next;
    if (node->data && need <= node->cap) return 0;
    while (cap < need) cap *= 2;
    next = (char *)kmalloc((size_t)(cap + 1));
    if (!next) return -1;
    for (uint64_t i = 0; i < node->size && node->data; i++) next[i] = node->data[i];
    for (uint64_t i = node->size; i <= cap; i++) next[i] = 0;
    if (node->data) kfree(node->data);
    node->data = next;
    node->cap = cap;
    return 0;
}

static int external_range(int op, vfs_node_t *node, const char *data, uint64_t size, uint64_t off) {
    char path[512];
    if (!node->mount_id || !vfs_external_hook) return 0;
    if (build_path(node->parent, node->name, path, sizeof(path)) != 0) return -1;
    return vfs_external_hook(op, node->mount_id, path, data, size, off);
}

int vfs_node_write_at(vfs_node_t *node, uint64_t off, const char *data,
                      uint64_t size) {
    uint64_t new_end;

    if (!node || node->type != VFS_NODE_FILE || node->readonly) {
        return -1;
    }
    if (!data && size != 0) {
        return -1;
    }
    new_end = off + size;
    if (new_end < off) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    if (external_range(VFS_EXT_WRITE_AT, node, data, size, off) != 0) {
        return -1;
    }
    if (node->lazy) {
        if (new_end > node->size) node->size = new_end;
        node->modified = vfs_tick++;
        return 0;
    }
    if (node_reserve(node, new_end > node->size ? new_end : node->size) != 0) {
        return -1;
    }
    for (uint64_t i = node->size; i < off; i++) {
        node->data[i] = 0;
    }
    for (uint64_t i = 0; i < size; i++) {
        node->data[off + i] = data[i];
    }
    if (new_end > node->size) {
        node->size = new_end;
        node->data[new_end] = 0;
    }
    node->modified = vfs_tick++;
    return node->mount_id ? 0 : vfs_sync();
}

int vfs_node_truncate(vfs_node_t *node, uint64_t len) {
    if (!node || node->type != VFS_NODE_FILE || node->readonly) {
        return -1;
    }
    if (len == node->size) {
        return 0;
    }
    if (external_range(VFS_EXT_TRUNCATE, node, 0, len, 0) != 0) {
        return -1;
    }
    if (node->lazy) {
        node->size = len;
        if (len == 0) node->lazy = 0;
        node->modified = vfs_tick++;
        return 0;
    }
    if (len > node->size) {
        if (node_reserve(node, len) != 0) return -1;
        for (uint64_t i = node->size; i < len; i++) node->data[i] = 0;
    }
    node->size = len;
    if (node->data) node->data[len] = 0;
    node->modified = vfs_tick++;
    return node->mount_id ? 0 : vfs_sync();
}

int vfs_seed_readonly(const char *path, const char *data, uint64_t size) {
    char leaf[64];
    vfs_node_t *parent;
    vfs_node_t *node;

    if (!vfs_root_node || !path || !*path) {
        return -1;
    }

    parent = resolve_parent(vfs_root_node, path, leaf, sizeof(leaf), 1);
    if (!parent || !valid_name(leaf)) {
        return -1;
    }

    node = find_child(parent, leaf);
    if (!node) {
        node = new_node(leaf, VFS_NODE_FILE, 1);
        if (!node) {
            return -1;
        }
        append_child(parent, node);
        parent->modified = vfs_tick++;
    }

    if (node->type != VFS_NODE_FILE) {
        return -1;
    }

    if (node->data) {
        kfree(node->data);
        node->data = 0;
    }

    node->data = (char *)kmalloc((size_t)(size + 1));
    if (!node->data) {
        return -1;
    }
    node->cap = size;
    if (size) {
        copy_bytes(node->data, data, size);
    }
    node->data[size] = '\0';
    node->size = size;
    node->readonly = 1;
    node->modified = vfs_tick++;
    return 0;
}

int vfs_import_node(const char *path, uint8_t type, uint8_t readonly, const char *data, uint64_t size,
                    uint64_t inode, uint64_t created, uint64_t modified) {
    char leaf[64];
    vfs_node_t *parent;
    vfs_node_t *node;

    if (!vfs_root_node || !path || !*path) {
        return -1;
    }

    parent = resolve_parent(vfs_root_node, path, leaf, sizeof(leaf), 1);
    if (!parent || !valid_name(leaf)) {
        return -1;
    }

    node = find_child(parent, leaf);
    if (!node) {
        node = new_node(leaf, type, readonly);
        if (!node) {
            return -1;
        }
        append_child(parent, node);
    } else if (node->type != type) {
        return -1;
    }

    if (type == VFS_NODE_FILE) {
        char *next = (char *)kmalloc((size_t)(size + 1));
        if (!next) {
            return -1;
        }
        if (size) {
            copy_bytes(next, data, size);
        }
        next[size] = '\0';
        if (node->data) {
            kfree(node->data);
        }
        node->data = next;
        node->size = size;
        node->lazy = 0;
        node->cap = size;
    }

    node->readonly = readonly;
    node->inode = inode;
    node->created = created;
    node->modified = modified;
    if (node->inode >= vfs_next_inode) {
        vfs_next_inode = node->inode + 1;
    }
    if (node->modified >= vfs_tick) {
        vfs_tick = node->modified + 1;
    }
    if (node->created >= vfs_tick) {
        vfs_tick = node->created + 1;
    }
    return 0;
}

const char *vfs_read(vfs_node_t *cwd, const char *path, uint64_t *size_out) {
    vfs_node_t *node = vfs_resolve(cwd, path);
    if (!node || node->type != VFS_NODE_FILE || ensure_loaded(node) != 0) {
        return 0;
    }
    if (size_out) {
        *size_out = node->size;
    }
    return node->data ? node->data : "";
}

int vfs_stat(vfs_node_t *cwd, const char *path, vfs_stat_t *out) {
    vfs_node_t *node = vfs_resolve(cwd, path);
    if (!node || !out) {
        return -1;
    }

    out->inode = node->inode;
    out->size = node->size;
    out->created = node->created;
    out->modified = node->modified;
    out->type = node->type;
    out->readonly = node->readonly;
    return 0;
}

vfs_node_t *vfs_child_at(vfs_node_t *dir, uint64_t index) {
    vfs_node_t *child;

    if (!dir || dir->type != VFS_NODE_DIR) {
        return 0;
    }
    ensure_children(dir);

    child = dir->first_child;
    while (child && index) {
        child = child->next_sibling;
        index--;
    }
    return child;
}

uint64_t vfs_child_count(vfs_node_t *dir) {
    uint64_t count = 0;
    vfs_node_t *child;

    if (!dir || dir->type != VFS_NODE_DIR) {
        return 0;
    }
    ensure_children(dir);

    child = dir->first_child;
    while (child) {
        count++;
        child = child->next_sibling;
    }
    return count;
}

const char *vfs_node_name(vfs_node_t *node) {
    return node ? node->name : "";
}

uint8_t vfs_node_type(vfs_node_t *node) {
    return node ? node->type : 0;
}

uint8_t vfs_node_mount_id(vfs_node_t *node) {
    return node ? node->mount_id : 0;
}

int vfs_import_ref(const char *path, uint8_t type, uint64_t size, uint8_t readonly, uint64_t ref) {
    vfs_node_t *node;
    if (type == VFS_NODE_DIR) {
        if (vfs_import_node(path, VFS_NODE_DIR, readonly, 0, 0, 0, 0, 0) != 0) return -1;
        node = vfs_resolve(vfs_root_node, path);
        if (!node) return -1;
        node->lazy_dir = 1;
        node->ext_ref = ref;
        return 0;
    }
    if (vfs_import_lazy(path, size, readonly) != 0) return -1;
    node = vfs_resolve(vfs_root_node, path);
    if (!node) return -1;
    node->ext_ref = ref;
    return 0;
}

int vfs_import_lazy(const char *path, uint64_t size, uint8_t readonly) {
    vfs_node_t *node;
    if (vfs_import_node(path, VFS_NODE_FILE, readonly, "", 0, 0, 0, 0) != 0) return -1;
    node = vfs_resolve(vfs_root_node, path);
    if (!node) return -1;
    if (node->data) kfree(node->data);
    node->data = 0;
    node->size = size;
    node->lazy = 1;
    node->cap = 0;
    return 0;
}

int vfs_set_mount(const char *path, uint8_t mount_id) {
    vfs_node_t *node = vfs_resolve(vfs_root_node, path);
    if (!node || node->type != VFS_NODE_DIR) return -1;
    node->mount_id = mount_id;
    return 0;
}

int vfs_detach_tree(const char *path) {
    vfs_node_t *node = vfs_resolve(vfs_root_node, path);
    vfs_node_t **link;
    if (!node || node == vfs_root_node || !node->parent) return -1;
    link = &node->parent->first_child;
    while (*link && *link != node) link = &(*link)->next_sibling;
    if (!*link) return -1;
    *link = node->next_sibling;
    node->next_sibling = 0;
    return 0;
}

uint8_t vfs_node_readonly(vfs_node_t *node) {
    return node ? node->readonly : 1;
}

uint64_t vfs_node_size(vfs_node_t *node) {
    return node ? node->size : 0;
}

uint64_t vfs_node_inode(vfs_node_t *node) {
    return node ? node->inode : 0;
}

uint64_t vfs_node_created(vfs_node_t *node) {
    return node ? node->created : 0;
}

uint64_t vfs_node_modified(vfs_node_t *node) {
    return node ? node->modified : 0;
}

const char *vfs_node_data(vfs_node_t *node) {
    if (node && ensure_loaded(node) != 0) return 0;
    return (node && node->data) ? node->data : "";
}
