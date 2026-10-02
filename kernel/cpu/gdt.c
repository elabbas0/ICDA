#include "gdt.h"


#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;
static struct tss       tss;   

static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity) {
    gdt[index].base_low   = base & 0xFFFF;
    gdt[index].base_mid   = (base >> 16) & 0xFF;
    gdt[index].base_high  = (base >> 24) & 0xFF;
    gdt[index].limit_low  = limit & 0xFFFF;
    gdt[index].granularity = (granularity & 0xF0) | ((limit >> 16) & 0x0F);
    gdt[index].access     = access;
}


extern void gdt_flush(uint64_t gdt_ptr);

void gdt_init() {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)&gdt;

    
    gdt_set_entry(0, 0, 0, 0, 0);

    
    gdt_set_entry(1, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING0 | GDT_CODE_DATA | GDT_EXEC | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    
    gdt_set_entry(2, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING0 | GDT_CODE_DATA | GDT_RW,
        GDT_GRAN_4K | GDT_32BIT);

    
    gdt_set_entry(3, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_EXEC | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    
    gdt_set_entry(4, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_RW,
        GDT_GRAN_4K | GDT_32BIT);

    
    
    
    uint64_t tss_base  = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;

    
    for (int i = 0; i < (int)sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.iomap_base = sizeof(tss);

    
    gdt[5].limit_low   =  tss_limit & 0xFFFF;
    gdt[5].base_low    =  tss_base & 0xFFFF;
    gdt[5].base_mid    = (tss_base >> 16) & 0xFF;
    gdt[5].access      =  GDT_PRESENT | 0x09;   
    gdt[5].granularity = ((tss_limit >> 16) & 0x0F);
    gdt[5].base_high   = (tss_base >> 24) & 0xFF;

    
    uint32_t *tss_high = (uint32_t *)&gdt[6];
    tss_high[0] = (uint32_t)(tss_base >> 32);
    tss_high[1] = 0;

    gdt_flush((uint64_t)&gp);

    
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
