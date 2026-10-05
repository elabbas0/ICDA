#include "smp.h"
#include "lapic.h"
#include "fpu.h"
#include "pat.h"
#include "../firmware/acpi.h"
#include "../drivers/serial/serial.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"

#define AP_TRAMPOLINE 0x8000ULL
#define AP_STACK_PAGES 4

cpu_t smp_cpus[SMP_MAX_CPUS];
static uint32_t cpu_count = 1;
static uint8_t apic_to_cpu[256];
static volatile uint32_t bkl_next = 0;
static volatile uint32_t bkl_serving = 0;
static uint32_t lapic_ticks_per_tick = 0;

extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[];
extern void idt_reload(void);
void cpu_syscall_init(void);

cpu_t *this_cpu(void) {
    if (cpu_count <= 1) return &smp_cpus[0];
    return &smp_cpus[apic_to_cpu[lapic_id() & 0xFF]];
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

int bkl_enter(void) {
    cpu_t *c = this_cpu();
    if (c->bkl_held) return 0;
    {
        /* Ticket lock: CPUs get the kernel in arrival order, so a CPU busy
         * with syscalls cannot starve the one handling device interrupts. */
        uint32_t ticket = __sync_fetch_and_add(&bkl_next, 1);
        while (bkl_serving != ticket) __asm__ volatile("pause");
    }
    c->bkl_held = 1;
    return 1;
}

void bkl_exit(void) {
    cpu_t *c = this_cpu();
    if (!c->bkl_held) return;
    c->bkl_held = 0;
    __sync_synchronize();
    bkl_serving = bkl_serving + 1;
}

static void log_num(const char *what, uint64_t v) {
    char buf[24];
    int n = 0;
    serial_write(what);
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        char c[2] = { buf[--n], 0 };
        serial_write(c);
    }
    serial_write("\n");
}

/* Waits for n PIT ticks with interrupts briefly enabled (boot only). */
static void wait_ticks(uint64_t n) {
    uint64_t end = sched_ticks() + n;
    while (sched_ticks() < end) __asm__ volatile("sti; hlt; cli");
}

static void gdt_setup_cpu(cpu_t *c) {
    uint64_t base = (uint64_t)&c->tss_store;
    uint32_t limit = sizeof(struct tss) - 1;
    struct gdt_ptr bsp_gp;
    __asm__ volatile("sgdt %0" : "=m"(bsp_gp));
    for (int i = 0; i < 5; i++) c->gdt[i] = ((struct gdt_entry *)bsp_gp.base)[i];
    for (uint64_t i = 0; i < sizeof(struct tss); i++) ((uint8_t *)&c->tss_store)[i] = 0;
    c->tss_store.iomap_base = sizeof(struct tss);
    c->gdt[5].limit_low = limit & 0xFFFF;
    c->gdt[5].base_low = base & 0xFFFF;
    c->gdt[5].base_mid = (base >> 16) & 0xFF;
    c->gdt[5].access = GDT_PRESENT | 0x09;
    c->gdt[5].granularity = (limit >> 16) & 0x0F;
    c->gdt[5].base_high = (base >> 24) & 0xFF;
    ((uint32_t *)&c->gdt[6])[0] = (uint32_t)(base >> 32);
    ((uint32_t *)&c->gdt[6])[1] = 0;
    c->gp.limit = sizeof(c->gdt) - 1;
    c->gp.base = (uint64_t)&c->gdt;
    c->tss = &c->tss_store;
}

extern void gdt_flush(uint64_t gdt_ptr);

void ap_entry(cpu_t *c) {
    gdt_flush((uint64_t)&c->gp);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
    idt_reload();
    fpu_init();
    pat_init_wc();
    lapic_enable_ap();
    cpu_syscall_init();
    c->current = c->idle;
    c->idle->kernel_stack_top = c->kstack_top;
    c->tss->rsp0 = c->kstack_top;
    c->online = 1;
    lapic_timer_periodic(SMP_TIMER_VECTOR, lapic_ticks_per_tick);
    sched_idle_loop();
}

static int start_ap(cpu_t *c) {
    volatile uint64_t *param = (volatile uint64_t *)PHYS_TO_VIRT(AP_TRAMPOLINE + 8);
    uint64_t stack = pmm_alloc_contiguous(AP_STACK_PAGES);
    uint64_t cr0, cr3, cr4, efer;
    uint32_t lo, hi;
    if (!stack) return -1;
    c->kstack_top = (uint64_t)PHYS_TO_VIRT(stack) + AP_STACK_PAGES * PAGE_SIZE_4K;
    c->idle = sched_create_idle(c->index);
    if (!c->idle) return -1;
    gdt_setup_cpu(c);
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
    efer = (((uint64_t)hi << 32) | lo) & ((1ULL << 0) | (1ULL << 8) | (1ULL << 11));
    param[0] = vmm_kernel_address_space()->pml4_phys;
    param[1] = cr4;
    param[2] = cr0;
    param[3] = c->kstack_top;
    param[4] = (uint64_t)c;
    param[5] = (uint64_t)ap_entry;
    param[6] = efer;
    (void)cr3;
    lapic_send_ipi(c->apic_id, 0x4500);           /* INIT */
    wait_ticks(2);
    for (int i = 0; i < 2 && !c->online; i++) {
        lapic_send_ipi(c->apic_id, 0x4600 | (AP_TRAMPOLINE >> 12)); /* SIPI */
        wait_ticks(1);
    }
    for (int i = 0; i < 50 && !c->online; i++) wait_ticks(1);
    return c->online ? 0 : -1;
}

void smp_init(void *multiboot_info) {
    const struct acpi_madt *madt;
    if (!acpi_madt()) (void)acpi_init(multiboot_info);
    madt = acpi_madt();
    uint32_t bsp_apic = lapic_id();
    const uint8_t *p, *end;
    uint32_t found = 0;

    if (!madt) {
        serial_write("[smp] no MADT, single CPU\n");
        return;
    }
    /* With the 8259 PIC in charge the LAPIC was never set up: enable it and
     * keep LINT0 as the PIC's virtual wire so legacy IRQs still arrive. */
    if (!lapic_physical_base()) {
        if (lapic_init(madt->lapic_address) != 0) return;
        lapic_virtual_wire();
    }
    bsp_apic = lapic_id();
    smp_cpus[0].self = (uint64_t)&smp_cpus[0];
    smp_cpus[0].apic_id = bsp_apic;
    smp_cpus[0].online = 1;
    apic_to_cpu[bsp_apic & 0xFF] = 0;

    lapic_ticks_per_tick = lapic_calibrate(wait_ticks);
    log_num("[smp] lapic counts per tick: ", lapic_ticks_per_tick);
    if (!lapic_ticks_per_tick) return;
    {
        uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(AP_TRAMPOLINE);
        for (uint64_t i = 0; i < size; i++) dst[i] = ap_trampoline_start[i];
    }

    p = madt->entries;
    end = (const uint8_t *)madt + madt->header.length;
    while (p + 2 <= end && p[1] >= 2) {
        if (p[0] == 0 && p[1] >= 8) {
            uint8_t apic = p[3];
            uint32_t flags = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            if ((flags & 3) && apic != bsp_apic && cpu_count < SMP_MAX_CPUS) {
                cpu_t *c = &smp_cpus[cpu_count];
                c->self = (uint64_t)c;
                c->index = cpu_count;
                c->apic_id = apic;
                apic_to_cpu[apic] = (uint8_t)cpu_count;
                found++;
                cpu_count++;
                if (start_ap(c) != 0) {
                    cpu_count--;
                    serial_write("[smp] AP failed to start\n");
                }
            }
        }
        p += p[1];
    }
    log_num("[smp] cpus online: ", cpu_count);
    (void)found;
}
