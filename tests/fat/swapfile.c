/* Host test of /swapfile's making (kernel/fs/fatfs.c: fatfs_create_reserved
 * and fatfs_extents) on a FAT32 image whose free space is in pieces:
 *   - it gets no more than asked, leaves keep_free and half of what was free,
 *     and is no more pieces than allowed;
 *   - fatfs_extents gives its clusters in file order: a pattern written to
 *     those sectors directly (as swap does) reads back through the file;
 *   - the other files are untouched, and fsck.fat finds nothing wrong, after
 *     making it, refusing a second one with no room, and removing it.
 *
 *   swapfile <image>     (an empty FAT32 image; swapfile.sh makes it) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/fs/fatfs.h"

#define MB (1024ULL * 1024ULL)

static FILE *img;
static block_device_t dev;
static char *image_path;
static int failures;

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

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* the other files: written until the disk is nearly full, then every other
 * one removed, so the free space is in pieces of 1 to 13 MB */
#define NF 1200
static struct { char path[32]; uint64_t size; int kept; } F[NF];
static int nfiles;

/* the bytes of a file (seed) from offset at: regenerated, not kept */
static void fill_at(unsigned char *p, uint64_t n, unsigned seed, uint64_t at) {
    for (uint64_t i = 0; i < n; i++) {
        uint64_t k = at + i;
        p[i] = (unsigned char)(seed * 131 + k * 7 + (k >> 9));
    }
}

static int files_intact(fatfs_t *v) {
    static unsigned char want[14 * MB];
    for (int i = 0; i < nfiles; i++) {
        char *d = 0;
        uint64_t n = 0;
        int bad;
        if (!F[i].kept) continue;
        fill_at(want, F[i].size, (unsigned)i + 1, 0);
        bad = fatfs_read(v, F[i].path, &d, &n) != 0 || n != F[i].size || memcmp(d, want, n);
        free(d);
        if (bad) {
            printf("  %s reads back wrong\n", F[i].path);
            return 0;
        }
    }
    return 1;
}

static int fsck_clean(void) {
    char cmd[512];
    fflush(img);
    snprintf(cmd, sizeof cmd, "fsck.fat -n '%s' > /tmp/swapfile-fsck.out 2>&1 || (head -20 /tmp/swapfile-fsck.out; exit 1)",
             image_path);
    return system(cmd) == 0;
}

typedef struct {
    uint64_t lba[64], sectors[64];
    int n;
    uint64_t total;
    int backwards;
} runs_t;

static int add_run(uint64_t lba, uint64_t sectors, void *ctx) {
    runs_t *r = (runs_t *)ctx;
    if (r->n && lba < r->lba[r->n - 1]) r->backwards = 1;
    if (r->n == 64) return -1;
    r->lba[r->n] = lba;
    r->sectors[r->n] = sectors;
    r->n++;
    r->total += sectors * 512;
    return 0;
}

static uint64_t free_bytes(fatfs_t *v) {
    uint32_t free_clusters = 0, highest = 0;
    if (fatfs_usage(v, &free_clusters, &highest) != 0) return 0;
    return (uint64_t)free_clusters * v->cluster_bytes;
}

int main(int argc, char **argv) {
    fatfs_t v;
    fatfs_entry_t e;
    runs_t r = {0};
    uint64_t size, free_before, keep_free = 64 * MB, off = 0;
    static unsigned char buf[14 * MB], chunk[MB];
    if (argc < 2 || !(img = fopen(image_path = argv[1], "r+b"))) {
        fprintf(stderr, "usage: swapfile <FAT32 image>\n");
        return 2;
    }
    fseeko(img, 0, SEEK_END);
    dev.read = rd;
    dev.write = wr;
    dev.sector_size = 512;
    dev.sector_count = (uint64_t)ftello(img) / 512;
    if (fatfs_mount(&v, &dev, 0, dev.sector_count) != 0) {
        puts("FAIL mount");
        return 1;
    }

    for (int i = 0; i < NF && free_bytes(&v) > 20 * MB; i++) {
        snprintf(F[i].path, sizeof F[i].path, "/file-%04d.bin", i);
        F[i].size = (uint64_t)(1 + (i * 7) % 13) * MB + (uint64_t)i * 1000;
        fill_at(buf, F[i].size, (unsigned)i + 1, 0);
        if (fatfs_write(&v, F[i].path, buf, F[i].size) != 0) {
            printf("FAIL writing %s\n", F[i].path);
            return 1;
        }
        F[i].kept = 1;
        nfiles = i + 1;
    }
    printf("     %d files, %llu MB left free\n", nfiles, (unsigned long long)(free_bytes(&v) / MB));
    for (int i = 0; i < nfiles; i += 2) {
        if (fatfs_remove(&v, F[i].path) != 0) {
            printf("FAIL removing %s\n", F[i].path);
            return 1;
        }
        F[i].kept = 0;
    }

    /* a swap file of the whole free space is asked for: half stays free */
    free_before = free_bytes(&v);
    size = 4096 * MB;
    check(fatfs_create_reserved(&v, "/swapfile", &size, 0x06, 64, keep_free) == 0, "made /swapfile");
    printf("     %llu MB free before, got %llu MB\n", (unsigned long long)(free_before / MB),
           (unsigned long long)(size / MB));
    check(size <= free_before / 2 + v.cluster_bytes, "half of what was free is left");
    check(free_bytes(&v) + size == free_before, "the free space went down by its size");
    check(fatfs_lookup(&v, "/swapfile", &e) == 0 && e.size == size, "its entry has the size it got");
    check((e.attr & 0x06) == 0x06, "it is hidden and a system file");
    check(fatfs_extents(&v, e.cluster, e.size, add_run, &r) == 0 && r.total == size, "its clusters cover it");
    check(!r.backwards, "it runs forward on the disk");
    check(r.n > 1 && r.n <= 64, "it is in pieces, 64 at most");
    check(size + 4 * MB >= free_before / 2 || r.n == 64, "it got that half, unless 64 pieces could not hold it");
    printf("     %d runs\n", r.n);
    check(files_intact(&v), "the other files are untouched");
    check(fsck_clean(), "fsck.fat: made");

    /* written directly, as swap does; read through the file system */
    for (int i = 0; i < r.n; i++) {
        uint64_t bytes = r.sectors[i] * 512;
        for (uint64_t done = 0; done < bytes; done += MB) {
            uint64_t k = bytes - done < MB ? bytes - done : MB;
            fill_at(chunk, k, 99, off + done);
            if (wr(0, r.lba[i] + done / 512, (uint32_t)(k / 512), chunk) != 0) {
                puts("FAIL raw write");
                return 1;
            }
        }
        off += bytes;
    }
    fflush(img);
    {
        fatfs_t fresh;
        fatfs_hint_t hint = {0};
        int ok = fatfs_mount(&fresh, &dev, 0, dev.sector_count) == 0 && fatfs_lookup(&fresh, "/swapfile", &e) == 0 &&
                 e.size == size;
        for (uint64_t at = 0; ok && at < size; at += MB) {
            uint64_t k = size - at < MB ? size - at : MB;
            fill_at(buf, k, 99, at);
            ok = fatfs_read_range(&fresh, &e, at, chunk, k, &hint) == (int64_t)k && !memcmp(chunk, buf, k);
        }
        check(ok, "what was written to its sectors reads back through the file");
    }
    check(files_intact(&v), "the other files are untouched after writing it");

    /* a second one, when keep_free is more than is free: refused, nothing changes */
    {
        uint64_t s2 = 512 * MB, before = free_bytes(&v);
        check(fatfs_create_reserved(&v, "/swapfile2", &s2, 0x06, 64, before + MB) != 0 &&
                  fatfs_lookup(&v, "/swapfile2", &e) != 0 && free_bytes(&v) == before,
              "no room: refused, and nothing changed");
        check(fatfs_create_reserved(&v, "/swapfile", &s2, 0x06, 64, 0) != 0, "an existing one is not made again");
    }
    check(fsck_clean(), "fsck.fat: refused");

    /* in at most 3 pieces: the 3 largest free runs */
    {
        runs_t r3 = {0};
        uint64_t s3 = 4096 * MB;
        check(fatfs_create_reserved(&v, "/swap3", &s3, 0x06, 3, 0) == 0 && fatfs_lookup(&v, "/swap3", &e) == 0 &&
                  fatfs_extents(&v, e.cluster, e.size, add_run, &r3) == 0 && r3.n <= 3 && r3.total == s3,
              "max_runs 3: three runs at most");
        check(s3 <= 3 * 14 * MB, "max_runs 3: no more than the three largest pieces");
        printf("     %llu MB in %d runs\n", (unsigned long long)(s3 / MB), r3.n);
        check(fatfs_remove(&v, "/swap3") == 0, "removed /swap3");
    }

    /* a small one: exactly what was asked, in clusters */
    {
        uint64_t s4 = 3 * MB + 5;
        check(fatfs_create_reserved(&v, "/small", &s4, 0, 64, 0) == 0 && s4 == ((3 * MB + 5 + v.cluster_bytes - 1) /
                                                                               v.cluster_bytes) * v.cluster_bytes,
              "a small one gets what was asked, rounded up to clusters");
        check(fatfs_remove(&v, "/small") == 0, "removed /small");
    }

    check(fatfs_remove(&v, "/swapfile") == 0 && free_bytes(&v) == free_before, "removing it gives the space back");
    check(files_intact(&v), "the other files are untouched at the end");
    check(fsck_clean(), "fsck.fat: removed");
    printf("%s\n", failures ? "swapfile: FAILURES" : "swapfile: ALL PASS");
    return failures ? 1 : 0;
}
