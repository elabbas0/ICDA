/* Host test of the kernel's FAT32 code (kernel/fs/fatfs.c) on a disk image:
 * copies a directory tree into the image the way the installer copies
 * WebKit (mkdir, then each file written in 1 MB pieces: fatfs_write for the
 * first, fatfs_write_at for the rest), then the image is checked with
 * fsck.fat and compared with the source.
 *
 *   fattest <image> <source dir> <destination path in the image> */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../../kernel/fs/fatfs.h"

static FILE *img;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (fseeko(img, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    return fread(buf, 512, count, img) == count ? 0 : -1;
}

static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (fseeko(img, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    return fwrite(buf, 512, count, img) == count ? 0 : -1;
}

static fatfs_t vol;
static char *chunk;
static long files, failures;

static int copy_tree(const char *src, char *dst, size_t cap) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(src);
        struct dirent *e;
        size_t base = strlen(dst);
        (void)fatfs_mkdir(&vol, dst);
        while (d && (e = readdir(d))) {
            char s[1024];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(s, sizeof s, "%s/%s", src, e->d_name);
            snprintf(dst + base, cap - base, "/%s", e->d_name);
            copy_tree(s, dst, cap);
            dst[base] = 0;
        }
        if (d) closedir(d);
        return 0;
    }
    {
        FILE *f = fopen(src, "rb");
        uint64_t off = 0, size = (uint64_t)st.st_size;
        if (!f) return -1;
        do {
            size_t want = size - off < (1 << 20) ? size - off : (1 << 20);
            if (fread(chunk, 1, want, f) != want ||
                (off == 0 ? fatfs_write(&vol, dst, chunk, want) : fatfs_write_at(&vol, dst, off, chunk, want)) != 0) {
                printf("FAILED %s at %llu\n", dst, (unsigned long long)off);
                failures++;
                break;
            }
            off += want;
        } while (off < size);
        fclose(f);
        files++;
    }
    return 0;
}

int main(int argc, char **argv) {
    static block_device_t dev;
    char dst[1024];
    off_t size;
    if (argc < 4) return 2;
    img = fopen(argv[1], "r+b");
    if (!img) return 1;
    fseeko(img, 0, SEEK_END);
    size = ftello(img);
    dev.read = img_read;
    dev.write = img_write;
    dev.sector_size = 512;
    dev.sector_count = (uint64_t)size / 512;
    chunk = malloc(1 << 20);
    if (fatfs_mount(&vol, &dev, 0, dev.sector_count) != 0) {
        printf("mount failed\n");
        return 1;
    }
    snprintf(dst, sizeof dst, "%s", argv[3]);
    copy_tree(argv[2], dst, sizeof dst);
    fatfs_flush(&vol);
    fclose(img);
    printf("copied %ld files, %ld failures\n", files, failures);
    return failures ? 1 : 0;
}
