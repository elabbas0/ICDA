#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

// GRUB fills and passes the address via ebx 

// every tag starts with these two fields
struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

// tag type 8 = framebuffer info
struct multiboot_tag_framebuffer {
    uint32_t type;              // always 8
    uint32_t size;
    uint64_t framebuffer_addr;  // physical address of framebuffer
    uint32_t framebuffer_pitch; // bytes per row
    uint32_t framebuffer_width; // pixels wide
    uint32_t framebuffer_height;// pixels tall
    uint8_t  framebuffer_bpp;   // bits per pixel (32 = ARGB)
    uint8_t  framebuffer_type;  // 1 = RGB color
    uint16_t reserved;
};

// the info struct header
struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
};

// tag types required
#define MULTIBOOT_TAG_TYPE_END          0
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER  8

// alignment between tags
#define MULTIBOOT_TAG_ALIGN             8

#endif