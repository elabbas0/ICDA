/*
 * fpu.c — x87/SSE enablement and per-thread state switching.
 *
 * Long mode guarantees SSE2, so no CPUID gate is needed for the base
 * feature set.  The OS has to opt in explicitly though: with
 * CR4.OSFXSR clear every SSE instruction raises #UD, and with CR0.EM
 * set every x87 instruction raises #NM.
 *
 * MXCSR 0x1F80 masks all SIMD exceptions (the SysV default), and
 * fninit loads the default x87 control word 0x037F.  A zeroed save
 * area would unmask every exception, so new threads copy the template
 * captured here instead of starting from zeroes.
 */
#include "fpu.h"

#define CR0_MP        (1ULL << 1)
#define CR0_EM        (1ULL << 2)
#define CR0_TS        (1ULL << 3)
#define CR4_OSFXSR    (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define MXCSR_DEFAULT 0x1F80u

static uint8_t fpu_template[FPU_STATE_SIZE] __attribute__((aligned(16)));

void fpu_init(void) {
    uint64_t cr0;
    uint64_t cr4;
    uint32_t mxcsr = MXCSR_DEFAULT;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])fpu_template));
}

void fpu_state_init(uint8_t *area) {
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        area[i] = fpu_template[i];
    }
}

void fpu_switch(uint8_t *prev, const uint8_t *next) {
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])prev));
    __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t (*)[FPU_STATE_SIZE])next));
}
