/* FAT32 stress test for kernel/fs/fatfs.c: random file and folder work the
 * way ICDA does it - a long-lived volume (like the 1.9 move to real files)
 * interleaved with operations that mount the volume afresh (the boot log,
 * updates, programs) - against a model of what every file must contain.
 * Every round the image is checked with fsck.fat and every file read back.
 *
 *   stress <image> <rounds> <seed>     (run.sh-style: alpine with dosfstools) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/fs/fatfs.h"

static FILE *img;
static block_device_t dev;
static int rd(void *c, uint64_t l, uint32_t n, void *b) { (void)c; fseeko(img, (off_t)l * 512, SEEK_SET); return fread(b, 512, n, img) == n ? 0 : -1; }
static int wr(void *c, uint64_t l, uint32_t n, const void *b) { (void)c; fseeko(img, (off_t)l * 512, SEEK_SET); return fwrite(b, 512, n, img) == n ? 0 : -1; }

#define MAXF 400
static struct { char path[96]; unsigned char *data; uint64_t size; int dir, used; } F[MAXF];
static fatfs_t longlived;
static char *image_path;
static unsigned rnd(void) { return (unsigned)rand(); }

/* the volume an operation uses: the long-lived one, or a fresh mount */
static fatfs_t *vol(fatfs_t *fresh) {
    if (rnd() % 2) return &longlived;
    if (fatfs_mount(fresh, &dev, 0, dev.sector_count) != 0) { puts("mount failed"); exit(1); }
    return fresh;
}

static int pick(int dir) {
    for (int t = 0; t < 50; t++) { int i = rnd() % MAXF; if (F[i].used && F[i].dir == dir) return i; }
    return -1;
}

static int slot(void) { for (int i = 0; i < MAXF; i++) if (!F[i].used) return i; return -1; }

static void fill(unsigned char *p, uint64_t n, unsigned seed) { for (uint64_t i = 0; i < n; i++) p[i] = (unsigned char)(seed * 131 + i * 7 + (i >> 9)); }

static uint64_t some_size(void) {
    switch (rnd() % 5) {
    case 0: return rnd() % 600;
    case 1: return rnd() % 9000;
    case 2: return 8192 * (1 + rnd() % 8);
    case 3: return rnd() % 300000;
    default: return 1 + rnd() % 70000;
    }
}

static void check(int round) {
    char cmd[300];
    fatfs_t v;
    fflush(img);
    snprintf(cmd, sizeof cmd, "fsck.fat -n %s > /tmp/fsck.out 2>&1", image_path);
    if (system(cmd) != 0) {
        printf("round %d: fsck.fat found problems:\n", round);
        system("grep -v 'Free cluster summary' /tmp/fsck.out | head -20");
        exit(1);
    }
    if (system("grep -qiE 'cross|shared|lost|truncat|loop|bad' /tmp/fsck.out") == 0) {
        printf("round %d: fsck.fat reports:\n", round);
        system("head -20 /tmp/fsck.out");
        exit(1);
    }
    fatfs_mount(&v, &dev, 0, dev.sector_count);
    for (int i = 0; i < MAXF; i++) {
        char *d = 0;
        uint64_t n = 0;
        if (!F[i].used || F[i].dir) continue;
        if (fatfs_read(&v, F[i].path, &d, &n) != 0 || n != F[i].size || (n && memcmp(d, F[i].data, n))) {
            printf("round %d: %s reads back wrong (%llu bytes, want %llu)\n", round, F[i].path,
                   (unsigned long long)n, (unsigned long long)F[i].size);
            exit(1);
        }
        free(d);
    }
}

int main(int argc, char **argv) {
    int rounds = atoi(argv[2]);
    fseeko(img = fopen(image_path = argv[1], "r+b"), 0, SEEK_END);
    dev.read = rd; dev.write = wr; dev.sector_size = 512; dev.sector_count = (uint64_t)ftello(img) / 512;
    srand((unsigned)atoi(argv[3]));
    fatfs_mount(&longlived, &dev, 0, dev.sector_count);
    F[0].used = 1; F[0].dir = 1; strcpy(F[0].path, "");
    for (int round = 1; round <= rounds; round++) {
        for (int step = 0; step < 60; step++) {
            fatfs_t fresh;
            int op = rnd() % 10, i, j;
            if (op == 0) {                                          /* a folder */
                if ((i = pick(1)) < 0 || (j = slot()) < 0 || strlen(F[i].path) > 60) continue;
                snprintf(F[j].path, sizeof F[j].path, "%s/d%u", F[i].path, rnd() % 100000);
                if (fatfs_mkdir(vol(&fresh), F[j].path) == 0) { F[j].used = 1; F[j].dir = 1; F[j].data = 0; }
            } else if (op <= 3) {                                   /* a new file */
                if ((i = pick(1)) < 0 || (j = slot()) < 0) continue;
                snprintf(F[j].path, sizeof F[j].path, "%s/file-with-a-long-name-%u.txt", F[i].path, rnd() % 100000);
                F[j].size = some_size();
                F[j].data = malloc(F[j].size + 1);
                fill(F[j].data, F[j].size, rnd());
                if (fatfs_write(vol(&fresh), F[j].path, F[j].data, F[j].size) == 0) { F[j].used = 1; F[j].dir = 0; }
                else free(F[j].data);
            } else if (op <= 5) {                                   /* rewritten whole, like the boot log */
                if ((i = pick(0)) < 0) continue;
                free(F[i].data);
                F[i].size = some_size();
                F[i].data = malloc(F[i].size + 1);
                fill(F[i].data, F[i].size, rnd());
                if (fatfs_write(vol(&fresh), F[i].path, F[i].data, F[i].size) != 0) { printf("rewrite %s failed\n", F[i].path); exit(1); }
            } else if (op == 6) {                                   /* grown in pieces, like a download */
                if ((i = pick(0)) < 0) continue;
                uint64_t add = some_size();
                F[i].data = realloc(F[i].data, F[i].size + add + 1);
                fill(F[i].data + F[i].size, add, rnd());
                if (fatfs_write_at(vol(&fresh), F[i].path, F[i].size, F[i].data + F[i].size, add) != 0) { printf("append %s failed\n", F[i].path); exit(1); }
                F[i].size += add;
            } else if (op == 7) {                                   /* moved into another folder */
                char to[96];
                if ((i = pick(0)) < 0 || (j = pick(1)) < 0) continue;
                snprintf(to, sizeof to, "%s/moved-%u.bin", F[j].path, rnd() % 100000);
                if (fatfs_rename(vol(&fresh), F[i].path, to) == 0) strcpy(F[i].path, to);
            } else if (op == 8) {                                   /* removed */
                if ((i = pick(0)) < 0) continue;
                if (fatfs_remove(vol(&fresh), F[i].path) == 0) { free(F[i].data); F[i].used = 0; }
            } else {                                                /* cut short */
                if ((i = pick(0)) < 0 || !F[i].size) continue;
                uint64_t n = rnd() % F[i].size;
                if (fatfs_truncate(vol(&fresh), F[i].path, n) != 0) { printf("truncate %s failed\n", F[i].path); exit(1); }
                F[i].size = n;
            }
        }
        check(round);
    }
    printf("%d rounds: no shared or lost clusters, every file reads back\n", rounds);
    return 0;
}
