/* /swapfile: swap on an installed system's own partition.
 *
 * It is made once, on the fewest runs of free clusters, and never written
 * through the file system afterwards: swap reads and writes its sectors
 * itself (memory/swap.c).  Nothing but the kernel may open it, since other
 * programs' memory is in it (vfs.c refuses it to programs, administrators
 * too).  Its size: what /etc/icda/swap says, or as much as the memory, from
 * 1 GB to just under 4 GB (FAT32's largest file); never so much that less
 * than 2 GB of the partition, or half of what is free, would be left.  Not
 * on a USB disk: flash would wear, and slowly.
 *
 * Its first page says what size it was made for, and swap leaves it alone;
 * the file is made again only when a different size is wanted (one the disk
 * had no room for is not tried again at every start).  When swap is turned
 * off, or a swap partition is used instead, the file goes. */

#include "swaparea.h"
#include "fatfs.h"
#include "rootfs.h"
#include "../memory/swap.h"
#include "../memory/pmm.h"
#include "../memory/heap.h"
#include "../drivers/serial/serial.h"

#define SWAPFILE   "/swapfile"
#define ATTR_HIDDEN_SYSTEM 0x06
#define MB         (1024ULL * 1024ULL)
#define GB         (1024ULL * MB)
#define MAGIC      "ICDA swap file 1, made for "

typedef struct {
    swap_run_t runs[SWAP_MAX_RUNS];
    int        n;
    uint32_t   spp;
    int        too_many;
} runs_t;

static uint8_t header[4096];

static int add_run(uint64_t lba, uint64_t sectors, void *ctx) {
    runs_t *r = (runs_t *)ctx;
    uint64_t pages = sectors / r->spp;
    if (!pages) return 0;
    if (r->n && r->runs[r->n - 1].lba + r->runs[r->n - 1].pages * r->spp == lba) {
        r->runs[r->n - 1].pages += pages;
        return 0;
    }
    if (r->n == SWAP_MAX_RUNS) {
        r->too_many = 1;                 /* the rest goes unused */
        return 0;
    }
    r->runs[r->n].lba = lba;
    r->runs[r->n].pages = pages;
    r->n++;
    return 0;
}

static uint64_t wanted_size(int wanted_mb) {
    uint64_t ram = pmm_usable_frames() * 4096ULL, want;
    if (wanted_mb > 0) return (uint64_t)wanted_mb * MB;
    /* in 256 MB steps: what the firmware keeps varies a little between starts */
    want = (ram + 256 * MB - 1) / (256 * MB) * (256 * MB);
    if (want < 1 * GB) want = 1 * GB;
    if (want > 4 * GB - 64 * 1024) want = 4 * GB - 64 * 1024;
    return want;
}

static int is_usb(const block_device_t *d) {
    const char *n = d->name;
    return n && n[0] == 'u' && n[1] == 's' && n[2] == 'b';
}

/* ICDA's partition, mounted (to be freed), or 0 */
static fatfs_t *mount_root(void) {
    const partition_info_t *part = rootfs_partition();
    fatfs_t *v;
    if (!rootfs_active() || !part || !part->device || !part->device->write || !part->device->sector_size) return 0;
    v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
    if (!v) return 0;
    if (fatfs_mount_part(v, part) != 0) {
        kfree(v);
        return 0;
    }
    return v;
}

static int file_runs(fatfs_t *v, const fatfs_entry_t *e, runs_t *r) {
    r->n = 0;
    r->too_many = 0;
    r->spp = 4096 / v->dev->sector_size;
    if (!r->spp || fatfs_extents(v, e->cluster, e->size, add_run, r) != 0 || !r->n) return -1;
    return 0;
}

/* the size the file was made for, from its first page; 0 if it does not say */
static uint64_t made_for(block_device_t *d, const runs_t *r) {
    const char *m = MAGIC;
    uint64_t size = 0;
    int i = 0;
    if (d->read(d->context, r->runs[0].lba, r->spp, header) != 0) return 0;
    for (; m[i]; i++)
        if (header[i] != (uint8_t)m[i]) return 0;
    while (i < 64 && header[i] >= '0' && header[i] <= '9') size = size * 10 + (uint64_t)(header[i++] - '0');
    return size;
}

static int write_header(block_device_t *d, const runs_t *r, uint64_t size) {
    const char *m = MAGIC;
    char digits[24];
    int i = 0, k = 0;
    for (uint32_t j = 0; j < sizeof(header); j++) header[j] = 0;
    for (; m[i]; i++) header[i] = (uint8_t)m[i];
    do digits[k++] = (char)('0' + size % 10); while (size /= 10);
    while (k) header[i++] = (uint8_t)digits[--k];
    header[i] = '\n';
    return d->write(d->context, r->runs[0].lba, r->spp, header);
}

void swapfile_remove(void) {
    fatfs_t *v = mount_root();
    fatfs_entry_t e;
    if (!v) return;
    if (fatfs_lookup(v, SWAPFILE, &e) == 0 && fatfs_remove(v, SWAPFILE) == 0)
        serial_write("[swap] /swapfile removed: its room is free again\n");
    kfree(v);
}

int swapfile_boot(int wanted_mb) {
    static runs_t r;
    fatfs_t *v;
    fatfs_entry_t e;
    block_device_t *d;
    uint64_t want;
    if (!rootfs_partition() || !rootfs_partition()->device) return -1;
    if (is_usb(rootfs_partition()->device)) {
        serial_write("[swap] ICDA is on a USB disk: no swap file\n");
        return -1;
    }
    v = mount_root();
    if (!v) return -1;
    d = v->dev;
    want = wanted_size(wanted_mb);
    if (fatfs_lookup(v, SWAPFILE, &e) == 0) {
        /* the one made before, unless it was made for another size */
        if (file_runs(v, &e, &r) == 0 && made_for(d, &r) == want) goto on;
        serial_write("[swap] /swapfile is made again for the size wanted now\n");
        if (fatfs_remove(v, SWAPFILE) != 0) {
            kfree(v);
            return -1;
        }
    }
    {
        /* half of what is free at most, and 2 GB left over */
        uint64_t size = want;
        if (fatfs_create_reserved(v, SWAPFILE, &size, ATTR_HIDDEN_SYSTEM, SWAP_MAX_RUNS, 2 * GB) != 0 ||
            fatfs_lookup(v, SWAPFILE, &e) != 0) {
            serial_write("[swap] no room for /swapfile\n");
            kfree(v);
            return -1;
        }
        if (size < 256 * MB || file_runs(v, &e, &r) != 0 || write_header(d, &r, want) != 0) {
            /* too little to be worth it (or unreadable): given back */
            (void)fatfs_remove(v, SWAPFILE);
            serial_write("[swap] too little room for /swapfile\n");
            kfree(v);
            return -1;
        }
    }
on:
    kfree(v);
    /* the first page is the header */
    r.runs[0].lba += r.spp;
    if (--r.runs[0].pages == 0) {
        for (int i = 1; i < r.n; i++) r.runs[i - 1] = r.runs[i];
        r.n--;
    }
    return r.n ? swap_on(d, r.runs, r.n, SWAPFILE) : -1;
}
