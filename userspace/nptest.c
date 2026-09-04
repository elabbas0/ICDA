/* Negative syscall-gate test (P0 hardening).
 *
 * Exercises the validated gate with hostile pointers. On a hardened
 * kernel every case below returns an error and the machine survives;
 * on an unhardened kernel the bad writes panic with PAGE FAULT (which
 * is exactly what scripts/qemu-smoke.sh greps for).
 *
 * Run from the shell once booted:
 *   run /apps/nptest.app
 * Exit code 0 = all checks behaved, nonzero = count of failures.
 */
#include "icda_sys.h"

#define U_EFAULT 14

static char good_buf[512];
static int failures = 0;

static void check(const char *name, long rc, int want_efault) {
    if (want_efault) {
        if (rc == -(long)U_EFAULT) {
            icda_write("PASS ");
        } else {
            failures++;
            icda_write("FAIL ");
        }
    } else {
        if (rc == -1) {
            icda_write("PASS ");
        } else {
            failures++;
            icda_write("FAIL ");
        }
    }
    icda_write(name);
    icda_write("\n");
}

int nptest_main(void) {
    /* Kernel-half addresses must be rejected, never dereferenced. */
    const char *kaddr = (const char *)0xFFFFFFFF80000000ULL;
    /* Low unmapped page (nothing is mapped at 0x1000 in user space). */
    char *hole = (char *)0x1000;

    icda_write("nptest: syscall gate negative tests\n");

    check("console_write(NULL)", (long)icda_write(0), 0);
    check("console_write(kernel)", (long)icda_write(kaddr), 1);

    check("vfs_read hole buf",
          (long)icda_read_file("/etc/motd.txt", hole, sizeof(good_buf)), 1);
    check("vfs_read kernel path",
          (long)icda_read_file(kaddr, good_buf, sizeof(good_buf)), 1);
    check("vfs_read 1GB cap",
          (long)icda_read_file("/etc/motd.txt", good_buf, 0x40000001ULL), 1);
    /* Sanity: a good call must still succeed (guards over-blocking). */
    if ((long)icda_read_file("/etc/motd.txt", good_buf, sizeof(good_buf)) < 0) {
        failures++;
        icda_write("FAIL vfs_read ok\n");
    } else {
        icda_write("PASS vfs_read ok\n");
    }

    check("list_dir hole buf",
          (long)icda_list_dir("/", hole, sizeof(good_buf)), 1);
    check("list_dir kernel path",
          (long)icda_list_dir(kaddr, good_buf, sizeof(good_buf)), 1);

    check("getcwd hole buf",
          (long)icda_getcwd(hole, sizeof(good_buf)), 1);

    /* /dev/fb0 claim path (moved to devnodes.c in P0 OS-ification):
     * claim must succeed pre-desktop with sane geometry. The claim is
     * released on exit (owner-gone takeover), so the desktop that
     * starts after us is unaffected. */
    {
        icda_fb_info_t fb;
        uint64_t base;
        fb.virt_addr = 0;
        fb.width = 0;
        fb.height = 0;
        fb.pitch = 0;
        fb.bpp = 0;
        base = icda_map_framebuffer(&fb);
        if ((long)base < 0 || fb.width <= 0 || fb.height <= 0) {
            failures++;
            icda_write("FAIL fb claim\n");
        } else {
            icda_write("PASS fb claim\n");
        }
    }

    /* W^X kill-path: spawn the linux test in suicide mode (it mprotects
     * a page read-only, then stores through it). The kernel must
     * terminate it with -11; survival or any other code is a FAIL. */
    {
        uint64_t pid = icda_spawn_args("/bin/nptestlx.elf", "suicide");
        if ((long)pid < 0) {
            failures++;
            icda_write("FAIL suicide spawn\n");
        } else {
            long code = (long)icda_waitpid(pid);
            if (code == -11) {
                icda_write("PASS suicide killed -11\n");
            } else {
                failures++;
                icda_write("FAIL suicide kill code\n");
            }
        }
    }

    if (failures == 0) {
        icda_write("nptest: ALL PASS\n");
    } else {
        icda_write("nptest: FAILURES PRESENT\n");
    }
    return failures;
}
