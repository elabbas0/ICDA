#ifndef GPU_H
#define GPU_H

#include <stdint.h>

/*
 * Kernel GPU device layer - the "general GPU driver" interface.
 *
 * Mirrors the shape of Linux DRM/KMS: the kernel owns a display device
 * abstraction (modes, scanout memory, present/cursor ops) and drivers
 * plug into it.  Today one driver exists - fbdev, wrapping the firmware
 * framebuffer GRUB handed us (the same role efifb/simpledrm play on
 * Linux).  A virtio-gpu or other PCI GPU driver can register later with
 * the same gpu_device_t interface and userspace need not change.
 */

#define GPU_MAX_MODES 8
#define GPU_NAME_MAX  32

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;    /* bytes per row */
    uint32_t bpp;
} gpu_mode_t;

typedef struct gpu_device {
    char        name[GPU_NAME_MAX];
    uint32_t    mode_count;
    gpu_mode_t  modes[GPU_MAX_MODES];
    int         current_mode;

    /* Scanout memory (physical).  The compositor maps this and blits
     * into it; present() tells the device the frame is ready. */
    uint64_t    fb_phys;
    uint64_t    fb_size;

    /* Capabilities */
    int         hw_cursor;      /* device has a hardware cursor plane */
    int         present_supported;

    /* Driver ops */
    int (*present)(struct gpu_device *dev);              /* commit frame */
    int (*set_cursor)(struct gpu_device *dev, int x, int y,
                      const uint32_t *image, int w, int h); /* hw cursor or -1 */

    void *priv;
    struct gpu_device *next;
} gpu_device_t;

/* Register a display driver (called by the fbdev driver at init). */
int  gpu_register_device(gpu_device_t *dev);

/* The primary (boot) display. */
gpu_device_t *gpu_primary(void);

/* Find a device by name; NULL if absent. */
gpu_device_t *gpu_find(const char *name);

/* Initialize the built-in fbdev driver from the multiboot framebuffer.
 * Returns 0 on success, -1 if no usable framebuffer exists. */
int gpu_init(void *multiboot_info);

#endif
