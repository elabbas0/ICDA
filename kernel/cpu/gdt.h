#ifndef GDT_H
#define GDT_H

#include <stdint.h>


struct gdt_entry {
    uint16_t limit_low;     
    uint16_t base_low;      
    uint8_t  base_mid;      
    uint8_t  access;        
    uint8_t  granularity;   
    uint8_t  base_high;     
} __attribute__((packed));


struct gdt_ptr {
    uint16_t limit;         
    uint64_t base;          
} __attribute__((packed));


#define GDT_PRESENT     (1 << 7)   
#define GDT_RING0       (0 << 5)   
#define GDT_RING3       (3 << 5)   
#define GDT_SYSTEM      (0 << 4)   
#define GDT_CODE_DATA   (1 << 4)   
#define GDT_EXEC        (1 << 3)   
#define GDT_DC          (1 << 2)   
#define GDT_RW          (1 << 1)   
#define GDT_ACCESSED    (1 << 0)   


#define GDT_GRAN_4K     (1 << 7)   
#define GDT_LONG_MODE   (1 << 5)   
#define GDT_32BIT       (1 << 6)   


#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20
#define GDT_TSS          0x28   



struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    
} __attribute__((packed));

void gdt_init();



void tss_set_rsp0(uint64_t rsp0);
void cpu_syscall_init(void);
uint64_t cpu_fs_base(void);
void cpu_set_fs_base(uint64_t base);

#endif