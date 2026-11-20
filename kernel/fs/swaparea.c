/* Where swap goes (memory/swap.c does the swapping), the first that works:
 *
 *   - ICDA's own swap partition: "ICDA Swap", which ICDA's disk layout makes
 *     (and the installer names a partition it is given for swap), on the
 *     disk ICDA runs from;
 *   - a swap partition Linux's mkswap prepared ("SWAPSPACE2" at the end of
 *     its first page);
 *   - /swapfile on ICDA's partition (swapfile.c).
 *
 * The first page of a swap partition is left as it is, so Linux can still
 * use it.  One holding a hibernated system ("S1SUSPEND" or the like there)
 * is not touched.  A live start borrows no disk's swap unless icda.swap=on
 * asks.
 *
 * /etc/icda/swap says what is wanted: "off" (the swap file, if any, goes),
 * "auto" (the default), or a size in MB, which makes a swap file of that
 * size the swap area.  icda.swap=off on the kernel's command line turns swap
 * off for one start. */

#include "swaparea.h"
#include "vfs.h"
#include "rootfs.h"
#include "../memory/swap.h"
#include "../memory/pmm.h"
#include "../drivers/storage/partition.h"
#include "../drivers/storage/block.h"
#include "../drivers/serial/serial.h"

static uint8_t first_page[4096];

static int same(const uint8_t *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

static int same_str(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* "off", "auto" or a number of MB: -1 off, 0 auto, else MB */
int swap_area_wanted(void) {
    uint64_t size = 0;
    const char *t = vfs_read(vfs_root(), "/etc/icda/swap", &size);
    long mb = 0;
    uint64_t i = 0;
    if (!t || !size) return 0;
    while (i < size && (t[i] == ' ' || t[i] == '\t')) i++;
    if (size - i >= 3 && t[i] == 'o' && t[i + 1] == 'f' && t[i + 2] == 'f') return -1;
    while (i < size && t[i] >= '0' && t[i] <= '9') mb = mb * 10 + (t[i++] - '0');
    return mb > 0 && mb < 1000000 ? (int)mb : 0;
}

/* a hibernated system's image, by the signatures Linux's hibernation
 * tools leave where mkswap's is (S1SUSPEND, S2SUSPEND, ULSUSPEND, ...) */
static int hibernated(const uint8_t *page) {
    for (int i = 4086; i + 7 <= 4096; i++)
        if (same(page + i, "SUSPEND", 7)) return 1;
    return same(page + 4086, "LINHIB0001", 10);
}

/* ICDA's own swap partition: named so by its layout or installer, on the
 * disk ICDA runs from (any disk on a live start, which asked for swap) */
static int own_partition(const partition_info_t *p, int live) {
    const partition_info_t *root = rootfs_partition();
    if (!same_str(p->name, "ICDA Swap")) return 0;
    return live || (root && root->device == p->device);
}

static int try_partition(const partition_info_t *p, int own) {
    block_device_t *d = p->device;
    uint32_t spp;
    swap_run_t run;
    if (!d || !d->read || !d->write || !d->sector_size || 4096 % d->sector_size) return -1;
    spp = 4096 / d->sector_size;
    if (p->sector_count / spp < 64 + 1) return -1;
    if (d->read(d->context, p->start_lba, spp, first_page) != 0) return -1;
    if (hibernated(first_page)) {
        serial_write("[swap] a hibernated system's swap partition: left alone\n");
        return -1;
    }
    if (!own && !same(first_page + 4086, "SWAPSPACE2", 10)) {
        serial_write("[swap] a swap partition without mkswap's signature: left alone\n");
        return -1;
    }
    run.lba = p->start_lba + spp;               /* the first page stays as it is */
    run.pages = p->sector_count / spp - 1;
    return swap_on(d, &run, 1, own ? "ICDA's swap partition" : "swap partition");
}

void swap_area_boot(int live, int forced_on, int forced_off) {
    int wanted;
    if (forced_off) {
        serial_write("[swap] off for this start (icda.swap=off)\n");
        return;
    }
    wanted = swap_area_wanted();
    if (wanted < 0 && !forced_on) {
        serial_write("[swap] off (/etc/icda/swap)\n");
        if (!live) swapfile_remove();
        return;
    }
    /* a size asked for: the swap file */
    if (!live && wanted > 0 && swapfile_boot(wanted) == 0) return;
    if (!live || forced_on) {
        /* ICDA's own partition first, then one another system made */
        for (int own = 1; own >= 0; own--) {
            for (uint32_t i = 0; i < partition_count(); i++) {
                const partition_info_t *p = partition_get(i);
                if (!p || p->role != PARTITION_ROLE_SWAP || own_partition(p, live) != own) continue;
                if (try_partition(p, own) == 0) {
                    if (!live) swapfile_remove();
                    return;
                }
            }
        }
    }
    if (!live && wanted <= 0) (void)swapfile_boot(wanted);
}
