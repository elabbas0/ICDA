#ifndef DISPLAY_FLIP_H
#define DISPLAY_FLIP_H

#include <stdint.h>




int flip_probe(uint64_t fb_phys, uint32_t pitch, uint32_t height,
               uint32_t bpp, uint64_t vmm_addr_space);




uint8_t *flip_back_buffer(void);


void flip_swap(void);


int flip_active(void);

#endif 
