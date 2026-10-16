#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIR = 2
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t type;
    uint8_t readonly;
} vfs_stat_t;

#define VFS_EXT_MKDIR  1
#define VFS_EXT_WRITE  2
#define VFS_EXT_REMOVE 3
#define VFS_EXT_WRITE_AT 4
#define VFS_EXT_TRUNCATE 5
#define VFS_EXT_RENAME   6

typedef int (*vfs_external_fn)(int op, uint8_t mount_id, const char *path, const char *data, uint64_t size,
                               uint64_t off);
typedef int64_t (*vfs_loader_fn)(uint8_t mount_id, const char *path, uint64_t ref, uint64_t off, char *buf,
                                 uint64_t len);
typedef int (*vfs_dir_loader_fn)(uint8_t mount_id, const char *path, uint64_t ref);

/* The official static Codex CLI is about 281 MiB. Whole-file readers such
 * as the ELF loader need room for it; streaming model reads remain lazy. */
#define VFS_LAZY_LOAD_MAX (512ULL * 1024ULL * 1024ULL)

/* ---- users ---------------------------------------------------------------
 * Who a call runs for (kernel/proc/accounts.c fills it in).  No credentials -
 * the kernel itself, or the session manager before anyone signed in - may do
 * anything.  A signed-in user sees their own home as /home, cannot enter
 * other users' homes, internal disks (unless an administrator) or
 * /etc/icda, and may change only their home, /tmp (their own files there)
 * and removable disks. */
#define VFS_NOBODY 0xFFFFFFFFu

typedef struct {
    uint32_t    uid;
    uint8_t     admin;
    vfs_node_t *home;           /* the real directory shown as /home */
    vfs_node_t *cfg;            /* and the one shown as /cfg (settings) */
} vfs_cred_t;

enum { VFS_VOL_REMOVABLE = 0, VFS_VOL_SYSTEM = 1, VFS_VOL_INTERNAL = 2 };

void vfs_set_cred_hook(const vfs_cred_t *(*fn)(void));
void vfs_set_home_owner_hook(uint32_t (*fn)(vfs_node_t *dir));   /* VFS_NOBODY: not a home */
void vfs_set_volume_class_hook(int (*fn)(uint8_t mount_id));
/* the kernel acting for itself inside a user's call (installing, accounts) */
void vfs_kernel_begin(void);
void vfs_kernel_end(void);
/* 1 if the last failure was a permission refusal (and clears it) */
int vfs_last_denied(void);
/* a directory as the calling user sees it (their home as /home) */
int vfs_user_path(vfs_node_t *node, char *buf, size_t size);
uint32_t vfs_node_owner(vfs_node_t *node);
vfs_node_t *vfs_node_parent(vfs_node_t *node);
int vfs_may_modify(vfs_node_t *node);   /* may the caller change it */

int vfs_init(void);
void vfs_set_external_hook(vfs_external_fn fn);
int vfs_set_mount(const char *path, uint8_t mount_id);
/* a disk as "/" (1.9): see kernel/fs/rootfs.c */
void vfs_mount_root(uint8_t mount_id, uint64_t ref);
int vfs_root_is_disk(void);
int vfs_mkdir_ram(const char *path);          /* /dev, /tmp, /volumes on a disk root */
void vfs_cache_trim(void);                    /* frees disk files read whole and unused */
uint64_t vfs_cache_size(void);
int vfs_detach_tree(const char *path);
void vfs_set_loader(vfs_loader_fn fn);
void vfs_set_dir_loader(vfs_dir_loader_fn fn);
int vfs_import_ref(const char *path, uint8_t type, uint64_t size, uint8_t readonly, uint64_t ref);
int vfs_import_lazy(const char *path, uint64_t size, uint8_t readonly);
int64_t vfs_node_read_at(vfs_node_t *node, uint64_t off, char *buf, uint64_t len);
uint8_t vfs_node_is_lazy(vfs_node_t *node);
uint8_t vfs_node_mount_id(vfs_node_t *node);
vfs_node_t *vfs_root(void);
int vfs_seed_readonly(const char *path, const char *data, uint64_t size);
void vfs_set_sync_hook(int (*hook)(void));
int vfs_sync(void);
int vfs_flush(int force);
int vfs_node_truncate(vfs_node_t *node, uint64_t len);
vfs_node_t *vfs_resolve(vfs_node_t *cwd, const char *path);
int vfs_getcwd(vfs_node_t *node, char *buf, size_t size);
int vfs_mkdir(vfs_node_t *cwd, const char *path);
int vfs_create(vfs_node_t *cwd, const char *path);
int vfs_remove(vfs_node_t *cwd, const char *path);
int vfs_rename(vfs_node_t *cwd, const char *from, const char *to);
int vfs_write(vfs_node_t *cwd, const char *path, const char *data, uint64_t size);


int vfs_node_write_at(vfs_node_t *node, uint64_t off, const char *data,
                      uint64_t size);
int vfs_import_node(const char *path, uint8_t type, uint8_t readonly, const char *data, uint64_t size,
                    uint64_t inode, uint64_t created, uint64_t modified);
const char *vfs_read(vfs_node_t *cwd, const char *path, uint64_t *size_out);
int vfs_stat(vfs_node_t *cwd, const char *path, vfs_stat_t *out);
vfs_node_t *vfs_child_at(vfs_node_t *dir, uint64_t index);
uint64_t vfs_child_count(vfs_node_t *dir);
const char *vfs_node_name(vfs_node_t *node);
uint8_t vfs_node_type(vfs_node_t *node);
uint8_t vfs_node_readonly(vfs_node_t *node);
uint64_t vfs_node_size(vfs_node_t *node);
uint64_t vfs_node_inode(vfs_node_t *node);
uint64_t vfs_node_created(vfs_node_t *node);
uint64_t vfs_node_modified(vfs_node_t *node);
const char *vfs_node_data(vfs_node_t *node);

#endif
