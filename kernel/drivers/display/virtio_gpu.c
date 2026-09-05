/*
 * virtio-gpu.c — Legacy-PIO virtio-gpu driver for transitional -vga virtio
 *
 * PCI vendor 0x1AF4 device 0x1050.  Control queue only (queue 0).
 * Uses legacy PCI transport: BAR0 is an I/O-port (PIO) region.
 * Register access via inb/inw/inl/outb/outw/outl on (port + offset).
 * No MMIO, no PCI capabilities, no modern transport — transitional-only v1.
 *
 * Legacy virtqueue layout: ONE contiguous below-4GB region holds
 * descriptor table + available ring + used ring packed per spec:
 *   descs  (16B each) at +0
 *   avail  (4+2*q_size) right after descs
 *   used   at next 4K boundary
 *
 * Two chained descriptors per command:
 *   desc0 = request bytes  (OUT:  flags=NEXT, next=1)
 *   desc1 = response area  (IN:   flags=WRITE)
 * Response reuses the same page AFTER the request bytes
 * (device reads request before writing response).
 *
 * Init: GET_DISPLAY_INFO → RESOURCE_CREATE_2D → ATTACH_BACKING
 *        → SET_SCANOUT → adopt as system framebuffer.
 * present(): TRANSFER_TO_HOST_2D + RESOURCE_FLUSH + bounded poll.
 *
 * Skipped entirely when a multiboot framebuffer tag is present
 * (fb_available() == 1).
 */
#include "virtio_gpu.h"
#include "framebuffer.h"
#include "gpu.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#include <stdint.h>

/* Legacy virtio PCI register offsets (I/O-port based) */
#define VIRTIO_REG_DEVICE_FEATURES  0x00  /* 32-bit RO */
#define VIRTIO_REG_DRIVER_FEATURES  0x04  /* 32-bit WO */
#define VIRTIO_REG_QUEUE_ADDRESS    0x08  /* 32-bit WO: phys >> 12 */
#define VIRTIO_REG_QUEUE_SIZE       0x0C  /* 16-bit RO */
#define VIRTIO_REG_QUEUE_SELECT     0x0E  /* 16-bit WO */
#define VIRTIO_REG_QUEUE_NOTIFY     0x10  /* 16-bit WO */
#define VIRTIO_REG_DEVICE_STATUS    0x12  /* 8-bit  WO */

/* Status bits */
#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FAILED        0x80

/* Descriptor flags */
#define VIRTIO_DESC_F_NEXT          0x01
#define VIRTIO_DESC_F_WRITE         0x02

/* ---- Driver state ---- */

static struct {
    const pci_device_t *pci;
    uint16_t port;            /* I/O port base from BAR0 */

    /* Control queue (queue 0) — single contiguous legacy layout */
    uint8_t  *vq_mem;
    uint64_t  vq_phys;
    uint32_t  q_size;

    virtio_gpu_desc_t  *ctrl_desc;
    virtio_gpu_avail_t *ctrl_avail;
    virtio_gpu_used_t  *ctrl_used;

    /* Command buffer (one 4-KiB page) */
    uint8_t  *cmd_buf;
    uint64_t  cmd_buf_phys;

    uint32_t ctrl_last_used;

    /* Display state from GET_DISPLAY_INFO */
    uint32_t width;
    uint32_t height;

    int ready;
} vg;

static gpu_device_t virtio_gpu_dev;

/* ---- x86 port I/O helpers ---- */

static inline void vg_out8(uint16_t port, uint32_t off, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"((uint16_t)(port + off)));
}
static inline uint8_t vg_in8(uint16_t port, uint32_t off) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)(port + off)));
    return val;
}
static inline void vg_out16(uint16_t port, uint32_t off, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"((uint16_t)(port + off)));
}
static inline uint16_t vg_in16(uint16_t port, uint32_t off) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"((uint16_t)(port + off)));
    return val;
}
static inline void vg_out32(uint16_t port, uint32_t off, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)(port + off)));
}
static inline uint32_t vg_in32(uint16_t port, uint32_t off) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)(port + off)));
    return val;
}

static void vg_pause(void) {
    for (volatile uint32_t i = 0; i < 5000; i++)
        __asm__ volatile("" ::: "memory");
}

static int vg_alloc_page(uint64_t *phys_out, void **virt_out) {
    uint64_t phys = pmm_alloc_contiguous_below(1, 0xFFFFFFFFULL);
    if (!phys) return -1;
    *phys_out = phys;
    *virt_out = (void *)PHYS_TO_VIRT(phys);
    uint8_t *v = (uint8_t *)*virt_out;
    for (uint32_t i = 0; i < 4096; i++) v[i] = 0;
    return 0;
}

/* ---- Serial helper: print uint32_t as decimal ---- */

static void vg_serial_u32(uint32_t v) {
    char buf[11];
    int len = 0;
    if (v == 0) { serial_write("0"); return; }
    while (v > 0 && len < 10) { buf[len++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = len - 1; i >= 0; i--) {
        char tmp[2] = { buf[i], 0 };
        serial_write(tmp);
    }
}

/*
 * Legacy PCI capability walk and vg_legacy_mmio removed.
 * Transitional -vga virtio guarantees a PIO BAR0; no MMIO mapping
 * or PCI capability enumeration is needed or correct here.
 */

/* ---- Control queue setup (legacy packed layout) ---- */

static int vg_init_controlq(uint16_t q_size) {
    /* Legacy layout: desc table + avail ring contiguously, used ring at 4K boundary */
    uint32_t desc_bytes  = q_size * sizeof(virtio_gpu_desc_t);
    uint32_t avail_bytes = sizeof(uint16_t) + sizeof(uint16_t)
                         + q_size * sizeof(uint16_t);
    uint32_t used_off    = (desc_bytes + avail_bytes + 4095) & ~4095u;
    uint32_t used_bytes  = sizeof(uint16_t) + sizeof(uint16_t)
                         + q_size * sizeof(virtio_gpu_used_elem_t);
    uint32_t total       = used_off + used_bytes;
    uint32_t num_pages   = (total + 4095) / 4096;

    uint64_t phys = pmm_alloc_contiguous_below(num_pages, 0xFFFFFFFFULL);
    if (!phys) return -1;

    uint8_t *v = (uint8_t *)PHYS_TO_VIRT(phys);
    for (uint32_t i = 0; i < num_pages * 4096; i++) v[i] = 0;

    vg.vq_mem       = v;
    vg.vq_phys      = phys;
    vg.q_size       = q_size;
    vg.ctrl_desc    = (virtio_gpu_desc_t *)v;
    vg.ctrl_avail   = (virtio_gpu_avail_t *)(v + desc_bytes);
    vg.ctrl_used    = (virtio_gpu_used_t *)(v + used_off);

    /* Allocate command buffer */
    if (vg_alloc_page(&vg.cmd_buf_phys, (void **)&vg.cmd_buf) != 0)
        return -1;

    vg.ctrl_last_used = 0;
    return 0;
}

/* ---- Controlq send: two chained descriptors ---- */

static int vg_ctrl_send(uint32_t request_len) {
    uint16_t qsz = vg.q_size;

    /* desc[0]: request — OUT to device, chain to desc[1] */
    vg.ctrl_desc[0].addr  = vg.cmd_buf_phys;
    vg.ctrl_desc[0].len   = request_len;
    vg.ctrl_desc[0].flags = VIRTIO_DESC_F_NEXT;
    vg.ctrl_desc[0].next  = 1;

    /* desc[1]: response area — IN from device (WRITE), no chain */
    vg.ctrl_desc[1].addr  = vg.cmd_buf_phys + request_len;
    vg.ctrl_desc[1].len   = 4096 - request_len;
    vg.ctrl_desc[1].flags = VIRTIO_DESC_F_WRITE;
    vg.ctrl_desc[1].next  = 0;

    /* Post head index 0 to available ring */
    uint16_t avail_idx = vg.ctrl_avail->idx;
    vg.ctrl_avail->ring[avail_idx % qsz] = 0;
    __asm__ volatile("" ::: "memory");
    vg.ctrl_avail->idx = avail_idx + 1;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    vg_out16(vg.port, VIRTIO_REG_QUEUE_NOTIFY, VIRTIO_GPU_CONTROLQ);
    vg_pause();

    /* Poll used ring — bounded with vg_pause() inside each iteration */
    for (uint32_t iter = 0; iter < 10000; iter++) {
        vg_pause();
        __asm__ volatile("" ::: "memory");
        uint16_t used_idx = vg.ctrl_used->idx;
        if (vg.ctrl_last_used != used_idx) {
            while (vg.ctrl_last_used != used_idx) {
                uint16_t slot = vg.ctrl_last_used % qsz;
                virtio_gpu_used_elem_t *elem = &vg.ctrl_used->ring[slot];
                vg.ctrl_last_used++;
                if (elem->id == 0) {
                    /* Response is at cmd_buf + request_len */
                    virtio_gpu_ctrl_hdr_t *resp =
                        (virtio_gpu_ctrl_hdr_t *)(vg.cmd_buf + request_len);
                    uint32_t rtype = resp->type;
                    if (rtype == VIRTIO_GPU_RESP_OK_NODATA ||
                        rtype == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
                        return 0;
                    }
                    return -1;  /* device error */
                }
                used_idx = vg.ctrl_used->idx;
            }
        }
    }
    return -1;  /* timeout */
}

/* ---- High-level control commands ---- */

static int vg_get_display_info(uint32_t *w_out, uint32_t *h_out) {
    virtio_gpu_ctrl_hdr_t *hdr = (virtio_gpu_ctrl_hdr_t *)vg.cmd_buf;
    for (uint32_t i = 0; i < sizeof(virtio_gpu_ctrl_hdr_t); i++)
        vg.cmd_buf[i] = 0;
    hdr->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t req_len = sizeof(virtio_gpu_ctrl_hdr_t);
    if (vg_ctrl_send(req_len) != 0) return -1;

    virtio_gpu_resp_display_info_t *resp =
        (virtio_gpu_resp_display_info_t *)(vg.cmd_buf + req_len);
    for (int i = 0; i < 16; i++) {
        if (resp->pmodes[i].enabled &&
            resp->pmodes[i].primary_width > 0 &&
            resp->pmodes[i].primary_height > 0) {
            *w_out = resp->pmodes[i].primary_width;
            *h_out = resp->pmodes[i].primary_height;
            return 0;
        }
    }
    return -1;  /* no enabled display mode */
}

static int vg_resource_create_2d(uint32_t resource_id, uint32_t w, uint32_t h) {
    virtio_gpu_cmd_create_2d_t *cmd = (virtio_gpu_cmd_create_2d_t *)vg.cmd_buf;
    for (uint32_t i = 0; i < sizeof(virtio_gpu_cmd_create_2d_t); i++)
        vg.cmd_buf[i] = 0;
    cmd->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id  = resource_id;
    cmd->format       = VIRTIO_GPU_FORMAT_XRGB8888;
    cmd->width        = w;
    cmd->height       = h;
    return vg_ctrl_send(sizeof(virtio_gpu_cmd_create_2d_t));
}

static int vg_attach_backing(uint32_t resource_id, uint64_t phys, uint32_t size) {
    /* Build the full command (header + 1 mem_entry) in local buf */
    uint32_t req_len = sizeof(virtio_gpu_cmd_attach_backing_t)
                     + sizeof(virtio_gpu_mem_entry_t);
    uint8_t buf[sizeof(virtio_gpu_cmd_attach_backing_t)
              + sizeof(virtio_gpu_mem_entry_t)];
    for (uint32_t i = 0; i < req_len; i++) buf[i] = 0;

    virtio_gpu_cmd_attach_backing_t *cmd =
        (virtio_gpu_cmd_attach_backing_t *)buf;
    cmd->hdr.type    = VIRTIO_GPU_CMD_ATTACH_BACKING;
    cmd->resource_id = resource_id;
    cmd->nr_entries  = 1;

    virtio_gpu_mem_entry_t *entry =
        (virtio_gpu_mem_entry_t *)(buf + sizeof(virtio_gpu_cmd_attach_backing_t));
    entry->addr   = phys;
    entry->length = size;

    for (uint32_t i = 0; i < req_len; i++)
        vg.cmd_buf[i] = buf[i];
    return vg_ctrl_send(req_len);
}

static int vg_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                           uint32_t w, uint32_t h) {
    virtio_gpu_cmd_set_scanout_t *cmd =
        (virtio_gpu_cmd_set_scanout_t *)vg.cmd_buf;
    for (uint32_t i = 0; i < sizeof(virtio_gpu_cmd_set_scanout_t); i++)
        vg.cmd_buf[i] = 0;
    cmd->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->scanout_id  = scanout_id;
    cmd->resource_id = resource_id;
    cmd->x = 0;  cmd->y = 0;
    cmd->width = w;  cmd->height = h;
    return vg_ctrl_send(sizeof(virtio_gpu_cmd_set_scanout_t));
}

static int vg_transfer_to_host_2d(uint32_t resource_id, uint32_t w, uint32_t h) {
    virtio_gpu_cmd_transfer_flush_t *cmd =
        (virtio_gpu_cmd_transfer_flush_t *)vg.cmd_buf;
    for (uint32_t i = 0; i < sizeof(virtio_gpu_cmd_transfer_flush_t); i++)
        vg.cmd_buf[i] = 0;
    cmd->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->resource_id = resource_id;
    cmd->offset_x = 0;  cmd->offset_y = 0;
    cmd->width = w;     cmd->height = h;
    return vg_ctrl_send(sizeof(virtio_gpu_cmd_transfer_flush_t));
}

static int vg_resource_flush(uint32_t resource_id, uint32_t w, uint32_t h) {
    virtio_gpu_cmd_transfer_flush_t *cmd =
        (virtio_gpu_cmd_transfer_flush_t *)vg.cmd_buf;
    for (uint32_t i = 0; i < sizeof(virtio_gpu_cmd_transfer_flush_t); i++)
        vg.cmd_buf[i] = 0;
    cmd->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->resource_id = resource_id;
    cmd->offset_x = 0;  cmd->offset_y = 0;
    cmd->width = w;     cmd->height = h;
    return vg_ctrl_send(sizeof(virtio_gpu_cmd_transfer_flush_t));
}

/* ---- gpu_device_t callbacks ---- */

static int vg_gpu_present(gpu_device_t *dev) {
    (void)dev;
    if (!vg.ready) return -1;
    if (vg_transfer_to_host_2d(1, vg.width, vg.height) != 0) return -1;
    if (vg_resource_flush(1, vg.width, vg.height) != 0) return -1;
    return 0;
}

static int vg_gpu_set_cursor(gpu_device_t *dev, int x, int y,
                             const uint32_t *image, int w, int h) {
    (void)dev; (void)x; (void)y; (void)image; (void)w; (void)h;
    return -1;  /* no hardware cursor on virtio-gpu */
}

/* ---- Public init ---- */

int virtio_gpu_init(void) {
    const pci_device_t *pci = 0;
    uint64_t backing_phys;
    uint32_t pitch;
    uint64_t fb_size;
    uint64_t pages;

    serial_write("[virtio-gpu] scanning PCI...\n");

    /* Step 0: early-out when multiboot framebuffer already present */
    if (fb_available()) {
        serial_write("[virtio-gpu] skipped: multiboot fb present\n");
        return -1;
    }

    /* Step 1: PCI scan for vendor 0x1AF4, device 0x1050 */
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *cand = pci_device_at(i);
        if (!cand) continue;
        if (cand->vendor_id == 0x1AF4 &&
            cand->device_id == VIRTIO_GPU_PCI_DEVICE_ID) {
            pci = cand;
            break;
        }
    }
    if (!pci) {
        serial_write("[virtio-gpu] no device found\n");
        return -1;
    }
    serial_write("[virtio-gpu] found device\n");

    if (pci_enable_memory_busmaster(pci) != 0) {
        serial_write("[virtio-gpu] busmaster failed\n");
        return -1;
    }

    /* Step 2: Read BAR0 — must be I/O (PIO, bit 0 == 1) */
    uint32_t bar0 = pci_read_config32(pci, 0x10);
    if ((bar0 & 1) == 0) {
        serial_write("[virtio-gpu] BAR0 is not PIO\n");
        return -1;
    }
    uint16_t port = (uint16_t)(bar0 & ~0x3u);
    vg.port = port;
    vg.pci  = pci;
    serial_write("[virtio-gpu] PIO port ");
    vg_serial_u32(port);
    serial_write("\n");

    /* Step 3: lifecycle reset -> ACK -> Driver */
    vg_out8(port, VIRTIO_REG_DEVICE_STATUS, 0);
    vg_pause();
    vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vg_pause();
    vg_out8(port, VIRTIO_REG_DEVICE_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    vg_pause();

    /* Features: don't negotiate virgl/edid for minimal driver */
    (void)vg_in32(port, VIRTIO_REG_DEVICE_FEATURES);
    vg_out32(port, VIRTIO_REG_DRIVER_FEATURES, 0);
    vg_pause();

    /* Step 4: select queue 0, read negotiated size */
    vg_out16(port, VIRTIO_REG_QUEUE_SELECT, VIRTIO_GPU_CONTROLQ);
    vg_pause();
    uint16_t q_size = vg_in16(port, VIRTIO_REG_QUEUE_SIZE);
    if (q_size == 0) {
        serial_write("[virtio-gpu] controlq size=0\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    if (q_size > VIRTIO_GPU_CONTROLQ_SIZE) q_size = VIRTIO_GPU_CONTROLQ_SIZE;

    /* Allocate legacy packed queue (single contiguous region) */
    if (vg_init_controlq(q_size) != 0) {
        serial_write("[virtio-gpu] controlq alloc failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Tell device about control queue: Queue Address at 0x08 (phys >> 12) */
    vg_out16(port, VIRTIO_REG_QUEUE_SELECT, VIRTIO_GPU_CONTROLQ);
    vg_pause();
    vg_out32(port, VIRTIO_REG_QUEUE_ADDRESS, (uint32_t)(vg.vq_phys >> 12));
    vg_pause();
    /* NOTE: No 0x0C high-dword write — legacy has no such register;
     * it would clobber Queue Size (0x0C is read-only Queue Size in legacy). */

    /* DRIVER_OK */
    vg_out8(port, VIRTIO_REG_DEVICE_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    vg_pause();
    serial_write("[virtio-gpu] DRIVER_OK\n");

    /* Step 5: GET_DISPLAY_INFO */
    if (vg_get_display_info(&vg.width, &vg.height) != 0) {
        serial_write("[virtio-gpu] GET_DISPLAY_INFO failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    serial_write("[virtio-gpu] display ");
    vg_serial_u32(vg.width);
    serial_write("x");
    vg_serial_u32(vg.height);
    serial_write("\n");
    if (vg.width == 0 || vg.height == 0) {
        serial_write("[virtio-gpu] invalid dimensions\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Step 6: RESOURCE_CREATE_2D (resource_id=1, XRGB8888) */
    if (vg_resource_create_2d(1, vg.width, vg.height) != 0) {
        serial_write("[virtio-gpu] RESOURCE_CREATE_2D failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    serial_write("[virtio-gpu] resource created\n");

    /* Step 7: allocate backing pages */
    pitch  = vg.width * 4;
    fb_size = (uint64_t)pitch * (uint64_t)vg.height;
    pages  = (fb_size + 4095) / 4096;
    backing_phys = pmm_alloc_contiguous_below(pages, 0xFFFFFFFFULL);
    if (!backing_phys) {
        serial_write("[virtio-gpu] backing alloc failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    /* Zero the backing buffer */
    {
        uint8_t *v = (uint8_t *)PHYS_TO_VIRT(backing_phys);
        for (uint64_t i = 0; i < fb_size; i++) v[i] = 0;
    }
    serial_write("[virtio-gpu] backing allocated\n");

    /* Step 8: ATTACH_BACKING (single contiguous entry — valid since backing IS contiguous) */
    if (vg_attach_backing(1, backing_phys, (uint32_t)fb_size) != 0) {
        serial_write("[virtio-gpu] ATTACH_BACKING failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    serial_write("[virtio-gpu] backing attached\n");

    /* Step 9: SET_SCANOUT (scanout 0, resource 1) */
    if (vg_set_scanout(0, 1, vg.width, vg.height) != 0) {
        serial_write("[virtio-gpu] SET_SCANOUT failed\n");
        vg_out8(port, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    serial_write("[virtio-gpu] scanout set\n");

    /* Step 10: adopt as the system framebuffer — update framebuffer.c globals
     * so fb_print / fb_phys_addr / devnodes claim map all work. */
    fb_adopt(backing_phys, pitch, (int)vg.width, (int)vg.height, 32);
    fb_set_double_frame(0);
    serial_write("[virtio-gpu] framebuffer adopted\n");

    /* Register as gpu_device_t (only when no primary exists) */
    if (!gpu_primary()) {
        const char *name = "virtio-gpu";
        int i;
        for (i = 0; name[i] && i < GPU_NAME_MAX - 1; i++)
            virtio_gpu_dev.name[i] = name[i];
        virtio_gpu_dev.name[i] = 0;
        virtio_gpu_dev.mode_count    = 1;
        virtio_gpu_dev.modes[0].width  = (uint32_t)vg.width;
        virtio_gpu_dev.modes[0].height = (uint32_t)vg.height;
        virtio_gpu_dev.modes[0].pitch  = pitch;
        virtio_gpu_dev.modes[0].bpp    = 32;
        virtio_gpu_dev.current_mode    = 0;
        virtio_gpu_dev.fb_phys    = backing_phys;
        virtio_gpu_dev.fb_size    = fb_size;
        virtio_gpu_dev.hw_cursor  = 0;
        virtio_gpu_dev.present_supported = 1;
        virtio_gpu_dev.needs_present     = 1;
        virtio_gpu_dev.present     = vg_gpu_present;
        virtio_gpu_dev.set_cursor  = vg_gpu_set_cursor;
        virtio_gpu_dev.priv        = 0;
        virtio_gpu_dev.next        = 0;
        gpu_register_device(&virtio_gpu_dev);
    }

    vg.ready = 1;
    serial_write("[virtio-gpu] initialized OK\n");
    return 0;
}

int virtio_gpu_present(void) {
    return vg_gpu_present(0);
}

int virtio_gpu_ready(void) {
    return vg.ready;
}
