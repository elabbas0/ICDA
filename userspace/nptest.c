










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
    
    const char *kaddr = (const char *)0xFFFFFFFF80000000ULL;
    
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
