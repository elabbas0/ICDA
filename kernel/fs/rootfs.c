/* "/" on ICDA's own partition (see rootfs.h). */
#include "rootfs.h"
#include "fatfs.h"
#include "persistfs.h"
#include "install.h"
#include "volumes.h"
#include "vfs.h"
#include "../memory/heap.h"
#include "../drivers/serial/serial.h"

#define OLD_IMAGE    "/ICDAROOT.BIN"
#define OLD_IMAGE_KEPT "/ICDAROOT.OLD"
#define OLD_SYSTEM   "/SYSTEM"
#define OLD_RELEASE  "/SYSTEM/etc/icda-release.txt"
#define KERNEL_PATH  "/EFI/ICDA/KERNEL.BIN"
#define ENV_PATH     "/EFI/ICDA/GRUBENV"
#define OLD_BACKUP_LIST "/UPDATE/BACKUP/LIST.TXT"
#define PATH_CAP     512

static int active;
static int boot_on_esp;
static int root_index = -1;
static const partition_info_t *icda_part, *esp_part;

int rootfs_active(void) { return active; }
const partition_info_t *rootfs_partition(void) { return icda_part; }
const partition_info_t *rootfs_esp(void) { return esp_part; }
int rootfs_boot_on_esp(void) { return boot_on_esp; }

/* ---- small helpers -------------------------------------------------------------- */

static uint64_t slen(const char *s) { uint64_t n = 0; while (s && s[n]) n++; return n; }

static void scopy(char *d, const char *s, uint64_t cap) {
    uint64_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static void scat(char *d, const char *s, uint64_t cap) {
    uint64_t n = slen(d);
    for (uint64_t i = 0; s[i] && n + 1 < cap; i++) d[n++] = s[i];
    d[n] = 0;
}

static void say(const char *a, const char *b) {
    serial_write("[rootfs] ");
    serial_write(a);
    if (b) serial_write(b);
    serial_write("\n");
}

static void say_num(const char *a, uint64_t v) {
    char t[24], o[24];
    int n = 0, k = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = t[--n];
    o[k] = 0;
    say(a, o);
}

static int has(fatfs_t *v, const char *path) {
    fatfs_entry_t e;
    return fatfs_lookup(v, path, &e) == 0;
}

static int is_dir(fatfs_t *v, const char *path) {
    fatfs_entry_t e;
    return fatfs_lookup(v, path, &e) == 0 && (e.attr & FATFS_ATTR_DIR);
}

/* every folder on the way to path (not path itself) */
static void make_parents(fatfs_t *v, const char *path) {
    char p[PATH_CAP];
    scopy(p, path, sizeof(p));
    for (uint64_t i = 1; p[i]; i++) {
        if (p[i] != '/') continue;
        p[i] = 0;
        if (!has(v, p)) (void)fatfs_mkdir(v, p);
        p[i] = '/';
    }
}

static int mount_part(fatfs_t *v, const partition_info_t *p) {
    return p ? fatfs_mount_part(v, p) : -1;
}

/* ---- finding the partitions ----------------------------------------------------------- */

/* 2: the 1.9 layout, 1: an earlier one, 0: not ICDA's */
static int layout_of(fatfs_t *v) {
    if (has(v, ROOTFS_LAYOUT_PATH)) return 2;
    if (has(v, OLD_IMAGE) || has(v, OLD_RELEASE)) return 1;
    return 0;
}

static void find_partitions(int *layout, int *index) {
    fatfs_t *v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
    *layout = 0;
    *index = -1;
    icda_part = esp_part = 0;
    if (!v) return;
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *p = partition_get(i);
        int l;
        if (!p || p->fs_hint != PARTITION_FS_FAT32) continue;
        if (p->role == PARTITION_ROLE_EFI) {
            if (!esp_part) esp_part = p;
            continue;
        }
        if (p->role != PARTITION_ROLE_SYSTEM && p->role != PARTITION_ROLE_DATA) continue;
        if (fatfs_mount_part(v, p) != 0) continue;
        l = layout_of(v);
        if (l > *layout) {            /* a moved one wins over an old one elsewhere */
            *layout = l;
            *index = (int)i;
            icda_part = p;
        }
    }
    /* prefer the EFI partition on ICDA's own disk */
    if (icda_part) {
        for (uint32_t i = 0; i < partition_count(); i++) {
            const partition_info_t *p = partition_get(i);
            if (p && p->role == PARTITION_ROLE_EFI && p->fs_hint == PARTITION_FS_FAT32 && p->device == icda_part->device) {
                esp_part = p;
                break;
            }
        }
    }
    kfree(v);
}

/* GRUB's count of starts of a fresh update: 1 or 2 while one is on trial */
static int trial(fatfs_t *v) {
    char *data = 0;
    uint64_t size = 0;
    int value = 0;
    if (fatfs_read(v, ENV_PATH, &data, &size) != 0 || !data) return 0;
    for (uint64_t i = 0; i + 11 < size; i++) {
        const char *k = "icda_trial=";
        int j = 0;
        while (k[j] && data[i + j] == k[j]) j++;
        if (k[j]) continue;
        for (i += 11; i < size && data[i] >= '0' && data[i] <= '9'; i++) value = value * 10 + (data[i] - '0');
        break;
    }
    kfree(data);
    return value;
}

/* ---- moving an earlier layout ------------------------------------------------------------ */

static int written, failed;

/* one entry of \ICDAROOT.BIN as a real file or folder */
static int write_entry(const char *path, uint8_t type, const char *data, uint64_t len, void *ctx) {
    fatfs_t *v = (fatfs_t *)ctx;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && (path[4] == '/' || !path[4]))
        return 0;                               /* temporary: not kept */
    make_parents(v, path);
    if (type == VFS_NODE_DIR) {
        if (!has(v, path) && fatfs_mkdir(v, path) != 0) { failed++; say("could not make ", path); }
    } else if (fatfs_write(v, path, data ? data : "", len) != 0) {
        failed++;
        say("could not write ", path);
    } else {
        written++;
    }
    return 0;
}

typedef struct {
    char (*names)[256];
    int count, cap;
} names_t;

static int collect(const fatfs_entry_t *e, void *ctx) {
    names_t *n = (names_t *)ctx;
    if (e->name[0] == '.' && (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0))) return 0;
    if (n->count >= n->cap) return 0;
    scopy(n->names[n->count++], e->name, 256);
    return 0;
}

/* from -> to; a folder that is already there gets the contents one by one,
 * a file that is already there is replaced (the system's copy wins) */
static void move_tree(fatfs_t *v, const char *from, const char *to, int depth) {
    fatfs_entry_t e;
    if (depth > 16 || fatfs_lookup(v, from, &e) != 0) return;
    if (!has(v, to)) {
        make_parents(v, to);
        if (fatfs_rename(v, from, to) != 0) { failed++; say("could not move ", from); }
        return;
    }
    if (!(e.attr & FATFS_ATTR_DIR)) {
        if (is_dir(v, to) || fatfs_remove(v, to) != 0 || fatfs_rename(v, from, to) != 0) {
            failed++;
            say("could not replace ", to);
        }
        return;
    }
    {
        names_t n;
        n.cap = 512;
        n.count = 0;
        n.names = (char (*)[256])kmalloc(sizeof(*n.names) * (uint64_t)n.cap);
        if (!n.names) return;
        (void)fatfs_iterate(v, e.cluster, collect, &n);
        for (int i = 0; i < n.count; i++) {
            char a[PATH_CAP], b[PATH_CAP];
            scopy(a, from, sizeof(a)); scat(a, "/", sizeof(a)); scat(a, n.names[i], sizeof(a));
            scopy(b, to, sizeof(b)); scat(b, "/", sizeof(b)); scat(b, n.names[i], sizeof(b));
            move_tree(v, a, b, depth + 1);
        }
        kfree(n.names);
        (void)fatfs_remove(v, from);        /* empty now */
    }
}

/* a whole file from one partition to another; 0 when it reads back the same size */
static int copy_file(fatfs_t *src, const char *sp, fatfs_t *dst, const char *dp) {
    char *data = 0;
    uint64_t size = 0;
    fatfs_entry_t e;
    int rc = -1;
    if (fatfs_read(src, sp, &data, &size) != 0 || !data) return -1;
    make_parents(dst, dp);
    if (fatfs_write(dst, dp, data, size) == 0 && fatfs_lookup(dst, dp, &e) == 0 && e.size == size) rc = 0;
    kfree(data);
    return rc;
}

/* the kernel and GRUB's environment onto the EFI partition, if it has room */
static void move_boot_files(fatfs_t *v, fatfs_t *esp) {
    fatfs_entry_t k;
    uint32_t free_clusters = 0, highest = 0;
    if (fatfs_lookup(v, KERNEL_PATH, &k) != 0) return;            /* already moved */
    if (fatfs_usage(esp, &free_clusters, &highest) != 0 ||
        (uint64_t)free_clusters * esp->cluster_bytes < (uint64_t)k.size * 2 + 4 * 1024 * 1024) {
        say("the EFI partition has no room for the kernel: it stays on the ICDA partition", 0);
        return;
    }
    if (copy_file(v, KERNEL_PATH, esp, KERNEL_PATH) != 0) {
        say("could not copy the kernel to the EFI partition: it stays on the ICDA partition", 0);
        (void)fatfs_remove(esp, KERNEL_PATH);
        return;
    }
    if (has(v, ENV_PATH) && copy_file(v, ENV_PATH, esp, ENV_PATH) != 0) {
        say("could not copy GRUB's environment: the kernel stays on the ICDA partition", 0);
        (void)fatfs_remove(esp, KERNEL_PATH);
        return;
    }
    /* only now: GRUB looks for the kernel by name and must find this copy */
    (void)fatfs_remove(v, KERNEL_PATH);
    (void)fatfs_remove(v, ENV_PATH);
    (void)fatfs_flush(esp);
    say_num("kernel moved to the EFI partition, bytes ", k.size);
}

static void migrate(fatfs_t *v, fatfs_t *esp) {
    char *bundle = 0;
    uint64_t size = 0;
    int32_t swap = -1;
    say("moving this installation to real files (once)", 0);
    written = failed = 0;

    /* 1. personal files out of the image (it stays until the end: running
     *    this again after a power cut writes the same files again) */
    if (has(v, OLD_IMAGE) && system_install_read_root_bundle(icda_part, &bundle, &size, &swap) == 0 && bundle) {
        if (persistfs_each_entry(bundle, size, write_entry, v) != 0) say("the image ended early", 0);
        kfree(bundle);
        say_num("personal files written: ", (uint64_t)written);
    }
    /* 2. the system up from \SYSTEM to "/" */
    if (is_dir(v, OLD_SYSTEM)) {
        names_t n;
        fatfs_entry_t s;
        n.cap = 256;
        n.count = 0;
        n.names = (char (*)[256])kmalloc(sizeof(*n.names) * (uint64_t)n.cap);
        if (n.names && fatfs_lookup(v, OLD_SYSTEM, &s) == 0) {
            (void)fatfs_iterate(v, s.cluster, collect, &n);
            for (int i = 0; i < n.count; i++) {
                char a[PATH_CAP], b[PATH_CAP];
                scopy(a, OLD_SYSTEM "/", sizeof(a)); scat(a, n.names[i], sizeof(a));
                scopy(b, "/", sizeof(b)); scat(b, n.names[i], sizeof(b));
                move_tree(v, a, b, 0);
            }
            (void)fatfs_remove(v, OLD_SYSTEM);
        }
        if (n.names) kfree(n.names);
        say("system files moved to /", 0);
    }
    if (failed) {
        /* nothing more: the next start tries the rest again, still in memory */
        say_num("not finished, problems: ", (uint64_t)failed);
        return;
    }
    /* 3. boot files to the EFI partition */
    if (esp) move_boot_files(v, esp);
    /* 4. the old update backups name \SYSTEM paths: an older kernel could
     *    not use them, nor run this layout; the image is kept, renamed */
    (void)fatfs_remove(v, OLD_BACKUP_LIST);
    if (has(v, OLD_IMAGE)) {
        if (has(v, OLD_IMAGE_KEPT)) (void)fatfs_remove(v, OLD_IMAGE_KEPT);
        (void)fatfs_rename(v, OLD_IMAGE, OLD_IMAGE_KEPT);
    }
    /* 5. done */
    make_parents(v, ROOTFS_LAYOUT_PATH);
    if (fatfs_write(v, ROOTFS_LAYOUT_PATH, "2\n", 2) != 0) say("could not mark the move as done", 0);
    (void)fatfs_flush(v);
    say("moved: / is the ICDA partition from now on", 0);
}

/* ---- starting ---------------------------------------------------------------------------- */

int rootfs_boot(int live) {
    int layout, index;
    fatfs_t *v, *esp = 0;
    if (live) return -1;
    find_partitions(&layout, &index);
    if (!layout) return -1;
    v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
    if (!v || mount_part(v, icda_part) != 0) {
        if (v) kfree(v);
        return -1;
    }
    if (esp_part) {
        esp = (fatfs_t *)kmalloc(sizeof(fatfs_t));
        if (esp && mount_part(esp, esp_part) != 0) { kfree(esp); esp = 0; }
    }
    if (layout == 1) {
        /* An update on trial may still be rolled back to an earlier kernel,
         * which needs the earlier layout: start as before this time. */
        int t = trial(v);
        if (!t && esp) t = trial(esp);
        if (t) {
            say("an update is on trial: the move waits for a healthy start", 0);
        } else {
            migrate(v, esp);
            layout = layout_of(v);
        }
    }
    boot_on_esp = esp && has(esp, KERNEL_PATH) && !has(v, KERNEL_PATH);
    kfree(v);
    if (esp) kfree(esp);
    if (layout != 2) return -1;
    persistfs_set_root_partition(index);      /* updates and the boot log: this partition */
    root_index = index;
    return 0;
}

/* After sysupdate_boot (an update rolled back renames files on the disk,
 * which must happen before anything of it is loaded). */
int rootfs_mount(void) {
    if (root_index < 0) return -1;
    if (volumes_mount_root(icda_part) != 0) {
        say("could not mount the ICDA partition as /", 0);
        return -1;
    }
    (void)vfs_mkdir_ram("/dev");
    (void)vfs_mkdir_ram("/tmp");
    (void)vfs_mkdir_ram("/volumes");
    if (!vfs_resolve(vfs_root(), "/home")) (void)vfs_mkdir(vfs_root(), "/home");
    active = 1;
    say(boot_on_esp ? "/ is the ICDA partition; boot files on the EFI partition"
                    : "/ is the ICDA partition; boot files on it too", 0);
    return 0;
}

/* the 1.9 layout was found (before "/" is mounted too) */
int rootfs_new_layout(void) {
    return root_index >= 0;
}
