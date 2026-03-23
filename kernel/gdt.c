#include "gdt.h"

// null, kernel code, kernel data, user code, user data
#define GDT_ENTRIES 5

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;

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
        GDT_GRAN_4K | GDT_LONG_MODE);

    // user code segment - ring 3, executable, 64-bit
    gdt_set_entry(3, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_EXEC | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    // user data segment - ring 3, writable
    gdt_set_entry(4, 0, 0xFFFFF,
        GDT_PRESENT | GDT_RING3 | GDT_CODE_DATA | GDT_RW,
        GDT_GRAN_4K | GDT_LONG_MODE);

    gdt_flush((uint64_t)&gp);
}