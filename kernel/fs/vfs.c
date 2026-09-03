#include "vfs.h"
#include "../memory/heap.h"

struct vfs_node {
    char *name;
    uint64_t inode;
    uint64_t created;
    uint64_t modified;
    uint64_t size;
    uint8_t type;
    uint8_t readonly;
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

int vfs_sync(void) {
    if (!vfs_sync_hook) {
        return 0;
    }
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

int vfs_mkdir(vfs_node_t *cwd, const char *path) {
    char leaf[64];
    vfs_node_t *parent = resolve_parent(cwd, path, leaf, sizeof(leaf), 0);
    vfs_node_t *node;

    if (!parent || !valid_name(leaf) || find_child(parent, leaf)) {
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

    node = new_node(leaf, VFS_NODE_FILE, 0);
    if (!node) {
        return -1;
    }
    append_child(parent, node);
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
    node->size = size;
    node->modified = vfs_tick++;
    return vfs_sync();
}

int vfs_node_write_at(vfs_node_t *node, uint64_t off, const char *data,
                      uint64_t size) {
    uint64_t new_end;
    char *next;
    uint64_t i;

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
    if (new_end <= node->size) {
        /* In-place overwrite inside the existing buffer. */
        for (i = 0; i < size; i++) {
            node->data[off + i] = data[i];
        }
        node->modified = vfs_tick++;
        return vfs_sync();
    }
    next = (char *)kmalloc((size_t)(new_end + 1));
    if (!next) {
        return -1;
    }
    for (i = 0; i < node->size; i++) {
        next[i] = node->data ? node->data[i] : 0;
    }
    for (; i < off; i++) {
        next[i] = 0;
    }
    for (i = 0; i < size; i++) {
        next[off + i] = data[i];
    }
    next[new_end] = '\0';
    if (node->data) {
        kfree(node->data);
    }
    node->data = next;
    node->size = new_end;
    node->modified = vfs_tick++;
    return vfs_sync();
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
    if (!node || node->type != VFS_NODE_FILE) {
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
    return (node && node->data) ? node->data : "";
}
