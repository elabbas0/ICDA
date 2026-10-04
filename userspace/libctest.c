#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "icda_sys.h"

static int passed, failed;

static void check(int ok, const char *what) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static void test_strings(void) {
    char buf[64];
    char tok[] = "a,b,,c";
    check(strlen("hello") == 5, "strlen");
    check(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("x", "x") == 0, "strcmp");
    check(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp");
    strcpy(buf, "foo");
    strcat(buf, "bar");
    check(strcmp(buf, "foobar") == 0, "strcpy/strcat");
    check(strchr(buf, 'b') == buf + 3 && strrchr(buf, 'o') == buf + 2, "strchr/strrchr");
    check(strstr(buf, "oba") == buf + 2 && strstr(buf, "zz") == 0, "strstr");
    check(strspn("aab", "a") == 2 && strcspn("abc", "c") == 2, "strspn/strcspn");
    check(strcmp(strtok(tok, ","), "a") == 0 && strcmp(strtok(0, ","), "b") == 0 &&
          strcmp(strtok(0, ","), "c") == 0 && strtok(0, ",") == 0, "strtok");
    memset(buf, 'x', 4);
    check(memcmp(buf, "xxxxar", 6) == 0, "memset/memcmp");
    memmove(buf + 1, buf, 5);
    check(memcmp(buf, "xxxxxa", 6) == 0, "memmove overlap");
    check(isdigit('7') && !isdigit('a') && isspace('\t') && toupper('q') == 'Q', "ctype");
}

static void test_numbers(void) {
    char *end;
    check(atoi("-42") == -42, "atoi");
    check(strtol("0x1F", &end, 0) == 31 && *end == 0, "strtol hex");
    check(strtol("0755", 0, 0) == 493, "strtol octal");
    check(strtoul("123abc", &end, 10) == 123 && strcmp(end, "abc") == 0, "strtoul end");
    check(strtoll("-9000000000", 0, 10) == -9000000000LL, "strtoll 64-bit");
}

static void test_format(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%d|%5d|%-5d|%05d|%x|%X|%#x", -12, 42, 42, 42, 255, 255, 255);
    check(strcmp(buf, "-12|   42|42   |00042|ff|FF|0xff") == 0, "printf integers");
    snprintf(buf, sizeof(buf), "%s|%8s|%-4s|%.2s|%c|%%", "hi", "pad", "l", "trunc", 'Z');
    check(strcmp(buf, "hi|     pad|l   |tr|Z|%") == 0, "printf strings");
    snprintf(buf, sizeof(buf), "%lu %lld %zu", 4000000000UL, -5LL, (size_t)7);
    check(strcmp(buf, "4000000000 -5 7") == 0, "printf long sizes");
    check(snprintf(buf, 4, "abcdef") == 6 && strcmp(buf, "abc") == 0, "snprintf truncation");
}

static void test_memory(void) {
    char *blocks[200];
    int ok = 1;
    char *big, *grown;
    int nums[] = { 5, 3, 9, 1, 7, 2 };
    for (int i = 0; i < 200; i++) {
        blocks[i] = (char *)malloc((size_t)(i * 37 % 900 + 1));
        if (!blocks[i]) ok = 0;
        else memset(blocks[i], i, (size_t)(i * 37 % 900 + 1));
    }
    for (int i = 0; i < 200 && ok; i++) {
        if (blocks[i][0] != (char)i || blocks[i][i * 37 % 900] != (char)i) ok = 0;
    }
    for (int i = 0; i < 200; i += 2) free(blocks[i]);
    for (int i = 0; i < 200; i += 2) blocks[i] = (char *)malloc(64);
    for (int i = 1; i < 200 && ok; i += 2) {
        if (blocks[i][0] != (char)i) ok = 0;
    }
    for (int i = 0; i < 200; i++) free(blocks[i]);
    check(ok, "malloc/free 200 blocks keep their contents");
    big = (char *)malloc(3 * 1024 * 1024);
    check(big != 0, "malloc 3 MB");
    if (big) {
        big[0] = 1;
        big[3 * 1024 * 1024 - 1] = 2;
        free(big);
    }
    grown = (char *)malloc(16);
    strcpy(grown, "keep");
    grown = (char *)realloc(grown, 500000);
    check(grown && strcmp(grown, "keep") == 0, "realloc keeps data");
    free(grown);
    grown = (char *)calloc(1000, 4);
    ok = grown != 0;
    for (int i = 0; ok && i < 4000; i++) ok = grown[i] == 0;
    check(ok, "calloc zeroes");
    free(grown);
    qsort(nums, 6, sizeof(int), cmp_int);
    check(nums[0] == 1 && nums[5] == 9 && nums[2] == 3, "qsort");
}

static void test_files(void) {
    const char *path = "/home/.libctest.txt";
    char line[64];
    FILE *f = fopen(path, "w");
    check(f != 0, "fopen w");
    if (!f) return;
    fprintf(f, "line %d\n", 1);
    fputs("second\n", f);
    fclose(f);
    f = fopen(path, "a");
    fputs("third\n", f);
    fclose(f);
    f = fopen(path, "r");
    check(f && fgets(line, sizeof(line), f) && strcmp(line, "line 1\n") == 0, "fgets first line");
    check(f && fgets(line, sizeof(line), f) && strcmp(line, "second\n") == 0, "fgets second line");
    check(f && fgets(line, sizeof(line), f) && strcmp(line, "third\n") == 0, "append mode");
    check(f && fgets(line, sizeof(line), f) == 0 && feof(f), "eof");
    if (f) {
        fseek(f, 0, SEEK_END);
        check(ftell(f) == 20, "fseek/ftell");
        fclose(f);
    }
    check(remove(path) == 0 && fopen(path, "r") == 0, "remove");
}

static void test_volume_writes(const char *root) {
    char dir[96], path[200], expect[64], got[64];
    static char listing[65536];
    int ok = 1, count = 0;
    icda_stat_t st;
    if ((long)icda_stat(root, &st) < 0) return;
    snprintf(dir, sizeof(dir), "%s/icda write test", root);
    printf("  write test in %s\n", dir);
    check((long)icda_mkdir(dir) >= 0, "mkdir on a volume");
    for (int i = 0; i < 200 && ok; i++) {
        FILE *f;
        snprintf(path, sizeof(path), "%s/file number %03d with a fairly long name for testing.txt", dir, i);
        f = fopen(path, "w");
        if (!f) {
            ok = 0;
            break;
        }
        fprintf(f, "contents of file %d", i);
        ok = fclose(f) == 0;
    }
    check(ok, "create 200 long-named files on a volume");
    for (int i = 0; i < 200 && ok; i++) {
        FILE *f;
        snprintf(path, sizeof(path), "%s/file number %03d with a fairly long name for testing.txt", dir, i);
        snprintf(expect, sizeof(expect), "contents of file %d", i);
        f = fopen(path, "r");
        ok = f && fgets(got, sizeof(got), f) && strcmp(got, expect) == 0;
        if (f) fclose(f);
    }
    check(ok, "read back 200 files from a volume");
    for (int i = 0; i < 200 && ok; i += 2) {
        snprintf(path, sizeof(path), "%s/file number %03d with a fairly long name for testing.txt", dir, i);
        ok = remove(path) == 0;
    }
    check(ok, "delete 100 files from a volume");
    if ((long)icda_list_dir(dir, listing, sizeof(listing) - 1) >= 0) {
        for (char *p = listing; *p; p++) {
            if ((p == listing || p[-1] == '\n') && *p != '.') count++;
        }
    }
    check(count == 100, "volume listing after deletes");
}

static void test_partial_writes(const char *root) {
    static char buf[24000];
    char path[160];
    FILE *f;
    long got;
    int ok;
    icda_stat_t st;
    if ((long)icda_stat(root, &st) < 0) return;
    snprintf(path, sizeof(path), "%s/partial.bin", root);
    printf("  partial write test on %s\n", path);
    f = fopen(path, "w");
    for (int i = 0; f && i < 10000; i++) fputc('A' + i % 26, f);
    check(f && fclose(f) == 0, "create 10000-byte file");
    f = fopen(path, "r+");
    if (f) {
        fseek(f, 5000, SEEK_SET);
        fputs("XYZ", f);
    }
    check(f && fclose(f) == 0, "overwrite in place");
    f = fopen(path, "a");
    for (int i = 0; f && i < 9000; i++) fputc('b', f);
    check(f && fclose(f) == 0, "append across clusters");
    got = (long)icda_read_file(path, buf, sizeof(buf));
    ok = got == 19000;
    for (int i = 0; ok && i < 19000; i++) {
        char want = i >= 10000 ? 'b' : (i >= 5000 && i < 5003) ? "XYZ"[i - 5000] : (char)('A' + i % 26);
        ok = buf[i] == want;
    }
    check(ok, "partial writes read back");
    check(icda_truncate(path, 7000) == 0 && (long)icda_stat(path, &st) >= 0 && st.size == 7000, "truncate shrink");
    check(icda_truncate(path, 12000) == 0 && (long)icda_read_file(path, buf, sizeof(buf)) == 12000 &&
              buf[6999] == (char)('A' + 6999 % 26) && buf[7000] == 0 && buf[11999] == 0,
          "truncate grow zero-fills");
}

static void test_volume_ranges(void) {
    static const char *paths[3] = { "/volumes/fat32-1/rangetest.bin", "/volumes/exfat-0/rangetest.bin",
                                    "/volumes/ntfs-0/rangetest.bin" };
    static const unsigned long offsets[4] = { 0UL, 4095UL, 5000003UL, 41934000UL };
    static char buf[8192];
    icda_stat_t st;
    const char *path = 0;
    for (int p = 0; p < 3 && !path; p++) {
        if ((long)icda_stat(paths[p], &st) >= 0) path = paths[p];
    }
    if (!path) return;
    printf("  range test on %s (%lu bytes)\n", path, (unsigned long)st.size);
    for (int k = 0; k < 4; k++) {
        long got = (long)icda_read_file_at(path, offsets[k], buf, sizeof(buf));
        int ok = got == (long)sizeof(buf);
        for (long i = 0; ok && i < got; i++) {
            ok = (unsigned char)buf[i] == (unsigned char)(((offsets[k] + (unsigned long)i) * 7UL + 3UL) & 0xFFUL);
        }
        check(ok, "ranged read from a volume");
    }
}

int main(int argc, char **argv) {
    printf("libctest: %s (argc=%d)\n", argc > 0 ? argv[0] : "?", argc);
    test_strings();
    test_numbers();
    test_format();
    test_memory();
    test_files();
    test_volume_ranges();
    test_volume_writes("/volumes/exfat-0");
    test_partial_writes("/home");
    test_partial_writes("/volumes/exfat-0");
    test_partial_writes("/volumes/fat32-1");
    printf("libctest: %d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}
