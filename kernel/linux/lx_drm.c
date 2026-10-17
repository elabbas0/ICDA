/* The GPU for Linux programs: /dev/dri/renderD128 (Linux's DRM render node,
 * the i915 driver interface of kernel/drivers/display/i915.c), dma-buf files
 * for buffers shared between processes, and the few /sys entries libdrm
 * reads to learn what the device is - its PCI ids and that it is a DRM
 * device on the PCI bus.
 *
 * Mesa's Intel driver finds the GPU through libdrm (drmGetDevice2): it stats
 * the render node for its device number (226:128), follows
 * /sys/dev/char/226:128/device (a link to the PCI device's folder), reads
 * vendor, device, revision and the subsystem ids there, PCI_SLOT_NAME from
 * its uevent, and the bus from where its "subsystem" link points. */

#include "lx_internal.h"
#include "../drivers/display/i915.h"
#include "../drivers/display/intel_gpu.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../syscall/uaccess.h"

#define DRM_NODE      "/dev/dri/renderD128"
#define DRM_RDEV      ((226ULL << 8) | 128)
#define PCI_DIR       "/sys/devices/pci0000:00/0000:00:02.0"

static const struct { const char *from, *to; } links[] = {
    { "/sys/dev/char/226:128/device", PCI_DIR },
    { PCI_DIR "/subsystem", "/sys/bus/pci" },
    { PCI_DIR "/driver", "/sys/bus/pci/drivers/i915" },
};
#define N_LINKS (sizeof(links) / sizeof(links[0]))

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static uint64_t str_len(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static void hex4(char *out, const char *pre, uint32_t v, int digits) {
    int n = 0;
    while (*pre) out[n++] = *pre++;
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) out[n++] = "0123456789abcdef"[(v >> s) & 15];
    out[n++] = '\n';
    out[n] = 0;
}

/* the nodes, made once there is a GPU and someone looks */
static void make_nodes(void) {
    static int made;
    const pci_device_t *pci = igpu_pci();
    char buf[256], v[24];
    uint32_t sub;
    uint64_t n = 0;
    if (made || !i915_available() || !pci) return;
    made = 1;
    (void)vfs_mkdir(vfs_root(), "/dev/dri");
    (void)vfs_create(vfs_root(), DRM_NODE);
    (void)vfs_mkdir_ram("/sys");
    sub = pci_read_config32(pci, 0x2c);
    hex4(v, "0x", pci->vendor_id, 4); (void)vfs_seed_readonly(PCI_DIR "/vendor", v, str_len(v));
    hex4(v, "0x", pci->device_id, 4); (void)vfs_seed_readonly(PCI_DIR "/device", v, str_len(v));
    hex4(v, "0x", pci->revision_id, 2); (void)vfs_seed_readonly(PCI_DIR "/revision", v, str_len(v));
    hex4(v, "0x", sub & 0xFFFF, 4); (void)vfs_seed_readonly(PCI_DIR "/subsystem_vendor", v, str_len(v));
    hex4(v, "0x", sub >> 16, 4); (void)vfs_seed_readonly(PCI_DIR "/subsystem_device", v, str_len(v));
    (void)vfs_seed_readonly(PCI_DIR "/class", "0x030000\n", 9);
    {
        const char *parts[] = { "DRIVER=i915\nPCI_CLASS=30000\nPCI_ID=", 0, "\nPCI_SUBSYS_ID=", 0,
                                "\nPCI_SLOT_NAME=0000:00:02.0\n" };
        char ids[2][16];
        hex4(ids[0], "8086:", pci->device_id, 4); ids[0][9] = 0;
        hex4(ids[1], "", sub & 0xFFFF, 4); ids[1][4] = ':';
        hex4(ids[1] + 5, "", sub >> 16, 4); ids[1][9] = 0;
        parts[1] = ids[0];
        parts[3] = ids[1];
        for (int i = 0; i < 5; i++) for (const char *p = parts[i]; *p && n + 1 < sizeof(buf); p++) buf[n++] = *p;
        (void)vfs_seed_readonly(PCI_DIR "/uevent", buf, n);
    }
    (void)vfs_seed_readonly("/sys/dev/char/226:128/uevent", "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n", 58);
    (void)vfs_mkdir(vfs_root(), PCI_DIR "/drm");
    (void)vfs_mkdir(vfs_root(), PCI_DIR "/drm/renderD128");
    (void)vfs_mkdir(vfs_root(), "/sys/bus");
    (void)vfs_mkdir(vfs_root(), "/sys/bus/pci");
    (void)vfs_mkdir(vfs_root(), "/sys/bus/pci/drivers");
    (void)vfs_mkdir(vfs_root(), "/sys/bus/pci/drivers/i915");
}

/* a path with the device links in it followed (/sys/dev/char/226:128/device/vendor
 * is PCI_DIR/vendor) */
void lx_drm_path(char *path) {
    if (!prefix(path, "/sys") && !prefix(path, "/dev/dri")) return;
    make_nodes();
    for (int hops = 0; hops < 4; hops++) {
        int moved = 0;
        for (uint64_t i = 0; i < N_LINKS; i++) {
            uint64_t n = str_len(links[i].from), t = str_len(links[i].to), rest;
            /* a link inside the path is followed; one that is the whole path
             * is left for readlink, as Linux does with the last component */
            if (!prefix(path, links[i].from) || path[n] != '/') continue;
            rest = str_len(path + n);
            if (t + rest + 1 > 256) return;
            {
                char tmp[256];
                for (uint64_t k = 0; k <= rest; k++) tmp[k] = path[n + k];
                for (uint64_t k = 0; k < t; k++) path[k] = links[i].to[k];
                for (uint64_t k = 0; k <= rest; k++) path[t + k] = tmp[k];
            }
            moved = 1;
            break;
        }
        if (!moved) return;
    }
}

/* readlink of a device link: the length, or -1 if path is none */
int64_t lx_drm_readlink(const char *path, char *out, uint64_t cap) {
    for (uint64_t i = 0; i < N_LINKS; i++) {
        if (!str_eq(path, links[i].from)) continue;
        uint64_t n = str_len(links[i].to);
        if (n > cap) n = cap;
        for (uint64_t k = 0; k < n; k++) out[k] = links[i].to[k];
        return (int64_t)n;
    }
    return -1;
}

int lx_drm_is_node(const char *path) {
    make_nodes();
    return i915_available() && str_eq(path, DRM_NODE);
}

/* ---- dma-buf files ---------------------------------------------------------------- */

static void dmabuf_release(void *obj) { i915_obj_unref((i915_obj_t *)obj); }
static uint64_t dmabuf_size(void *obj) { return i915_obj_size((i915_obj_t *)obj); }
static uint64_t dmabuf_frame(void *obj, uint64_t off) { return i915_obj_frame((i915_obj_t *)obj, off); }

static int64_t dmabuf_ioctl(lx_file_t *f, uint64_t req, uint64_t arg) {
    (void)arg;
    if (((req >> 8) & 0xFF) == 'b' && (req & 0xFF) == 0) {      /* DMA_BUF_IOCTL_SYNC: the GPU done with it */
        int r = i915_obj_wait((i915_obj_t *)f->obj, -1);
        return r < 0 ? r : 0;
    }
    return -25;                                                 /* ENOTTY: no sync files */
}

static const lx_fops_t dmabuf_ops = {
    "anon_inode:dmabuf", 0, 0, 0, dmabuf_release, dmabuf_size, 0, 0100000, dmabuf_frame, dmabuf_ioctl, 0
};

/* ---- the render node ---------------------------------------------------------------- */

static void drm_release(void *obj) { i915_close((i915_client_t *)obj); }
static uint64_t drm_frame(void *obj, uint64_t off) { return i915_client_frame((i915_client_t *)obj, off); }

static int64_t drm_ioctl(lx_file_t *f, uint64_t req, uint64_t arg) {
    uint32_t type = (uint32_t)(req >> 8) & 0xFF, nr = (uint32_t)req & 0xFF;
    uint32_t size = (uint32_t)(req >> 16) & 0x3FFF, dir = (uint32_t)(req >> 30);
    i915_client_t *c = (i915_client_t *)f->obj;
    uint64_t karg[32];
    int64_t r;
    if (type != 'd') return -25;                                /* ENOTTY */
    if (size > sizeof(karg)) return -22;
    for (int i = 0; i < 32; i++) karg[i] = 0;
    if ((dir & 1) && size && copy_from_user(karg, (const void *)arg, size)) return -14;
    if (nr == 0x2d) {                                           /* PRIME_HANDLE_TO_FD */
        uint32_t *a = (uint32_t *)karg;                         /* handle, flags, fd */
        i915_obj_t *o = i915_obj_from_handle(c, a[0]);
        lx_file_t *df;
        int64_t fd;
        if (!o) return -2;
        df = lxi_file_new(&dmabuf_ops, o, 2);
        if (!df) { i915_obj_unref(o); return -12; }
        fd = lxi_fd_install(df, (a[1] & LX_O_CLOEXEC) != 0);
        if (fd < 0) return fd;
        ((int32_t *)karg)[2] = (int32_t)fd;
        r = 0;
    } else if (nr == 0x2e) {                                    /* PRIME_FD_TO_HANDLE */
        uint32_t *a = (uint32_t *)karg;
        lx_file_t *df = lxi_fd_file(((int32_t *)karg)[2]);
        if (!df || df->kind != LXF_OBJ || df->ops != &dmabuf_ops) return -22;
        r = i915_handle_for_obj(c, (i915_obj_t *)df->obj, &a[0]);
    } else {
        r = i915_ioctl(c, nr, karg);
    }
    if ((dir & 2) && size && copy_to_user((void *)arg, karg, size)) return -14;
    return r;
}

static const lx_fops_t drm_ops = {
    DRM_NODE, 0, 0, 0, drm_release, 0, 0, 020000, drm_frame, drm_ioctl, DRM_RDEV
};

lx_file_t *lx_drm_open(uint64_t flags) {
    i915_client_t *c;
    lx_file_t *f;
    i915_interrupted = lxi_interrupted;
    i915_now_ns = lxi_now_ns;
    c = i915_open();
    if (!c) return 0;
    f = lxi_file_new(&drm_ops, c, flags);
    if (!f) i915_close(c);
    return f;
}
