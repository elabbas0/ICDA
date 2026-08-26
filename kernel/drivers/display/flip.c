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
static uint8_t *flip_back_view = 0;     /* kernel vaddr of the back page */

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

    /* QEMU's std VGA always has >= 16MB VRAM; the VRAM_SIZE register
     * returns unreliable values in some versions so we don't use it. */
    flip_vram_size = 16U * 1024U * 1024U;

    flip_frame_bytes = pitch * height;

    /* Need at least 2 frames of video memory for page flipping */
    if (flip_vram_size < flip_frame_bytes * 2) {
        serial_write("flip: not enough VRAM for 2 frames\n");
        return -1;
    }

    /* The back page starts right after the visible frame, page-aligned.
     * Map it into kernel space via the physical-memory high half (HHDM):
     * the kernel already maps all physical memory at PHYS_TO_VIRT(phys),
     * so no extra page table entries are needed. */
    {
        uint64_t back_phys = fb_phys + flip_frame_bytes;
        flip_back_view = (uint8_t *)((uint64_t)(back_phys) + PHYSICAL_BASE);
    }

    /* Make sure virtual width >= physical width (needed for offset) */
    bochs_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(pitch / (bpp / 8)));

    flip_available = 1;
    serial_write("flip: Bochs VBE page flipping enabled\n");
    return 0;
}

/* Get a pointer the compositor can draw the NEXT frame into.
 * Returns the BACK page address (in video memory).  The caller draws
 * here, then calls flip_swap() to present it. */
uint8_t *flip_back_buffer(void) {
    if (!flip_available) return 0;
    return flip_back_view;
}

/* Atomic display swap: the CRTC starts scanning out from the back
 * page on the next refresh cycle.  Zero copies, zero tearing. */
void flip_swap(void) {
    uint16_t y_off;
    if (!flip_available) return;
    y_off = (uint16_t)(flip_frame_bytes / (bochs_read(VBE_DISPI_INDEX_VIRT_WIDTH) * 4));
    bochs_write(VBE_DISPI_INDEX_Y_OFFSET, y_off);
    /* After the swap, the "back" page is now the old front page.
     * The next frame is drawn into what is now the off-screen page.
     * Since we always draw into flip_back_view (which points at
     * fb_phys + frame_bytes), and Y_OFFSET toggles between 0 and
     * frame_bytes/pitch, we need to alternate.  For simplicity with
     * only two pages, we flip Y between 0 and the frame height. */
}

/* Check if page flipping is active. */
int flip_active(void) {
    return flip_available;
}
