/* Host test of kernel/fs/swapfile.c, the making of /swapfile at boot, on a
 * FAT32 image standing in for ICDA's partition (the rest of the kernel is
 * stubbed: swap_on records the runs it is given):
 *   - the first start makes it, writes its header page and swaps on the
 *     rest of it;
 *   - the next start uses the same file without touching the FAT;
 *   - a size in /etc/icda/swap makes it again, once, at that size;
 *   - a size the disk has no room for is not tried again at every start;
 *   - swapfile_remove gives the room back; fsck.fat finds nothing wrong;
 *   - ICDA on a USB disk gets none.
 *
 *   swapfile-boot <image>     (an empty FAT32 image; swapfile.sh makes it) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/fs/fatfs.h"
#include "../../kernel/fs/rootfs.h"
#include "../../kernel/fs/swaparea.h"
#include "../../kernel/memory/swap.h"

#define MB (1024ULL * 1024ULL)

static FILE *img;
static char *image_path;
static block_device_t dev;
static partition_info_t part;
static int failures;
static uint64_t ram_frames;

static int rd(void *c, uint64_t l, uint32_t n, void *b) {
    (void)c;
    if (fseeko(img, (off_t)l * 512, SEEK_SET) != 0) return -1;
    return fread(b, 512, n, img) == n ? 0 : -1;
}

static int wr(void *c, uint64_t l, uint32_t n, const void *b) {
    (void)c;
    if (fseeko(img, (off_t)l * 512, SEEK_SET) != 0) return -1;
    return fwrite(b, 512, n, img) == n ? 0 : -1;
}

/* ---- the kernel, stubbed ---- */
int rootfs_active(void) { return 1; }
const partition_info_t *rootfs_partition(void) { return &part; }
uint64_t pmm_usable_frames(void) { return ram_frames; }
void serial_write(const char *s) { printf("     %s", s); }

static swap_run_t got_runs[SWAP_MAX_RUNS];
static int got_n, swap_calls;
int swap_on(block_device_t *d, const swap_run_t *runs, int n, const char *what) {
    (void)what;
    if (d != &dev) return -1;
    swap_calls++;
    got_n = n;
    memcpy(got_runs, runs, sizeof(swap_run_t) * (size_t)n);
    return 0;
}

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int fsck_clean(void) {
    char cmd[512];
    fflush(img);
    snprintf(cmd, sizeof cmd, "fsck.fat -n '%s' > /tmp/swapboot-fsck.out 2>&1 || (head -20 /tmp/swapboot-fsck.out; exit 1)",
             image_path);
    return system(cmd) == 0;
}

static uint64_t swap_bytes(void) {
    uint64_t b = 0;
    for (int i = 0; i < got_n; i++) b += got_runs[i].pages * 4096;
    return b;
}

/* the file's size and first cluster, 0 if there is none */
static uint64_t file_size(uint32_t *first) {
    fatfs_t v;
    fatfs_entry_t e;
    if (fatfs_mount(&v, &dev, 0, dev.sector_count) != 0 || fatfs_lookup(&v, "/swapfile", &e) != 0) return 0;
    if (first) *first = e.cluster;
    return e.size;
}

static uint64_t fat_sum(void) {
    fatfs_t v;
    uint64_t sum = 0;
    static uint8_t s[512];
    if (fatfs_mount(&v, &dev, 0, dev.sector_count) != 0) return 0;
    for (uint32_t i = 0; i < v.fat_sectors; i++) {
        rd(0, v.fat_lba + i, 1, s);
        for (int k = 0; k < 512; k += 8) sum = sum * 31 + *(uint64_t *)(s + k);
    }
    return sum;
}

int main(int argc, char **argv) {
    uint32_t first1 = 0, first2 = 0;
    uint64_t size1, before;
    if (argc < 2 || !(img = fopen(image_path = argv[1], "r+b"))) {
        fprintf(stderr, "usage: swapfile-boot <FAT32 image>\n");
        return 2;
    }
    fseeko(img, 0, SEEK_END);
    dev.name = "sata0";
    dev.read = rd;
    dev.write = wr;
    dev.sector_size = 512;
    dev.sector_count = (uint64_t)ftello(img) / 512;
    part.device = &dev;
    part.start_lba = 0;
    part.sector_count = dev.sector_count;
    ram_frames = (1536 * MB - 37 * 4096) / 4096;            /* 1.5 GB less what firmware keeps */

    /* the first start: 1.5 GB of memory, rounded up to 1.5 GB; the image
     * (5 GB) has room for that with 2 GB left over */
    check(swapfile_boot(0) == 0 && swap_calls == 1, "first start: swap on /swapfile");
    size1 = file_size(&first1);
    check(size1 == 1536 * MB, "it is as large as the memory (rounded up to 256 MB)");
    check(swap_bytes() == size1 - 4096, "swap gets all of it but the header page");
    {
        uint8_t h[512];
        rd(0, got_runs[0].lba - 8, 1, h);
        check(!memcmp(h, "ICDA swap file 1, made for 1610612736", 37), "the header page says what it was made for");
    }
    check(fsck_clean(), "fsck.fat: made");

    /* the next start, a few pages fewer usable: the same file, the FAT untouched */
    ram_frames -= 3;
    before = fat_sum();
    check(swapfile_boot(0) == 0 && swap_calls == 2 && file_size(&first2) == size1 && first2 == first1,
          "next start: the same file");
    check(fat_sum() == before, "next start: the FAT is not written");

    /* a size asked for: made again at that size, once */
    check(swapfile_boot(300) == 0 && file_size(0) == 300 * MB && swap_bytes() == 300 * MB - 4096,
          "/etc/icda/swap 300: made again at 300 MB");
    before = fat_sum();
    check(swapfile_boot(300) == 0 && fat_sum() == before, "and kept at the next start");

    /* more than there is room for: what fits, and not made again at every start */
    check(swapfile_boot(4000) == 0, "/etc/icda/swap 4000: swap on what fits");
    printf("     got %llu MB\n", (unsigned long long)(file_size(0) / MB));
    check(file_size(0) < 4000 * MB && file_size(0) >= 256 * MB, "less than asked: the disk keeps 2 GB");
    before = fat_sum();
    check(swapfile_boot(4000) == 0 && fat_sum() == before, "not made again at the next start");
    check(fsck_clean(), "fsck.fat: sizes changed");

    /* swap turned off: the file goes */
    swapfile_remove();
    check(file_size(0) == 0, "swapfile_remove: it is gone");
    check(fsck_clean(), "fsck.fat: removed");

    /* on a USB disk: none */
    dev.name = "usb0";
    swap_calls = 0;
    check(swapfile_boot(0) != 0 && swap_calls == 0 && file_size(0) == 0, "ICDA on a USB disk: no swap file");
    dev.name = "sata0";

    printf("%s\n", failures ? "swapfile-boot: FAILURES" : "swapfile-boot: ALL PASS");
    return failures ? 1 : 0;
}
