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

// tag types
#define MULTIBOOT_TAG_TYPE_END          0
#define MULTIBOOT_TAG_TYPE_MMAP         6
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER  8

// alignment between tags
#define MULTIBOOT_TAG_ALIGN             8

// memory map entry types
#define MULTIBOOT_MEMORY_AVAILABLE      1   // free RAM — safe to use
#define MULTIBOOT_MEMORY_RESERVED       2   // firmware/hardware reserved
#define MULTIBOOT_MEMORY_ACPI           3   // ACPI reclaimable
#define MULTIBOOT_MEMORY_NVS            4   // ACPI non-volatile (do not touch)
#define MULTIBOOT_MEMORY_BADRAM         5   // defective memory

// a single entry in the memory map
struct multiboot_mmap_entry {
    uint64_t addr;   // physical base address of this region
    uint64_t len;    // length in bytes
    uint32_t type;   // one of MULTIBOOT_MEMORY_* above
    uint32_t zero;   // reserved, always 0
} __attribute__((packed));

// tag type 6 = memory map
struct multiboot_tag_mmap {
    uint32_t type;          // always 6
    uint32_t size;          // total size of this tag including entries
    uint32_t entry_size;    // size of each entry (usually 24 bytes)
    uint32_t entry_version; // currently 0
    struct multiboot_mmap_entry entries[0]; // variable-length array of entries
};

#endif