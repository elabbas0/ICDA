/* "/" on a disk (ICDA 1.9): folders are listed when looked at, files read
 * when opened and freed again when unused, /dev and /tmp stay in memory,
 * a file the disk has wins over a copy built into the kernel. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/fs/vfs.c"
void *kmalloc(size_t n) { return malloc(n); }
void *kcalloc(size_t n, size_t s) { return calloc(n, s); }
void kfree(void *p) { free(p); }
static uint64_t ticks = 1;
uint64_t sched_ticks(void) { return ticks; }

static int listed_root, listed_apps, reads, writes;
static char last_write[128];

static int64_t load(uint8_t mount, const char *path, uint64_t ref, uint64_t off, char *buf, uint64_t len) {
    (void)mount; (void)ref;
    reads++;
    if (!strcmp(path, "/apps/hello.app")) { memcpy(buf, "ELF!" + off, len); return (int64_t)len; }
    if (!strcmp(path, "/etc/motd.txt")) { memcpy(buf, "disk" + off, len); return (int64_t)len; }
    return -1;
}

static int list_dir(uint8_t mount, const char *path, uint64_t ref) {
    (void)mount; (void)ref;
    if (!strcmp(path, "/")) {
        listed_root++;
        vfs_import_ref("/apps", VFS_NODE_DIR, 0, 0, 7);
        vfs_import_ref("/etc", VFS_NODE_DIR, 0, 0, 8);
        vfs_import_ref("/home", VFS_NODE_DIR, 0, 0, 9);
        vfs_import_ref("/dev", VFS_NODE_DIR, 0, 0, 10);     /* a stale one on the disk */
        return 0;
    }
    if (!strcmp(path, "/apps")) {
        listed_apps++;
        vfs_import_ref("/apps/hello.app", VFS_NODE_FILE, 4, 0, 11);
        return 0;
    }
    if (!strcmp(path, "/etc")) {
        vfs_import_ref("/etc/motd.txt", VFS_NODE_FILE, 4, 0, 12);
        return 0;
    }
    return 0;
}

static int external(int op, uint8_t mount, const char *path, const char *data, uint64_t size, uint64_t off) {
    (void)op; (void)mount; (void)data; (void)size; (void)off;
    writes++;
    snprintf(last_write, sizeof(last_write), "%s", path);
    return 0;
}

int main(void) {
    vfs_node_t *root;
    uint64_t size = 0;
    vfs_init();
    root = vfs_root();
    vfs_set_loader(load);
    vfs_set_dir_loader(list_dir);
    vfs_set_external_hook(external);
    vfs_mount_root(1, 2);
    assert(vfs_root_is_disk() && listed_root == 0);           /* nothing read yet */

    assert(!vfs_mkdir_ram("/dev") && !vfs_mkdir_ram("/tmp"));
    assert(listed_root == 1 && listed_apps == 0);              /* "/" listed once, /apps not yet */
    assert(vfs_resolve(root, "/apps/hello.app") && listed_apps == 1);

    /* a file is read when wanted, kept a while, freed when idle */
    assert(!strncmp(vfs_read(root, "/apps/hello.app", &size), "ELF!", 4) && size == 4 && reads == 1);
    assert(vfs_read(root, "/apps/hello.app", &size) && reads == 1 && vfs_cache_size() == 4);
    ticks += 3001;
    vfs_cache_trim();
    assert(vfs_cache_size() == 0);
    assert(vfs_read(root, "/apps/hello.app", &size) && reads == 2);   /* read again from the disk */

    /* memory folders: nothing goes to the disk, the disk's stale /dev is gone */
    writes = 0;
    assert(!vfs_write(root, "/dev/console", "x", 1) && !vfs_write(root, "/tmp/a", "y", 1) && writes == 0);
    assert(vfs_node_mount_id(vfs_resolve(root, "/tmp/a")) == 0);

    /* the disk: written through, the copy in memory dropped */
    assert(!vfs_write(root, "/home/note", "hi", 2) && writes == 2 && !strcmp(last_write, "/home/note"));
    assert(!vfs_mkdir(root, "/home/docs") && !strcmp(last_write, "/home/docs"));

    /* a seeded file fills in what the disk lacks, never replaces what it has */
    assert(!vfs_seed_readonly("/etc/motd.txt", "seed", 4));
    assert(!strncmp(vfs_read(root, "/etc/motd.txt", &size), "disk", 4));
    assert(!vfs_seed_readonly("/usr/share/fonts/ui.icf", "font", 4));
    writes = 0;
    assert(!strncmp(vfs_read(root, "/usr/share/fonts/ui.icf", &size), "font", 4) && writes == 0);
    assert(vfs_node_mount_id(vfs_resolve(root, "/usr/share/fonts/ui.icf")) == 0);
    ticks += 5000;
    vfs_cache_trim();                                          /* never frees a memory-only file */
    assert(!strncmp(vfs_read(root, "/usr/share/fonts/ui.icf", &size), "font", 4));

    /* a disk root's paths, as a program sees them */
    {
        char p[64];
        assert(!vfs_getcwd(vfs_resolve(root, "/home/docs"), p, sizeof(p)) && !strcmp(p, "/home/docs"));
    }
    puts("disk root: lazy folders, files read when opened and freed when idle, memory /dev and /tmp, disk over seed");
    return 0;
}
