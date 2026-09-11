/*
 * flip.c - display page flipping via Bochs VBE (QEMU std VGA).
 *
 * The Bochs VBE interface exposes I/O ports 0x01CE/0x01CF for display
 * configuration, including the Y-offset register that controls which
 * part of video memory the CRTC scans out.  By drawing into an
 * off-screen page and flipping Y-offset, the display never sees a
 * partially written frame - tearing and ghosts become structurally
 * impossible.
 *
 * On real hardware without Bochs VBE, the probe fails and the system
 * falls back to direct-to-FB compositing (current behaviour).
 *
 * Double-buffer layout in VRAM (requires VIRT_HEIGHT = 2 * yres):
 *   Page 0: offset 0            (initial front, shown by default)
 *   Page 1: offset frame_bytes  (initial back)
 * flip_page tracks which page the CRTC is currently scanning.
 * flip_back_buffer() returns the CPU-write target (the other page).
 */
#include "flip.h"
#include "../../memory/vmm.h"
#include "../../drivers/serial/serial.h"
#include <stdint.h>

/* Physical-to-virtual offset (from vmm.h) */
#ifndef PHYSICAL_BASE
#define PHYSICAL_BASE 0xFFFF800000000000ULL
#endif

/* Bochs VBE I/O ports and register indices */
#define VBE_DISPI_IOPORT_INDEX      0x01CE
#define VBE_DISPI_IOPORT_DATA       0x01CF
#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_BANK        0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9
#define VBE_DISPI_INDEX_VRAM_SIZE   0xA

#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED  0x01

static int flip_available = 0;
static uint32_t flip_frame_bytes = 0;   /* one frame: pitch * height */
static uint32_t flip_vram_size = 0;
static uint64_t flip_fb_phys = 0;       /* from Multiboot2 */
static uint8_t *flip_page0_view = 0;    /* kernel vaddr of page 0 */
static uint8_t *flip_page1_view = 0;    /* kernel vaddr of page 1 */
static int flip_page = 0;               /* 0 = CRTC shows page 0 */
static uint16_t flip_height = 0;        /* single-frame height for Y_OFFSET */

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint16_t bochs_read(uint16_t reg) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bochs_write(uint16_t reg, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, reg);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

int flip_probe(uint64_t fb_phys, uint32_t pitch, uint32_t height,
               uint32_t bpp, uint64_t vmm_addr_space) {
    uint16_t id;
    uint16_t vram16;
    (void)vmm_addr_space;

    flip_available = 0;
    flip_fb_phys = fb_phys;

    /* Bochs VBE ID should be 0xB0C0 or higher */
    id = bochs_read(VBE_DISPI_INDEX_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        flip_available = 0;
        return -1;
    }

    /* Query VRAM size from the Bochs register (returns 64KB units).
     * Fall back to 16 MB when the register returns zero (some QEMU
     * versions do not implement it). */
    vram16 = bochs_read(VBE_DISPI_INDEX_VRAM_SIZE);
    if (vram16 >= 256) {            /* >= 256 * 64KB = 16 MB */
        flip_vram_size = (uint32_t)vram16 * 64U * 1024U;
    } else {
        flip_vram_size = 16U * 1024U * 1024U;
    }

    flip_frame_bytes = pitch * height;
    flip_height = (uint16_t)height;

    /* Need at least 2 frames of video memory for page flipping */
    if (flip_vram_size < flip_frame_bytes * 2) {
        serial_write("flip: not enough VRAM for 2 frames\n");
        flip_available = 0;
        return -1;
    }

    /* Map both pages into kernel space via HHDM */
    flip_page0_view = (uint8_t *)((uint64_t)(fb_phys) + PHYSICAL_BASE);
    flip_page1_view = (uint8_t *)((uint64_t)(fb_phys + flip_frame_bytes) + PHYSICAL_BASE);



    /* Configure virtual framebuffer: width >= physical (needed for
     * Y-offset) and height = 2 * yres so the CRTC can scan either page. */
    bochs_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(pitch / (bpp / 8)));
    bochs_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)(height * 2));



    /* Hardening: read VIRT_HEIGHT back. Cores that ignore the write
     * (verified: QEMU std VGA returns a VRAM-derived constant whether
     * enabled or disabled) cannot scan a second page, so fall back to
     * the legacy direct-blit path instead of freezing on page 0. */
    if (bochs_read(VBE_DISPI_INDEX_VIRT_HEIGHT) != (uint16_t)(height * 2)) {
        serial_write("flip: VIRT_HEIGHT unsupported, using direct blit\n");
        bochs_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        flip_available = 0;
        return -1;
    }

    /* Start with Y_OFFSET = 0 (page 0 is the front). */
    bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    flip_page = 0;

    flip_available = 1;
    serial_write("flip: Bochs VBE page flipping enabled\n");
    return 0;
}

/* Get a pointer the compositor can draw the NEXT frame into.
 * Returns the BACK page address (in video memory).  The caller draws
 * here, then calls flip_swap() to present it. */
uint8_t *flip_back_buffer(void) {
    if (!flip_available) return 0;
    /* Return the page that the CRTC is NOT currently scanning. */
    return (flip_page == 0) ? flip_page1_view : flip_page0_view;
}

/* Atomic display swap: the CRTC starts scanning out from the back
 * page on the next refresh cycle.  Zero copies, zero tearing.
 * Alternates Y_OFFSET between 0 and frame_height to ping-pong
 * between the two pages. */
void flip_swap(void) {
    if (!flip_available) return;
    if (flip_page == 0) {
        /* Currently showing page 0; flip to page 1. */
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, flip_height);
        flip_page = 1;
    } else {
        /* Currently showing page 1; flip back to page 0. */
        bochs_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        flip_page = 0;
    }
}

/* Check if page flipping is active. */
int flip_active(void) {
    return flip_available;
}
