/* Guest regression: file-backed fault neighbours must keep their file offsets.
 * Build with a Linux libc and run on ICDA's Linux personality. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096
#define PAGES 48
static unsigned char page[PAGE];

static unsigned char expected(unsigned p, unsigned i) {
    return (unsigned char)(p * 71 + i * 13 + (i >> 8));
}

int main(void) {
    const char *path = "/tmp/file-prefetch-test.bin";
    const unsigned starts[] = {0, 5, 15, 31};
    int serial = open("/dev/serial", O_WRONLY);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600), bad = 0;
    if (fd < 0) return 2;
    for (unsigned p = 0; p < PAGES; p++) {
        for (unsigned i = 0; i < PAGE; i++) page[i] = expected(p, i);
        if (write(fd, page, PAGE) != PAGE) return 2;
    }
    for (unsigned offset = 0; offset <= 3; offset += 3) {
        unsigned count = PAGES - offset;
        for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
            volatile unsigned char *m = mmap(0, count * PAGE, PROT_READ,
                                            MAP_PRIVATE, fd, offset * PAGE);
            if (m == MAP_FAILED) return 2;
            /* Fault in the middle of an aligned group first, leaving holes
             * on both sides. Then check every byte, backwards by page. */
            bad += m[starts[s] * PAGE] != expected(offset + starts[s], 0);
            for (unsigned p = count; p-- > 0;) {
                for (unsigned i = 0; i < PAGE; i++) {
                    if (m[p * PAGE + i] != expected(offset + p, i)) {
                        bad++;
                        dprintf(serial, "file-prefetch: wrong page %u offset %u first %u byte %u\n",
                                p, offset, starts[s], i);
                        break;
                    }
                }
            }
            munmap((void *)m, count * PAGE);
        }
    }
    close(fd);
    unlink(path);
    dprintf(serial, "file-prefetch: %s (%d mismatches)\n", bad ? "FAIL" : "PASS", bad);
    puts(bad ? "file-prefetch: FAIL" : "file-prefetch: PASS");
    if (serial >= 0) close(serial);
    return bad ? 1 : 0;
}
