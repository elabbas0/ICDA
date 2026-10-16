/* Users and the file tree: each user's home is /home to them, other homes
 * cannot be entered, the system is read-only, /tmp files belong to their
 * maker, disks are open by kind (removable to all, internal to admins). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/fs/vfs.c"
void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t s) { return calloc(n, s); }
void kfree(void *p) { free(p); }
uint64_t sched_ticks(void) { return 1; }

static vfs_node_t *home_dir, *bob_dir, *cfg_dir, *bob_cfg;
static const vfs_cred_t *who;
static const vfs_cred_t *current(void) { return who; }
static uint32_t owner_of(vfs_node_t *d) { return d == home_dir || d == cfg_dir ? 1000 : d == bob_dir || d == bob_cfg ? 1001 : VFS_NOBODY; }
static int volume_class(uint8_t id) { return id == 1 ? VFS_VOL_REMOVABLE : id == 2 ? VFS_VOL_SYSTEM : VFS_VOL_INTERNAL; }

static int can_read(const char *p) { return vfs_resolve(vfs_root(), p) != 0; }
static int can_write(const char *p) { return vfs_write(vfs_root(), p, "x", 1) == 0; }

int main(void) {
    vfs_node_t *root;
    char path[128];
    vfs_init();
    root = vfs_root();
    /* the system sets things up (no credentials) */
    assert(!vfs_mkdir(root, "/home") && !vfs_mkdir(root, "/home/.config"));
    assert(!vfs_write(root, "/home/.config/secret", "alice", 5));
    assert(!vfs_mkdir(root, "/users") && !vfs_mkdir(root, "/users/bob"));
    assert(!vfs_mkdir(root, "/tmp") && !vfs_mkdir(root, "/apps") && !vfs_write(root, "/apps/x.app", "elf", 3));
    assert(!vfs_mkdir(root, "/etc") && !vfs_mkdir(root, "/etc/icda") && !vfs_write(root, "/etc/icda/users", "h", 1));
    assert(!vfs_mkdir(root, "/volumes"));
    assert(!vfs_mkdir(root, "/volumes/usb") && !vfs_set_mount("/volumes/usb", 1));
    assert(!vfs_mkdir(root, "/volumes/sys") && !vfs_set_mount("/volumes/sys", 2));
    assert(!vfs_mkdir(root, "/volumes/win") && !vfs_set_mount("/volumes/win", 3));
    assert(!vfs_write(root, "/volumes/win/doc", "w", 1) && !vfs_write(root, "/volumes/sys/lib", "s", 1));
    assert(!vfs_mkdir(root, "/cfg") && !vfs_write(root, "/cfg/settings", "owner", 5));
    assert(!vfs_mkdir(root, "/users/bob/.cfg"));
    home_dir = vfs_resolve(root, "/home");
    bob_dir = vfs_resolve(root, "/users/bob");
    cfg_dir = vfs_resolve(root, "/cfg");
    bob_cfg = vfs_resolve(root, "/users/bob/.cfg");
    vfs_set_cred_hook(current);
    vfs_set_home_owner_hook(owner_of);
    vfs_set_volume_class_hook(volume_class);

    vfs_cred_t alice = { 1000, 1, home_dir, cfg_dir }, bob = { 1001, 0, bob_dir, bob_cfg };

    who = &alice;                                   /* the owner, an administrator */
    assert(can_read("/home/.config/secret") && can_write("/home/.config/new"));
    assert(can_write("/cfg/settings") && vfs_resolve(root, "/cfg") == cfg_dir);
    assert(!can_read("/users/bob") && vfs_last_denied());
    assert(!can_read("/etc/icda/users") && !can_read("/etc/icda"));
    assert(can_read("/apps/x.app") && !can_write("/apps/x.app") && !can_write("/apps/y.app"));
    assert(!can_write("/newtop") && vfs_mkdir(root, "/users/eve") != 0);
    assert(can_read("/volumes/win/doc") && can_write("/volumes/win/doc"));   /* admins: internal disks */
    assert(can_write("/volumes/sys/lib") && can_write("/volumes/usb/a"));

    who = &bob;                                     /* a standard user */
    assert(vfs_resolve(root, "/home") == bob_dir);  /* their home is /home */
    assert(can_write("/home/note") && vfs_resolve(root, "/users/bob/note"));
    assert(vfs_user_path(vfs_resolve(root, "/home/note"), path, sizeof(path)) == 0 && !strcmp(path, "/home/note"));
    assert(vfs_resolve(root, "/home/..") == root);
    /* settings: their own /cfg */
    assert(vfs_resolve(root, "/cfg") == bob_cfg && !can_read("/cfg/settings") && can_write("/cfg/settings"));
    assert(vfs_resolve(root, "/users/bob/.cfg/settings") && vfs_resolve(root, "/cfg/..") == root);
    assert(!can_read("/home/.config/secret"));       /* that is alice's /home */
    assert(!can_read("/users/bob/../../home/.config"));
    vfs_last_denied();
    assert(!can_read("/volumes/win/doc") && vfs_last_denied());   /* internal disk: no */
    assert(can_read("/volumes/sys/lib") && !can_write("/volumes/sys/lib"));
    assert(can_write("/volumes/usb/b"));            /* removable: yes */
    assert(vfs_remove(root, "/home") != 0);         /* not their own home itself... */
    assert(can_write("/tmp/bobs"));
    assert(vfs_node_owner(vfs_resolve(root, "/tmp/bobs")) == 1001);

    who = &alice;                                   /* /tmp: only the maker removes */
    assert(vfs_remove(root, "/tmp/bobs") != 0 && vfs_write(root, "/tmp/bobs", "a", 1) != 0);
    assert(can_write("/tmp/alices"));
    who = &bob;
    assert(vfs_remove(root, "/tmp/alices") != 0 && vfs_remove(root, "/tmp/bobs") == 0);
    assert(vfs_rename(root, "/home/note", "/tmp/moved") == 0 && vfs_rename(root, "/tmp/moved", "/apps/z") != 0);

    who = &bob;                                     /* the kernel acting inside bob's call */
    vfs_kernel_begin();
    assert(can_read("/etc/icda/users") && can_write("/apps/w"));
    vfs_kernel_end();
    assert(!can_read("/etc/icda/users"));

    who = 0;                                        /* nobody signed in: the system */
    assert(can_read("/users/bob/note") == 0 && can_read("/home/.config/secret") && can_write("/apps/v"));
    puts("home and settings mapping, private homes, read-only system, /tmp ownership, disk classes and kernel calls passed");
    return 0;
}
