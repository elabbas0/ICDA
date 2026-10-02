




































#include "pat.h"
#include "../drivers/serial/serial.h"
#include "../drivers/console/console.h"
#include <stdint.h>



#define MSR_IA32_PAT 0x277


#define PAT_WB  0x00  
#define PAT_WT  0x01  
#define PAT_UCM 0x02  
#define PAT_UC  0x03  
#define PAT_WC  0x01  
#define PAT_WP  0x07  













#define PAT_WC_SLOT     4
#define PAT_WC_BYTE_IDX PAT_WC_SLOT  



static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
        : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr) : "memory");
}




static inline void cpu_serialize(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
}



static void serial_hex64(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;
    buf[16] = '\0';
    for (i = 15; i >= 0; i--)
        buf[15 - i] = hex[(v >> (uint64_t)(i * 4)) & 0xFULL];
    serial_write(buf);
}






static int wc_available;

int pat_wc_available(void)
{
    return wc_available;
}

int pat_init_wc(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t pat;

    
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1u << 16))) {
        serial_write("[PAT] CPUID: PAT not supported; using default caching\n");
        return 0;
    }

    
    pat = rdmsr(MSR_IA32_PAT);

    

    pat &= ~(0xFFULL << (PAT_WC_BYTE_IDX * 8));
    pat |= ((uint64_t)PAT_WC << (PAT_WC_BYTE_IDX * 8));

    
    cpu_serialize();
    wrmsr(MSR_IA32_PAT, pat);
    cpu_serialize();

    
    pat = rdmsr(MSR_IA32_PAT);

    
    serial_write("[PAT] IA32_PAT=");
    serial_hex64(pat);
    serial_write(" slot ");
    serial_write_char((char)('0' + PAT_WC_SLOT));
    serial_write("=WC(01h) fb mapped WC OK\n");

    
    console_write("PAT WC=slot ", CONSOLE_STYLE_OK);
    console_write_char((char)('0' + PAT_WC_SLOT), CONSOLE_STYLE_OK);
    console_write(" IA32_PAT=", CONSOLE_STYLE_OK);
    console_write_hex64(pat, CONSOLE_STYLE_OK);
    console_write(" WC on\n", CONSOLE_STYLE_OK);

    wc_available = 1;
    return 1;
}
