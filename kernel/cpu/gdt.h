#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// each GDT entry is 8 bytes
struct gdt_entry {
    uint16_t limit_low;     // lower 16 bits of limit
    uint16_t base_low;      // lower 16 bits of base
    uint8_t  base_mid;      // next 8 bits of base
    uint8_t  access;        // access flags
    uint8_t  granularity;   // granularity + upper 4 bits of limit
    uint8_t  base_high;     // upper 8 bits of base
} __attribute__((packed));

// the GDT pointer loaded via lgdt
struct gdt_ptr {
    uint16_t limit;         // size of GDT - 1
    uint64_t base;          // address of GDT
} __attribute__((packed));

// access byte flags
#define GDT_PRESENT     (1 << 7)   // segment present
#define GDT_RING0       (0 << 5)   // privilege level 0 (kernel)
#define GDT_RING3       (3 << 5)   // privilege level 3 (user)
#define GDT_SYSTEM      (0 << 4)   // system segment
#define GDT_CODE_DATA   (1 << 4)   // code or data segment
#define GDT_EXEC        (1 << 3)   // executable (code segment)
#define GDT_DC          (1 << 2)   // direction/conforming
#define GDT_RW          (1 << 1)   // readable/writable
#define GDT_ACCESSED    (1 << 0)   // accessed (set by CPU)

// granularity byte flags
#define GDT_GRAN_4K     (1 << 7)   // page granularity
#define GDT_LONG_MODE   (1 << 5)   // 64-bit segment
#define GDT_32BIT       (1 << 6)   // 32-bit segment

// segment selectors (index * 8)
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20

void gdt_init();

#endif