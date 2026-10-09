#include "fpu.h"

#define CR0_MP        (1ULL << 1)
#define CR0_EM        (1ULL << 2)
#define CR0_TS        (1ULL << 3)
#define CR4_OSFXSR    (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE   (1ULL << 18)
#define MXCSR_DEFAULT 0x1F80u
#define XCR0_X87_SSE_AVX 0x7u

/* With XSAVE the thread state holds the AVX registers too.  Without it the
 * CPU reported AVX (CPUID) that the OS had not enabled, and code that only
 * checks CPUID - WebKit's JavaScript JIT - died on its first AVX
 * instruction (SIGILL) on real Skylake hardware. */
static int use_xsave, template_done;
static uint8_t fpu_template[FPU_STATE_SIZE] __attribute__((aligned(64)));

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

void fpu_init(void) {
    uint64_t cr0;
    uint64_t cr4;
    uint32_t mxcsr = MXCSR_DEFAULT;
    uint32_t a, b, c, d;
    uint32_t xcr0 = 0x3u;                       /* x87 + SSE */

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    cpuid(1, 0, &a, &b, &c, &d);
    if (c & (1u << 26)) {                       /* XSAVE */
        cr4 |= CR4_OSXSAVE;
        if (c & (1u << 28)) xcr0 = XCR0_X87_SSE_AVX;
    }
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    if (cr4 & CR4_OSXSAVE) {
        __asm__ volatile("xsetbv" : : "c"(0), "a"(xcr0), "d"(0));
        cpuid(0xD, 0, &a, &b, &c, &d);          /* b: bytes for the enabled state */
        use_xsave = b <= FPU_STATE_SIZE;
        if (!use_xsave) __asm__ volatile("xsetbv" : : "c"(0), "a"(0x3u), "d"(0));
    }

    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    if (template_done) return;          /* each CPU sets itself up; the template once */
    template_done = 1;
    for (int i = 0; i < FPU_STATE_SIZE; i++) fpu_template[i] = 0;
    if (use_xsave) __asm__ volatile("xsave64 %0" : "+m"(*(uint8_t (*)[FPU_STATE_SIZE])fpu_template) : "a"(~0u), "d"(~0u));
    else __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])fpu_template));
}

void fpu_state_init(uint8_t *area) {
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        area[i] = fpu_template[i];
    }
}

void fpu_switch(uint8_t *prev, const uint8_t *next) {
    if (use_xsave) {
        __asm__ volatile("xsave64 %0" : "+m"(*(uint8_t (*)[FPU_STATE_SIZE])prev) : "a"(~0u), "d"(~0u));
        __asm__ volatile("xrstor64 %0" : : "m"(*(const uint8_t (*)[FPU_STATE_SIZE])next), "a"(~0u), "d"(~0u));
        return;
    }
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])prev));
    __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t (*)[FPU_STATE_SIZE])next));
}
