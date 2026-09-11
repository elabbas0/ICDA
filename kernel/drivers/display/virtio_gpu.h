#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

/* Virtio GPU PCI Device ID (vendor 0x1AF4) */
#define VIRTIO_GPU_PCI_DEVICE_ID  0x1050

/* Controlq index */
#define VIRTIO_GPU_CONTROLQ      0

/* Control command types */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO    0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF      0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT         0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH      0x0109
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0110
#define VIRTIO_GPU_CMD_ATTACH_BACKING      0x0112

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA        0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO  0x1101

/* Pixel format */
#define VIRTIO_GPU_FORMAT_XRGB8888  1

/* ---- Virtio-gpu controlq structs (packed, spec-matching) ---- */

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint32_t reserved[2];
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct {
    uint32_t primary_width;
    uint32_t primary_height;
    uint8_t  enabled;
    uint8_t  flags;
    uint8_t  reserved[2];
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[16];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_cmd_create_2d_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by virtio_gpu_mem_entry_t[nr_entries] */
} __attribute__((packed)) virtio_gpu_cmd_attach_backing_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t scanout_id;
    uint32_t resource_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_cmd_set_scanout_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_cmd_transfer_flush_t;

/* ---- Virtqueue ring structs (same layout as virtio_net) ---- */

#define VIRTIO_GPU_CONTROLQ_SIZE 64

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtio_gpu_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_GPU_CONTROLQ_SIZE];
} __attribute__((packed)) virtio_gpu_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtio_gpu_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtio_gpu_used_elem_t ring[VIRTIO_GPU_CONTROLQ_SIZE];
} __attribute__((packed)) virtio_gpu_used_t;

/* ---- Public API ---- */

/* Initialize virtio-gpu.  Returns 0 on success, -1 if no device or failure.
 * Only called when fb_available() == 0 (no multiboot framebuffer). */
int virtio_gpu_init(void);

/* Transfer + flush full framebuffer to host.  Returns 0 on success, -1 on error. */
int virtio_gpu_present(void);

/* 1 when the virtio-gpu device is initialized and the scanout is live. */
int virtio_gpu_ready(void);

#endif
