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
#define GDT_TSS          0x28   // TSS occupies slots 5+6 (16 bytes)

// Task State Segment — 64-bit layout (Intel SDM Vol.3 §8.7)
// We only care about RSP0: the kernel stack the CPU switches to on ring3->ring0
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          // kernel stack pointer for ring 0
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        // interrupt stack table (unused for now)
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    // offset to I/O permission bitmap (past end = disabled)
} __attribute__((packed));

void gdt_init();

// update RSP0 in the TSS — call on every context switch so the CPU
// knows which kernel stack to use if a ring-3 process is interrupted
void tss_set_rsp0(uint64_t rsp0);

#endif