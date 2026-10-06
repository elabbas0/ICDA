/* Boot log saved to ICDA's own partition (/BOOTLOG.TXT).
 *
 * Laptops have no serial port, so everything written to the serial port or
 * the console also goes into a RAM log, and that log is written to the
 * system partition at each boot milestone (synchronously, so a hang still
 * leaves the last milestone on disk) and every few seconds for the first
 * minutes of uptime.  From Windows, give the partition a drive letter and
 * read BOOTLOG.TXT. */
#include "bootlog.h"
#include "fatfs.h"
#include "persistfs.h"
#include "../drivers/storage/partition.h"
#include "../proc/sched.h"

#define BOOTLOG_CAP (128u * 1024u)
#define BOOTLOG_PATH "/BOOTLOG.TXT"

static char     log_buf[BOOTLOG_CAP];
static uint32_t log_len;
static uint32_t log_flushed_len;
static int      log_wrapped;
static int      log_writing;
static int      log_failed;             /* a write failed: stop touching the disk */

void bootlog_putc(char c) {
    if (log_writing) return;            /* don't log our own disk writes */
    if (log_len >= BOOTLOG_CAP) {
        /* keep the first half (early boot), drop the middle of the rest */
        uint32_t keep = BOOTLOG_CAP / 2, drop = BOOTLOG_CAP / 4;
        for (uint32_t i = keep; i + drop < BOOTLOG_CAP; i++) log_buf[i] = log_buf[i + drop];
        log_len = BOOTLOG_CAP - drop;
        log_wrapped = 1;
    }
    log_buf[log_len++] = c;
}

void bootlog_puts(const char *s) {
    while (s && *s) bootlog_putc(*s++);
}

static void put_dec(uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) bootlog_putc(t[--n]);
}

int bootlog_flush(const char *milestone) {
    const partition_info_t *part;
    fatfs_t vol;
    int idx, rc;

    if (milestone) {
        bootlog_puts("[bootlog] ");
        bootlog_puts(milestone);
        bootlog_puts(" (tick ");
        put_dec(sched_ticks());
        bootlog_puts(")\n");
    }
    idx = persistfs_active_partition();
    if (idx < 0) return -1;              /* not booted from an installed system */
    if (log_failed) return -1;
    if (log_len == log_flushed_len && !milestone) return 0;
    part = partition_get((uint32_t)idx);
    if (!part || part->fs_hint != PARTITION_FS_FAT32) return -1;
    log_writing = 1;
    rc = fatfs_mount_part(&vol, part);
    if (rc == 0) rc = fatfs_write(&vol, BOOTLOG_PATH, log_buf, log_len);
    log_writing = 0;
    if (rc == 0) log_flushed_len = log_len;
    else {
        log_failed = 1;
        bootlog_puts("[bootlog] writing BOOTLOG.TXT failed, boot log saving disabled\n");
    }
    return rc;
}

static void bootlog_thread(void) {
    /* every 3 s for the first 3 minutes */
    for (int i = 0; i < 60; i++) {
        sched_sleep(300);
        bootlog_flush(0);
    }
    bootlog_flush("periodic saving stopped");
    for (;;) sched_sleep(100000);
}

void bootlog_start_thread(void) {
    (void)proc_create_kernel(bootlog_thread);
}

static void put_hex(uint64_t v) {
    bootlog_puts("0x");
    for (int s = 60; s >= 0; s -= 4) bootlog_putc("0123456789abcdef"[(v >> s) & 0xF]);
}

/* Called from the CPU exception paths just before the kernel halts. */
void bootlog_crash(const char *what, uint64_t vector, uint64_t rip, uint64_t addr) {
    static int crashing;
    if (crashing) return;               /* crashed while saving the crash */
    crashing = 1;
    log_writing = 0;                    /* the crash may have hit mid-write */
    bootlog_puts("[crash] ");
    bootlog_puts(what);
    bootlog_puts(" vector ");
    put_dec(vector);
    bootlog_puts(" rip ");
    put_hex(rip);
    bootlog_puts(" addr ");
    put_hex(addr);
    bootlog_putc('\n');
    (void)bootlog_flush("crash");
}
