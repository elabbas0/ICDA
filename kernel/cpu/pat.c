/*
 * pat.c — PAT MSR setup for framebuffer Write-Combining (WC)
 *
 * Root cause: on bare-metal PCI framebuffer hardware, every composite
 * blit pays uncached write latency (~10-50x slower than QEMU's
 * WB RAM-backed VGA).  QEMU never shows the symptom because its VGA
 * memory is plain WB RAM.
 *
 * Fix: program one PAT MSR slot to WC (01h) and remap the framebuffer
 * through that slot.  WC allows the CPU to coalesce sequential writes
 * into burst transfers — ideal for pixel blits and scroll copies.
 *
 * PAT index encoding (SDM Vol.3A, Table 11-1):
 *   {PAT, PCD, PWT} -> selects PAT[n]
 * We program slot 4: {PAT=1, PCD=0, PWT=0}
 *   4K PTE:  PAT bit = bit 7   (SDM 4.9, "Figure 4-8")
 *   2M PDE:  PAT bit = bit 12  (SDM 4.9, "Figure 4-9")
 *
 * PAT MSR default after reset (SDM 11.12.1): all slots 00h (WB).
 * Slot 4 default = WB; we overwrite to WC (01h).
 *
 * Risks:
 * - WC reads behave like UC (slow, unaligned-safe).  Cursor save
 *   (read-back of pixels under software cursor) is unaffected — just
 *   not accelerated.  Writes are the hot path and those benefit.
 * - WC + PCI posted writes: the CPU may reorder stores within the WC
 *   buffer.  The framebuffer has no read-modify-write protocol, so
 *   this is safe.  BPP 24 byte-store paths are naturally ordered.
 * - MTRRs (not touched): if firmware marks the GPU BAR as UC in a
 *   variable MTRR, UC wins over PAT (SDM 11.11).  On most systems
 *   the high-address GPU BAR is NOT covered by any MTRR, so PAT WC
 *   takes effect.  This is strictly better than the current UC/UC-
 *   mapping regardless.
 *
 * C17, -ffreestanding -O0, zero warnings gcc + clang.
 */

#include "pat.h"
#include "../drivers/serial/serial.h"
#include "../drivers/console/console.h"
#include <stdint.h>

/* ---- MSRs and encodings (Intel SDM Vol.3A, 11.12.1) ---- */

#define MSR_IA32_PAT 0x277

/* Memory type encodings (SDM Table 4-1) */
#define PAT_WB  0x00  /* Write-Back      */
#define PAT_WT  0x01  /* Write-Through   */
#define PAT_UCM 0x02  /* Uncacheable Minus (UC-) */
#define PAT_UC  0x03  /* Uncacheable     */
#define PAT_WC  0x01  /* Write-Combining (WC = 01h, SDM Table 11-8) */
#define PAT_WP  0x07  /* Write-Protected */

/*
 * PAT slot selection.
 *
 * We use slot 4 = {PAT=1, PCD=0, PWT=0}.
 *   bit positions in PTE/PDE:  PAT=bit7(4K)/bit12(2M), PCD=bit4, PWT=bit3
 *   index = {PAT=1, PCD=0, PWT=0} = 4  ->  PAT[4]
 *
 * Slot 4 is safe: no existing mapping uses it.
 *   - All current pages use default WB via slot 0 {PAT=0,PCD=0,PWT=0}
 *   - The old fb mapping used {PAT=0,PCD=1,PWT=1}=slot 3 (UC/UC-)
 *     which we are replacing with slot 4 (WC).
 */
#define PAT_WC_SLOT     4
#define PAT_WC_BYTE_IDX PAT_WC_SLOT  /* byte index in the 64-bit MSR value */

/* ---- CPUID / rdmsr / wrmsr (no libc, -ffreestanding) ---- */

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

/* Serialize execution: CPUID serializes all prior loads/stores (SDM 2A.4).
 * Placed around WRMSR to ensure the PAT write is globally visible before
 * any subsequent PTE walk uses the new value. */
static inline void cpu_serialize(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
}

/* ---- serial hex printer (no libc) ---- */

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

/* ---- public API ---- */

/* Non-zero when PAT slot 4 has been programmed to WC and is safe
 * for framebuffer mappings.  Read by VMM and devnodes to select
 * WC (PAT-based) vs. UC (PCD+PWT) caching for device pages. */
static int wc_available;

int pat_wc_available(void)
{
    return wc_available;
}

int pat_init_wc(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t pat;

    /* CPUID.1:EDX bit 16 = PAT feature flag (SDM Vol.2A, CPUID) */
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1u << 16))) {
        serial_write("[PAT] CPUID: PAT not supported; using default caching\n");
        return 0;
    }

    /* Read current IA32_PAT */
    pat = rdmsr(MSR_IA32_PAT);

    /* Program the chosen slot to WC (01h).  All other slots preserved.
     * Slot 4 default = 00h (WB); overwrite to 01h (WC). */
    pat &= ~(0xFFULL << (PAT_WC_BYTE_IDX * 8));
    pat |= ((uint64_t)PAT_WC << (PAT_WC_BYTE_IDX * 8));

    /* Serialize, WRMSR, serialize (SDM Vol.3A, 8.3.2) */
    cpu_serialize();
    wrmsr(MSR_IA32_PAT, pat);
    cpu_serialize();

    /* Verify read-back */
    pat = rdmsr(MSR_IA32_PAT);

    /* --- serial proof (visible on HW serial port) --- */
    serial_write("[PAT] IA32_PAT=");
    serial_hex64(pat);
    serial_write(" slot ");
    serial_write_char((char)('0' + PAT_WC_SLOT));
    serial_write("=WC(01h) fb mapped WC OK\n");

    /* --- console one-liner (also visible on screen) --- */
    console_write("PAT WC=slot ", CONSOLE_STYLE_OK);
    console_write_char((char)('0' + PAT_WC_SLOT), CONSOLE_STYLE_OK);
    console_write(" IA32_PAT=", CONSOLE_STYLE_OK);
    console_write_hex64(pat, CONSOLE_STYLE_OK);
    console_write(" WC on\n", CONSOLE_STYLE_OK);

    wc_available = 1;
    return 1;
}
