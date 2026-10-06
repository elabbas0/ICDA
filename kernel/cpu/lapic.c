#include "lapic.h"

#include "../memory/vmm.h"

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_BSP (1ULL << 8)
#define IA32_APIC_BASE_EN  (1ULL << 11)
#define IA32_APIC_BASE_ADDR_MASK 0xFFFFF000ULL

#define LAPIC_REG_ID          0x020
#define LAPIC_REG_TPR         0x080
#define LAPIC_REG_EOI         0x0B0
#define LAPIC_REG_SVR         0x0F0
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_LVT_LINT0   0x350
#define LAPIC_REG_LVT_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370
#define LAPIC_REG_INITIAL_CNT 0x380
#define LAPIC_REG_CURRENT_CNT 0x390
#define LAPIC_REG_DIVIDE      0x3E0

#define LAPIC_SVR_ENABLE      0x100
#define LAPIC_LVT_MASKED      (1U << 16)
#define LAPIC_TIMER_PERIODIC  (1U << 17)

static volatile uint32_t *lapic_base = 0;
static uint64_t lapic_phys = 0;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
    (void)lapic_read(LAPIC_REG_ID);
}

int lapic_init(uint64_t madt_lapic_phys) {
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);

    if (!(apic_base & IA32_APIC_BASE_EN)) {
        apic_base |= IA32_APIC_BASE_EN;
    }
    if (madt_lapic_phys) {
        apic_base = (apic_base & ~IA32_APIC_BASE_ADDR_MASK) |
                    (madt_lapic_phys & IA32_APIC_BASE_ADDR_MASK);
    }
    wrmsr(IA32_APIC_BASE_MSR, apic_base);

    lapic_phys = apic_base & IA32_APIC_BASE_ADDR_MASK;
    lapic_base = (volatile uint32_t *)vmm_map_physical(
        lapic_phys, PAGE_SIZE_4K, VMM_WRITE | PTE_NO_CACHE | PTE_WRITE_THRU);
    if (!lapic_base) {
        return -1;
    }

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SVR, 47 | LAPIC_SVR_ENABLE);
    lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, 46);
    lapic_stop_timer();

    return 0;
}

void lapic_disable(void) {
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR);
    apic_base &= ~IA32_APIC_BASE_EN;
    wrmsr(IA32_APIC_BASE_MSR, apic_base);
}

void lapic_eoi(void) {
    if (!lapic_base) {
        return;
    }
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_start_timer(uint8_t vector) {
    if (!lapic_base) {
        return;
    }

    lapic_write(LAPIC_REG_DIVIDE, 0x3);
    lapic_write(LAPIC_REG_LVT_TIMER, vector | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_INITIAL_CNT, 100000000U);
}

void lapic_stop_timer(void) {
    if (!lapic_base) {
        return;
    }

    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | 32);
    lapic_write(LAPIC_REG_INITIAL_CNT, 0);
}

uint32_t lapic_id(void) {
    if (!lapic_base) {
        return 0;
    }
    return lapic_read(LAPIC_REG_ID) >> 24;
}

uint64_t lapic_physical_base(void) {
    return lapic_phys;
}

#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310

/* Enables this AP's local APIC; the MMIO mapping is shared with the BSP. */
void lapic_enable_ap(void) {
    uint64_t apic_base = rdmsr(IA32_APIC_BASE_MSR) | IA32_APIC_BASE_EN;
    wrmsr(IA32_APIC_BASE_MSR, apic_base);
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SVR, 47 | LAPIC_SVR_ENABLE);
    lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, 46);
    lapic_stop_timer();
}

void lapic_send_ipi(uint32_t apic_id, uint32_t low) {
    if (!lapic_base) return;
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
    for (uint32_t i = 0; i < 1000000U && (lapic_read(LAPIC_REG_ICR_LOW) & (1U << 12)); i++)
        __asm__ volatile("pause");
}

void lapic_timer_periodic(uint8_t vector, uint32_t count) {
    if (!lapic_base || !count) return;
    lapic_write(LAPIC_REG_DIVIDE, 0x3);
    lapic_write(LAPIC_REG_LVT_TIMER, vector | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_INITIAL_CNT, count);
}

/* Measures LAPIC timer counts (divide by 16) per PIT tick. */
uint32_t lapic_calibrate(void (*wait_ticks)(uint64_t)) {
    uint32_t left;
    if (!lapic_base) return 0;
    lapic_write(LAPIC_REG_DIVIDE, 0x3);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | 32);
    wait_ticks(1);
    lapic_write(LAPIC_REG_INITIAL_CNT, 0xFFFFFFFFU);
    wait_ticks(10);
    left = lapic_read(LAPIC_REG_CURRENT_CNT);
    lapic_stop_timer();
    return (0xFFFFFFFFU - left) / 10;
}

/* PIC mode: LINT0 delivers the 8259's INTR (ExtINT), LINT1 is NMI. */
void lapic_virtual_wire(void) {
    if (!lapic_base) return;
    lapic_write(LAPIC_REG_LVT_LINT0, 0x700);
    lapic_write(LAPIC_REG_LVT_LINT1, 0x400);
}
