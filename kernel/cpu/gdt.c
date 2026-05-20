#include "gdt.h"

// null, kernel code, kernel data, user code, user data, TSS low, TSS high
#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;
static struct tss       tss;   // one global TSS

static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity) {
    gdt[index].base_low   = base & 0xFFFF;
    gdt[index].base_mid   = (base >> 16) & 0xFF;
    gdt[index].base_high  = (base >> 24) & 0xFF;
    gdt[index].limit_low  = limit & 0xFFFF;
    gdt[index].granularity = (granularity & 0xF0) | ((limit >> 16) & 0x0F);
    gdt[index].access     = access;
}

// reloads segment registers with new GDT
extern void gdt_flush(uint64_t gdt_ptr);

void gdt_init() {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)&gdt;

    // null descriptor - required as first entry
    gdt_set_entry(0, 0, 0, 0, 0);

    // kernel code segment - ring 0, executable, 64-bit
    gdt_set_entry(1, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING0 | GDT_CODE_DATA | GDT_EXEC | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    // kernel data segment - ring 0, writable
    gdt_set_entry(2, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING0 | GDT_CODE_DATA | GDT_RW,
        GDT_GRAN_4K | GDT_32BIT);

    // user code segment - ring 3, executable, 64-bit
    gdt_set_entry(3, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_EXEC | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    // user data segment - ring 3, writable
    gdt_set_entry(4, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_RW,
        GDT_GRAN_4K | GDT_32BIT);

    // TSS descriptor — 64-bit TSS takes two 8-byte GDT slots (16 bytes total)
    // Format: base[31:0] split across the standard fields, type=0x89 (available TSS)
    // The high slot holds base[63:32] in its low 32 bits, rest zero.
    uint64_t tss_base  = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;

    // clear TSS and set iomap_base past the end (disables I/O permission bitmap)
    for (int i = 0; i < (int)sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.iomap_base = sizeof(tss);

    // low 8 bytes of TSS descriptor (slot 5)
    gdt[5].limit_low   =  tss_limit & 0xFFFF;
    gdt[5].base_low    =  tss_base & 0xFFFF;
    gdt[5].base_mid    = (tss_base >> 16) & 0xFF;
    gdt[5].access      =  GDT_PRESENT | 0x09;   // 0x09 = available 64-bit TSS
    gdt[5].granularity = ((tss_limit >> 16) & 0x0F);
    gdt[5].base_high   = (tss_base >> 24) & 0xFF;

    // high 8 bytes of TSS descriptor (slot 6) — upper 32 bits of base, rest 0
    uint32_t *tss_high = (uint32_t *)&gdt[6];
    tss_high[0] = (uint32_t)(tss_base >> 32);
    tss_high[1] = 0;

    gdt_flush((uint64_t)&gp);

    // load the TSS into TR (task register) — selector 0x28, RPL=0
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
