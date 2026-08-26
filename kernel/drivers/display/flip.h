#ifndef DISPLAY_FLIP_H
#define DISPLAY_FLIP_H

#include <stdint.h>

/* Probe Bochs VBE for page flipping support.  Call after the Multiboot2
 * framebuffer info is available.  vmm_addr_space is the kernel page table
 * for mapping the back page. */
int flip_probe(uint64_t fb_phys, uint32_t pitch, uint32_t height,
               uint32_t bpp, uint64_t vmm_addr_space);

/* Pointer to the off-screen (back) page in video memory.  The compositor
 * draws the next frame here, then calls flip_swap().  Returns 0 if
 * page flipping is not available. */
uint8_t *flip_back_buffer(void);

/* Atomic display swap: the CRTC starts scanning from the back page. */
void flip_swap(void);

/* 1 if page flipping is active, 0 if falling back to direct-to-FB. */
int flip_active(void);

#endif /* DISPLAY_FLIP_H */
