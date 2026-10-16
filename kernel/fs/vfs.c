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
    uint32_t owner;             /* who made it (VFS_NOBODY: the system, or not known) */
};

static vfs_node_t *vfs_root_node = 0;
static uint64_t vfs_next_inode = 1;
static uint64_t vfs_tick = 1;
static const uint64_t VFS_NAME_CAP = 255;
static int (*vfs_sync_hook)(void) = 0;
static vfs_external_fn vfs_external_hook = 0;
static vfs_loader_fn vfs_loader_hook = 0;
static vfs_dir_loader_fn vfs_dir_loader_hook = 0;
static void ensure_children(vfs_node_t *dir);

static const vfs_cred_t *(*vfs_cred_hook)(void);
static uint32_t (*vfs_home_owner_hook)(vfs_node_t *dir);
static int (*vfs_volume_class_hook)(uint8_t mount_id);
static int vfs_kernel_depth;
static int vfs_denied_flag;

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
    node->owner = VFS_NOBODY;
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

/* ---- users: who may do what ----------------------------------------------- */

enum { VFS_OP_READ, VFS_OP_CREATE, VFS_OP_MODIFY };   /* look, add to a directory, change or remove */

static const vfs_cred_t *cred(void) {
    return vfs_kernel_depth || !vfs_cred_hook ? 0 : vfs_cred_hook();
}

static uint32_t home_uid(vfs_node_t *dir) {
    return vfs_home_owner_hook ? vfs_home_owner_hook(dir) : VFS_NOBODY;
}

/* What a user may do with node n; where it lies decides (the saved image
 * keeps no owners or modes, so an older kernel still reads it). */
static int may_access(vfs_node_t *n, const vfs_cred_t *c, int op) {
    vfs_node_t *top = n, *second = 0;
    if (!c) return 1;
    if (n == vfs_root_node) return op == VFS_OP_READ;
    while (top->parent && top->parent != vfs_root_node) {
        second = top;
        top = top->parent;
    }
    if (!top->parent) return 0;                         /* not in the tree */
    /* a home: its user's alone (the folder itself stays put) */
    if (str_eq(top->name, "home") || str_eq(top->name, "cfg"))  /* the owner's (others see their own) */
        return home_uid(top) == c->uid && !(op == VFS_OP_MODIFY && n == top);
    if (str_eq(top->name, "users")) {
        if (!second) return op == VFS_OP_READ;        /* /users lists the homes */
        return home_uid(second) == c->uid && !(op == VFS_OP_MODIFY && n == second);
    }
    if (str_eq(top->name, "tmp")) {
        /* shared; what is there belongs to whoever made it */
        if (op != VFS_OP_MODIFY) return 1;
        return n != top && (n->owner == c->uid || n->owner == VFS_NOBODY);
    }
    if (str_eq(top->name, "volumes")) {
        int cls;
        if (!second) return op == VFS_OP_READ;
        cls = vfs_volume_class_hook ? vfs_volume_class_hook(second->mount_id) : VFS_VOL_INTERNAL;
        if (cls == VFS_VOL_REMOVABLE) return 1;
        if (cls == VFS_VOL_SYSTEM) return op == VFS_OP_READ || c->admin;
        return c->admin;                                /* internal disks: Windows, other systems */
    }
    if (str_eq(top->name, "etc") && second && str_eq(second->name, "icda")) return 0;   /* accounts */
    return op == VFS_OP_READ;                           /* the system: read only */
}

static int allowed(vfs_node_t *n, int op) {
    const vfs_cred_t *c = cred();
    if (may_access(n, c, op)) return 1;
    vfs_denied_flag = 1;
    return 0;
}

/* One step of a walk into child `part`: the caller's home answers to
 * /home, and the top two levels are where access is decided. */
static vfs_node_t *step_into(vfs_node_t *node, const char *part, const vfs_cred_t *c) {
    vfs_node_t *child;
    if (c && node == vfs_root_node) {
        if (c->home && str_eq(part, "home")) return c->home;
        if (c->cfg && str_eq(part, "cfg")) return c->cfg;     /* their settings */
    }
    child = find_child(node, part);
    if (child && c && (node == vfs_root_node || node->parent == vfs_root_node) &&
        !may_access(child, c, VFS_OP_READ)) {
        vfs_denied_flag = 1;
        return 0;
    }
    return child;
}

static vfs_node_t *step_up(vfs_node_t *node, const vfs_cred_t *c) {
    if (c && ((c->home && node == c->home) || (c->cfg && node == c->cfg))) return vfs_root_node;   /* ".." of /home, /cfg */
    return node->parent ? node->parent : node;
}

void vfs_set_cred_hook(const vfs_cred_t *(*fn)(void)) { vfs_cred_hook = fn; }
void vfs_set_home_owner_hook(uint32_t (*fn)(vfs_node_t *dir)) { vfs_home_owner_hook = fn; }
void vfs_set_volume_class_hook(int (*fn)(uint8_t mount_id)) { vfs_volume_class_hook = fn; }
void vfs_kernel_begin(void) { vfs_kernel_depth++; }
void vfs_kernel_end(void) { if (vfs_kernel_depth) vfs_kernel_depth--; }
uint32_t vfs_node_owner(vfs_node_t *node) { return node ? node->owner : VFS_NOBODY; }
vfs_node_t *vfs_node_parent(vfs_node_t *node) { return node ? node->parent : 0; }
int vfs_may_modify(vfs_node_t *node) { return node && may_access(node, cred(), VFS_OP_MODIFY); }

int vfs_last_denied(void) {
    int d = vfs_denied_flag;
    vfs_denied_flag = 0;
    return d;
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

static vfs_node_t *resolve_parent_as(vfs_node_t *cwd, const char *path, char *leaf, uint64_t leaf_cap, int create_dirs,
                                     const vfs_cred_t *c) {
    vfs_node_t *node = (*path == '/') ? vfs_root_node : cwd;
    char part[256];

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
            node = step_up(node, c);
            path = next;
            continue;
        }
        if (!valid_name(part)) {
            return 0;
        }

        vfs_node_t *child = step_into(node, part, c);
        if (!child && create_dirs && !c) {
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

/* the caller's view (public calls) */
static vfs_node_t *resolve_parent(vfs_node_t *cwd, const char *path, char *leaf, uint64_t leaf_cap, int create_dirs) {
    return resolve_parent_as(cwd, path, leaf, leaf_cap, create_dirs, cred());
}

/* the kernel's own view (importing, mounting) */
static vfs_node_t *resolve_parent_raw(vfs_node_t *cwd, const char *path, char *leaf, uint64_t leaf_cap, int create_dirs) {
    return resolve_parent_as(cwd, path, leaf, leaf_cap, create_dirs, 0);
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

static vfs_node_t *resolve_as(vfs_node_t *cwd, const char *path, const vfs_cred_t *c);

vfs_node_t *vfs_resolve(vfs_node_t *cwd, const char *path) {
    return resolve_as(cwd, path, cred());
}

static vfs_node_t *resolve_raw(vfs_node_t *cwd, const char *path) {
    return resolve_as(cwd, path, 0);
}

static vfs_node_t *resolve_as(vfs_node_t *cwd, const char *path, const vfs_cred_t *c) {
    vfs_node_t *node = (*path == '/') ? vfs_root_node : cwd;
    char part[256];

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
            node = step_up(node, c);
            path = skip_slash(path);
            continue;
        }
        if (!valid_name(part)) {
            return 0;
        }

        node = step_into(node, part, c);
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

/* As the caller sees it: inside their home the path starts /home. */
int vfs_user_path(vfs_node_t *node, char *buf, size_t size) {
    const vfs_cred_t *c = cred();
    char real[512], home[512];
    uint64_t hn;
    if (!c || !c->home || vfs_getcwd(node, real, sizeof(real)) != 0 ||
        vfs_getcwd(c->home, home, sizeof(home)) != 0) {
        return vfs_getcwd(node, buf, size);
    }
    hn = str_len(home);
    for (uint64_t i = 0; i < hn; i++) {
        if (real[i] != home[i]) return vfs_getcwd(node, buf, size);
    }
    if (real[hn] != 0 && real[hn] != '/') return vfs_getcwd(node, buf, size);
    if (5 + str_len(real + hn) + 1 > size) return -1;
    copy_bytes(buf, "/home", 5);
    copy_bytes(buf + 5, real + hn, str_len(real + hn) + 1);
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
    char leaf[256];
    vfs_node_t *parent = resolve_parent(cwd, path, leaf, sizeof(leaf), 0);
    vfs_node_t *node;

    if (!parent || !valid_name(leaf) || find_child(parent, leaf) || !allowed(parent, VFS_OP_CREATE)) {
        return -1;
    }
    if (external_op(VFS_EXT_MKDIR, parent, leaf, 0, 0) != 0) {
        return -1;
    }

    node = new_node(leaf, VFS_NODE_DIR, 0);
    if (!node) {
        return -1;
    }
    node->owner = cred() ? cred()->uid : VFS_NOBODY;
    append_child(parent, node);
    parent->modified = vfs_tick++;
    return vfs_sync();
}

int vfs_create(vfs_node_t *cwd, const char *path) {
    char leaf[256];
    vfs_node_t *parent = resolve_parent(cwd, path, leaf, sizeof(leaf), 0);
    vfs_node_t *node;


    if (!parent || !valid_name(leaf)) {
        return -1;
    }

    node = find_child(parent, leaf);
    if (node) {
        return node->type == VFS_NODE_FILE ? 0 : -1;
    }

    if (!allowed(parent, VFS_OP_CREATE)) {
        return -1;
    }
    if (external_op(VFS_EXT_WRITE, parent, leaf, "", 0) != 0) {
        return -1;
    }
    node = new_node(leaf, VFS_NODE_FILE, 0);
    if (!node) {
        return -1;
    }
    node->owner = cred() ? cred()->uid : VFS_NOBODY;
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

    if (!node || node == vfs_root_node || !node->parent || subtree_has_readonly(node) ||
        !allowed(node, VFS_OP_MODIFY)) {
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

    if (!node || node->type != VFS_NODE_FILE || node->readonly || !allowed(node, VFS_OP_MODIFY)) {
        return -1;
    }
    if (node->mount_id && external_op(VFS_EXT_WRITE, node->parent, node->name, data, size) != 0) {
        return -1;
    }
    /* Mounted files already live on disk. Keeping a second, growing RAM copy
     * makes streaming a model exhaust the kernel heap after a few chunks. */
    if (node->mount_id && vfs_external_hook) {
        if (node->data) kfree(node->data);
        node->data = 0; node->cap = 0; node->size = size; node->lazy = 1;
        node->modified = vfs_tick++;
        return 0;
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

    if (!node || node->type != VFS_NODE_FILE || node->readonly || !allowed(node, VFS_OP_MODIFY)) {
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
    if (node->mount_id && vfs_external_hook) {
        if (node->data) kfree(node->data);
        node->data = 0; node->cap = 0; node->lazy = 1;
        if (new_end > node->size) node->size = new_end;
        node->modified = vfs_tick++;
        return 0;
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
    if (!node || node->type != VFS_NODE_FILE || node->readonly || !allowed(node, VFS_OP_MODIFY)) {
        return -1;
    }
    if (len == node->size) {
        return 0;
    }
    if (external_range(VFS_EXT_TRUNCATE, node, 0, len, 0) != 0) {
        return -1;
    }
    if (node->mount_id && vfs_external_hook) {
        if (node->data) kfree(node->data);
        node->data = 0; node->cap = 0; node->size = len; node->lazy = 1;
        node->modified = vfs_tick++;
        return 0;
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
    char leaf[256];
    vfs_node_t *parent;
    vfs_node_t *node;

    if (!vfs_root_node || !path || !*path) {
        return -1;
    }

    parent = resolve_parent_raw(vfs_root_node, path, leaf, sizeof(leaf), 1);
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
    char leaf[256];
    vfs_node_t *parent;
    vfs_node_t *node;

    if (!vfs_root_node || !path || !*path) {
        return -1;
    }

    parent = resolve_parent_raw(vfs_root_node, path, leaf, sizeof(leaf), 1);
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
        node = resolve_raw(vfs_root_node, path);
        if (!node) return -1;
        node->lazy_dir = 1;
        node->ext_ref = ref;
        return 0;
    }
    if (vfs_import_lazy(path, size, readonly) != 0) return -1;
    node = resolve_raw(vfs_root_node, path);
    if (!node) return -1;
    node->ext_ref = ref;
    return 0;
}

int vfs_import_lazy(const char *path, uint64_t size, uint8_t readonly) {
    vfs_node_t *node;
    if (vfs_import_node(path, VFS_NODE_FILE, readonly, "", 0, 0, 0, 0) != 0) return -1;
    node = resolve_raw(vfs_root_node, path);
    if (!node) return -1;
    if (node->data) kfree(node->data);
    node->data = 0;
    node->size = size;
    node->lazy = 1;
    node->cap = 0;
    return 0;
}

int vfs_set_mount(const char *path, uint8_t mount_id) {
    vfs_node_t *node = resolve_raw(vfs_root_node, path);
    if (!node || node->type != VFS_NODE_DIR) return -1;
    node->mount_id = mount_id;
    return 0;
}

int vfs_detach_tree(const char *path) {
    vfs_node_t *node = resolve_raw(vfs_root_node, path);
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

/* Moves or renames a node.  An existing target file (or empty directory)
 * is replaced, as with POSIX rename().  Volume-backed nodes go through the
 * external hook so the filesystem driver renames on disk. */
int vfs_rename(vfs_node_t *cwd, const char *from, const char *to) {
    char leaf[256];
    vfs_node_t *node = vfs_resolve(cwd, from);
    vfs_node_t *dest = resolve_parent(cwd, to, leaf, sizeof(leaf), 0);
    vfs_node_t *existing;
    vfs_node_t **link;
    char *name;

    if (!node || !dest || node == vfs_root_node || !node->parent || !valid_name(leaf) ||
        dest->type != VFS_NODE_DIR || subtree_has_readonly(node) || dest->readonly ||
        !allowed(node, VFS_OP_MODIFY) || !allowed(dest, VFS_OP_CREATE)) {
        return -1;
    }
    for (vfs_node_t *p = dest; p; p = p->parent) {
        if (p == node) return -1;
    }
    if (node->mount_id != dest->mount_id) return -1;
    existing = find_child(dest, leaf);
    if (existing == node) return 0;
    if (existing) {
        if (existing->type != node->type || existing->first_child || existing->readonly ||
            !allowed(existing, VFS_OP_MODIFY)) return -1;
    }
    if (node->mount_id) {
        char src[512], dst[512];
        if (!vfs_external_hook || build_path(node->parent, node->name, src, sizeof(src)) != 0 ||
            build_path(dest, leaf, dst, sizeof(dst)) != 0 ||
            vfs_external_hook(VFS_EXT_RENAME, node->mount_id, src, dst, 0, 0) != 0) {
            return -1;
        }
    }
    if (existing) {
        link = &dest->first_child;
        while (*link && *link != existing) link = &(*link)->next_sibling;
        if (*link) *link = existing->next_sibling;
    }
    name = dup_cstr(leaf);
    if (!name) return -1;
    link = &node->parent->first_child;
    while (*link && *link != node) link = &(*link)->next_sibling;
    if (!*link) return -1;
    *link = node->next_sibling;
    node->parent->modified = vfs_tick++;
    node->next_sibling = 0;
    node->name = name;
    append_child(dest, node);
    dest->modified = vfs_tick++;
    return node->mount_id ? 0 : vfs_sync();
}
