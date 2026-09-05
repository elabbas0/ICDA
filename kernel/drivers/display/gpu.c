#include "gpu.h"
#include "framebuffer.h"
#include "flip.h"
#include "../device.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"

/* Registered display devices, newest first. */
static gpu_device_t *gpu_device_list = NULL;
static gpu_device_t *gpu_primary_device = NULL;

/* ---- fbdev driver: wraps the firmware framebuffer -------------------- */

static int fbdev_present(gpu_device_t *dev) {
    /* Single scanout buffer: the compositor already blitted into the
     * mapping, so there is nothing to flip.  Kept as a real op so a
     * double-buffered driver (e.g. virtio-gpu scanout flips) plugs in
     * without changing userspace. */
    (void)dev;
    return 0;
}

static int fbdev_set_cursor(gpu_device_t *dev, int x, int y,
                            const uint32_t *image, int w, int h) {
    /* No hardware cursor plane on the firmware framebuffer: the WM
     * draws a software cursor, so report unsupported. */
    (void)dev; (void)x; (void)y; (void)image; (void)w; (void)h;
    return -1;
}

/* ---- registry -------------------------------------------------------- */

int gpu_register_device(gpu_device_t *dev) {
    if (!dev) {
        return -1;
    }
    dev->next = gpu_device_list;
    gpu_device_list = dev;
    if (!gpu_primary_device) {
        gpu_primary_device = dev;
    }
    return 0;
}

gpu_device_t *gpu_primary(void) {
    return gpu_primary_device;
}

gpu_device_t *gpu_find(const char *name) {
    gpu_device_t *dev = gpu_device_list;
    if (!name) {
        return NULL;
    }
    while (dev) {
        const char *n = dev->name;
        int same = 1;
        for (int i = 0; i < GPU_NAME_MAX; i++) {
            if (n[i] != name[i]) { same = 0; break; }
            if (n[i] == 0) break;
        }
        if (same) return dev;
        dev = dev->next;
    }
    return NULL;
}

/* ---- init ------------------------------------------------------------ */

int gpu_init(void *multiboot_info) {
    static gpu_device_t fbdev;
    uint64_t phys;
    uint64_t size;
    int w;
    int h;
    uint32_t bpp;
    uint32_t pitch;

    /* fb_init parses the multiboot framebuffer tag; it must run first
     * (the console already does this before we get here). */
    (void)multiboot_info;
    if (!fb_available()) {
        return -1;
    }

    phys = fb_phys_addr();
    size = fb_phys_size();
    w = fb_width;
    h = fb_height;
    bpp = (uint32_t)fb_bpp_value();
    pitch = fb_pitch_value();
    if (!phys || !size || w <= 0 || h <= 0) {
        return -1;
    }

    /* Expose the native mode the firmware set up (gfxpayload=keep). */
    fbdev.name[0] = 'f'; fbdev.name[1] = 'b'; fbdev.name[2] = 'd';
    fbdev.name[3] = 'e'; fbdev.name[4] = 'v'; fbdev.name[5] = 0;
    fbdev.mode_count = 1;
    fbdev.modes[0].width = (uint32_t)w;
    fbdev.modes[0].height = (uint32_t)h;
    fbdev.modes[0].pitch = pitch;
    fbdev.modes[0].bpp = bpp;
    fbdev.current_mode = 0;
    fbdev.fb_phys = phys;
    fbdev.fb_size = size;
    fbdev.hw_cursor = 0;
    fbdev.present_supported = 1;
    fbdev.present = fbdev_present;
    fbdev.set_cursor = fbdev_set_cursor;
    fbdev.priv = NULL;
    fbdev.next = NULL;

    /* Probe for Bochs VBE page flipping.  On success, double the
     * reported fb_size so devnodes.c maps two frames and the WM can
     * blit into the back buffer.  On ANY failure flip_active stays
     * false (flip_available = 0) and the legacy blit path is used. */
    if (flip_probe(phys, pitch, (uint32_t)h, bpp, 0) == 0) {
        fb_set_double_frame(1);
        fbdev.fb_size = fb_phys_size();
    }

    return gpu_register_device(&fbdev);
}
