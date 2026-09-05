#include "devops.h"

#include "../drivers/serial/serial.h"
#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"
#include "../drivers/display/gpu.h"
#include "../drivers/display/flip.h"
#include "../drivers/display/vga.h"
#include "../drivers/input/input.h"
#include "../drivers/input/mouse.h"
#include "../fs/vfs.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"
#include "../syscall/syscall.h"

/* ---- local helpers ---- */

/* Minimal serial u64 printer (no printf in kernel). */
static void dev_serial_u64(uint64_t v) {
    char buf[21];
    int i = 0;
    int a;
    int b;
    char t;

    if (v == 0) {
        serial_write("0");
        return;
    }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    buf[i] = '\0';
    for (a = 0, b = i - 1; a < b; a++, b--) {
        t = buf[a];
        buf[a] = buf[b];
        buf[b] = t;
    }
    serial_write(buf);
}

static uint64_t dev_kstrlen(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

/* ---- /dev/console surface (wraps drivers/console) ---- */

static uint64_t dev_con_write(const char *text) {
    if (!text) {
        return (uint64_t)-1;
    }
    console_write(text, CONSOLE_STYLE_INFO);
    return dev_kstrlen(text);
}

static void dev_con_clear(void) {
    console_clear();
}

static void dev_con_backspace(void) {
    console_backspace();
}

static void dev_con_set_cursor(int x, int y) {
    console_set_cursor(x, y);
}

static void dev_con_get_cursor(int *x, int *y) {
    console_get_cursor(x, y);
}

static int dev_con_columns(void) {
    int cols = VGA_WIDTH;
    if (fb_available()) {
        int fb_cols = fb_columns();
        if (fb_cols > 0) {
            cols = fb_cols;
        }
    }
    return cols;
}

static int dev_con_rows(void) {
    int rows = VGA_HEIGHT;
    if (fb_available()) {
        int fb_rows_count = fb_rows();
        if (fb_rows_count > 0) {
            rows = fb_rows_count;
        }
    }
    return rows;
}

static const dev_calls_t dev_console_calls = {
    dev_con_write,
    dev_con_clear,
    dev_con_backspace,
    dev_con_set_cursor,
    dev_con_get_cursor,
    dev_con_columns,
    dev_con_rows,
    0, 0, 0, 0, 0, 0,
};

/* ---- /dev/input surface (keyboard only; mouse stays direct) ---- */

static const dev_calls_t dev_input_calls = {
    0, 0, 0, 0, 0, 0, 0,
    input_read_char,
    0, 0, 0, 0, 0,
};

/* ---- /dev/fb0 surface (claim state moved here from syscall.c) ---- */

static int fb_claimed = 0;
static uint64_t fb_claim_pid = 0;

static void fb_release_if_owner_gone(void) {
    process_t *owner;

    if (!fb_claimed || fb_claim_pid == 0) {
        return;
    }
    owner = sched_find_process(fb_claim_pid);
    if (!owner || owner->state == PROCESS_EXITED ||
        owner->state == PROCESS_REAPED) {
        fb_claimed = 0;
        fb_claim_pid = 0;
    }
}

static uint64_t dev_fb_claim_map(void *info) {
    syscall_fb_info_t *fb = (syscall_fb_info_t *)info;
    process_t *fproc = sched_current_process();
    uint64_t fb_phys;
    uint64_t fb_size;
    uint64_t fb_virt = 0x500000000ULL;
    uint64_t page_offset;
    uint64_t fb_phys_aligned;
    uint64_t pages;
    uint64_t pi;

    /* If the previous claimant is gone (killed, crashed, or exited
     * via a VT switch), let the new process take over the screen. */
    fb_release_if_owner_gone();
    if (fb_claimed) {
        return (uint64_t)-1;
    }
    if (!fb_available()) {
        return (uint64_t)-1;
    }
    if (!fproc || !fproc->addr_space) {
        return (uint64_t)-1;
    }
    /* Identity gate, log-only (P0 step 2): record who claims; no denial. */
    serial_write("[ident] op=fb-claim pid=");
    dev_serial_u64(fproc->pid);
    serial_write(" uid=");
    dev_serial_u64(fproc->ex_uid);
    serial_write(" tok=");
    dev_serial_u64(fproc->ex_token);
    serial_write(fproc->ex_session_leader ? " leader=1\n" : " leader=0\n");
    fb_phys = fb_phys_addr();
    fb_size = fb_phys_size();
    if (!fb_phys || !fb_size) {
        return (uint64_t)-1;
    }
    page_offset = fb_phys & 0xFFFULL;
    fb_phys_aligned = fb_phys & ~0xFFFULL;
    pages = (fb_size + page_offset + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (pi = 0; pi < pages; pi++) {
        if (vmm_map_page(fproc->addr_space,
                         fb_virt + pi * PAGE_SIZE_4K,
                         fb_phys_aligned + pi * PAGE_SIZE_4K,
                         VMM_FLAGS_USER_RW) != 0) {
            return (uint64_t)-1;
        }
    }
    if (fb) {
        fb->virt_addr = fb_virt + page_offset;
        fb->width     = fb_width;
        fb->height    = fb_height;
        fb->pitch     = fb_pitch_value();
        /* Report the real pixel format.  The window manager blits
         * into this mapping, so it must know whether it is 32bpp
         * (typical on real GPUs) or 24bpp (QEMU/GRUB fallbacks). */
        fb->bpp       = (uint32_t)fb_bpp_value();
    }
    /* Keep the PS/2 cursor position clamped to the real screen size */
    mouse_set_screen(fb_width, fb_height);
    fb_claimed = 1;
    fb_claim_pid = fproc->pid;
    return fb_virt + page_offset;
}

static int dev_fb_is_claimed(void) {
    fb_release_if_owner_gone();
    return fb_claimed ? 1 : 0;
}

static int dev_gpu_query(void *out) {
    syscall_gpu_info_t *info = (syscall_gpu_info_t *)out;
    gpu_device_t *dev;
    uint64_t i;

    if (!out) {
        return -1;
    }
    dev = gpu_primary();
    if (!dev) {
        return -1;
    }
    for (i = 0; dev->name[i] && i < sizeof(info->name) - 1; i++) {
        info->name[i] = dev->name[i];
    }
    info->name[i] = 0;
    info->width = (int32_t)dev->modes[dev->current_mode].width;
    info->height = (int32_t)dev->modes[dev->current_mode].height;
    info->pitch = dev->modes[dev->current_mode].pitch;
    info->bpp = dev->modes[dev->current_mode].bpp;
    info->mode_count = dev->mode_count;
    info->hw_cursor = dev->hw_cursor ? 1U : 0U;
    info->present_supported = dev->present_supported ? 1U : 0U;
    info->flip_active = flip_active() ? 1U : 0U;
    return 0;
}

static int dev_gpu_present_fn(void) {
    gpu_device_t *dev = gpu_primary();
    if (!dev || !dev->present) {
        return -1;
    }
    return dev->present(dev) == 0 ? 0 : -1;
}

static int dev_gpu_set_cursor(int x, int y, const uint32_t *image,
                              int w, int h) {
    gpu_device_t *dev = gpu_primary();
    if (!dev || !dev->set_cursor) {
        return -1;
    }
    return dev->set_cursor(dev, x, y, image, w, h) == 0 ? 0 : -1;
}

static const dev_calls_t dev_fb_calls = {
    0, 0, 0, 0, 0, 0, 0, 0,
    dev_fb_claim_map,
    dev_fb_is_claimed,
    dev_gpu_query,
    dev_gpu_present_fn,
    dev_gpu_set_cursor,
};

/* ---- population ---- */

int dev_populate(void) {
    vfs_node_t *dev;
    int rc = 0;

    /* Idempotent: a persistfs replay may already have recreated /dev. */
    dev = vfs_resolve(vfs_root(), "/dev");
    if (!dev) {
        if (vfs_mkdir(vfs_root(), "/dev") != 0) {
            return -1;
        }
        dev = vfs_resolve(vfs_root(), "/dev");
        if (!dev || vfs_node_type(dev) != VFS_NODE_DIR) {
            return -1;
        }
    }
    /* Discoverability nodes (plain files; dispatch uses the registry). */
    if (!vfs_resolve(vfs_root(), "/dev/console")) {
        if (vfs_create(vfs_root(), "/dev/console") != 0) {
            rc = -1;
        }
    }
    if (!vfs_resolve(vfs_root(), "/dev/input")) {
        if (vfs_create(vfs_root(), "/dev/input") != 0) {
            rc = -1;
        }
    }
    if (!vfs_resolve(vfs_root(), "/dev/fb0")) {
        if (vfs_create(vfs_root(), "/dev/fb0") != 0) {
            rc = -1;
        }
    }
    if (devops_register("/dev/console", &dev_console_calls) != 0) {
        rc = -1;
    }
    if (devops_register("/dev/input", &dev_input_calls) != 0) {
        rc = -1;
    }
    if (devops_register("/dev/fb0", &dev_fb_calls) != 0) {
        rc = -1;
    }
    return rc;
}
