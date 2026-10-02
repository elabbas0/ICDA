#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>




struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};


struct multiboot_tag_framebuffer {
    uint32_t type;              
    uint32_t size;
    uint64_t framebuffer_addr;  
    uint32_t framebuffer_pitch; 
    uint32_t framebuffer_width; 
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;   
    uint8_t  framebuffer_type;  
    uint16_t reserved;
};


struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
};


#define MULTIBOOT_TAG_TYPE_CMDLINE      1
#define MULTIBOOT_TAG_TYPE_END          0
#define MULTIBOOT_TAG_TYPE_ACPI_OLD     14
#define MULTIBOOT_TAG_TYPE_ACPI_NEW     15
#define MULTIBOOT_TAG_TYPE_MMAP         6
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER  8

struct multiboot_tag_string {
    uint32_t type;
    uint32_t size;
    char string[0];
} __attribute__((packed));


#define MULTIBOOT_TAG_ALIGN             8


#define MULTIBOOT_MEMORY_AVAILABLE      1   
#define MULTIBOOT_MEMORY_RESERVED       2   
#define MULTIBOOT_MEMORY_ACPI           3   
#define MULTIBOOT_MEMORY_NVS            4   
#define MULTIBOOT_MEMORY_BADRAM         5   


struct multiboot_mmap_entry {
    uint64_t addr;   
    uint64_t len;    
    uint32_t type;   
    uint32_t zero;   
} __attribute__((packed));


struct multiboot_tag_mmap {
    uint32_t type;          
    uint32_t size;          
    uint32_t entry_size;    
    uint32_t entry_version; 
    struct multiboot_mmap_entry entries[0]; 
};



struct multiboot_tag_acpi {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[0];
} __attribute__((packed));

#endif
