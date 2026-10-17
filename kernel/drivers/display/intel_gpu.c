/* Intel integrated graphics, 9th generation (HD/UHD 5xx/6xx).
 *
 * Stage 1 of hardware acceleration: find the GPU, take its registers, wake
 * its graphics domain, reset the render engine, give it memory through the
 * global GTT and run one command of ours on it - a store of a known value -
 * through execlists, the only way a 9th-generation engine takes commands.
 * The result is at /dev/gpu.  Nothing here touches the display engine: the
 * firmware's framebuffer stays as it is.
 *
 * Register offsets, the context image layout (gen9_rcs_offsets) and the
 * command encodings follow Intel's documentation as used by the i915 driver
 * of Linux and OpenBSD (inteldrm), MIT licensed:
 *   Copyright (c) 2014 Intel Corporation.  Permission is hereby granted,
 *   free of charge, to any person obtaining a copy of this software ... to
 *   deal in the Software without restriction ... The above copyright notice
 *   and this permission notice shall be included in all copies or
 *   substantial portions of the Software. */

#include "intel_gpu.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../cpu/tsc.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"

#define MMIO_SIZE        (16ULL << 20)    /* GTTMMADR: registers, then the GTT from 8 MB */
#define GSM_OFFSET       (8ULL << 20)

/* registers */
#define FORCEWAKE_RENDER       0xA278
#define FORCEWAKE_ACK_RENDER   0x0D84
#define GDRST                  0x941C
#define   GRDOM_RENDER         (1u << 1)
#define GFX_FLSH_CNTL          0x101008
#define RCS                    0x2000     /* the render engine's registers */
#define RING_TAIL              0x30
#define RING_HEAD              0x34
#define RING_START             0x38
#define RING_CTL               0x3C
#define RING_IPEHR             0x68
#define RING_ACTHD             0x74
#define RING_HWS_PGA           0x80
#define RING_HWSTAM            0x98
#define RING_MI_MODE           0x9C
#define   STOP_RING            (1u << 8)
#define RING_RESET_CTL         0xD0
#define   RESET_CTL_REQUEST    (1u << 0)
#define   RESET_CTL_READY      (1u << 1)
#define RING_ELSP              0x230
#define RING_EXECLIST_STATUS   0x234
#define RING_CSB_PTR           0x3A0
#define RING_CSB_BUF(i)        (0x370 + (i) * 8)
#define   CSB_ACTIVE_TO_IDLE   (1u << 3)
#define RING_MODE              0x29C
#define   GFX_RUN_LIST_ENABLE  (1u << 15)

#define MASKED_ON(b)   (((b) << 16) | (b))
#define MASKED_OFF(b)  ((b) << 16)

/* commands */
#define MI_INSTR(op, flags)  (((op) << 23) | (flags))
#define MI_NOOP              MI_INSTR(0, 0)
#define MI_STORE_DWORD_IMM   MI_INSTR(0x20, 2)
#define   MI_USE_GGTT        (1u << 22)
#define MI_LOAD_REGISTER_IMM(n) MI_INSTR(0x22, 2 * (n) - 1)
#define   MI_LRI_FORCE_POSTED (1u << 12)

/* the logical ring context: page 0 the per-process status page, page 1 the
 * registers the engine loads, then its saved state (22 pages on gen9) */
#define CTX_PAGES            22
#define RING_PAGES           4
#define CTX_CONTEXT_CONTROL  (0x02 + 1)
#define CTX_RING_HEAD        (0x04 + 1)
#define CTX_RING_TAIL        (0x06 + 1)
#define CTX_RING_START       (0x08 + 1)
#define CTX_RING_CTL         (0x0a + 1)
#define CTX_MI_MODE          0x54
#define   CTX_CTRL_RESTORE_INHIBIT  (1u << 0)
#define   CTX_CTRL_RS_CTX_ENABLE    (1u << 1)
#define   CTX_CTRL_SAVE_INHIBIT     (1u << 2)
#define   CTX_CTRL_INHIBIT_SYN_SWITCH (1u << 3)
#define CTX_DESC_VALID       (1u << 0)
#define CTX_DESC_LEGACY_32B  (1u << 3)    /* addressing mode 1 */
#define CTX_DESC_PRIVILEGE   (1u << 8)

#define TEST_VALUE           0x1CDA600Du

/* The render context's register list: which registers, in which order, as
 * MI_LOAD_REGISTER_IMM groups.  NOP(n) skips n dwords, LRI(n, posted) starts
 * a group, REG/REG16 give an offset from the engine's base. */
#define NOP(x)        (0x80 | (x))
#define LRI(n, f)     (((f) << 6) | (n))
#define POSTED        1
#define REG(x)        ((x) >> 2)
#define REG16(x)      (((x) >> 9) | 0x80), (((x) >> 2) & 0x7f)
static const uint8_t gen9_rcs_offsets[] = {
    NOP(1), LRI(14, POSTED),
    REG16(0x244), REG(0x34), REG(0x30), REG(0x38), REG(0x3c), REG(0x168), REG(0x140), REG(0x110),
    REG(0x11c), REG(0x114), REG(0x118), REG(0x1c0), REG(0x1c4), REG(0x1c8),
    NOP(3), LRI(9, POSTED),
    REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280), REG16(0x27c), REG16(0x278),
    REG16(0x274), REG16(0x270),
    NOP(13), LRI(1, 0), REG(0xc8),
    NOP(13), LRI(44, POSTED),
    REG(0x28), REG(0x9c), REG(0xc0), REG(0x178), REG(0x17c), REG16(0x358), REG(0x170), REG(0x150),
    REG(0x154), REG(0x158), REG16(0x41c), REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c),
    REG16(0x610), REG16(0x614), REG16(0x618), REG16(0x61c), REG16(0x620), REG16(0x624), REG16(0x628),
    REG16(0x62c), REG16(0x630), REG16(0x634), REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644),
    REG16(0x648), REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c), REG16(0x660),
    REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674), REG16(0x678), REG16(0x67c),
    REG(0x68),
    0
};

static struct {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    volatile uint64_t *gsm;               /* the global GTT's entries */
    uint64_t gtt_entries;
    int found, awake, passed;
    char report[1024];
    uint64_t report_len;
} G;

static uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(G.mmio + reg); }
static void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(G.mmio + reg) = v; }

/* ---- the report ------------------------------------------------------------------ */

static void say(const char *s) {
    while (*s && G.report_len + 1 < sizeof(G.report)) G.report[G.report_len++] = *s++;
    G.report[G.report_len] = 0;
}

static void say_hex(uint64_t v) {
    char t[19];
    int n = 0;
    t[n++] = '0';
    t[n++] = 'x';
    for (int s = 28; s >= 0; s -= 4) t[n++] = "0123456789abcdef"[(v >> s) & 15];
    t[n] = 0;
    say(t);
}

static void say_dec(uint64_t v) {
    char t[24];
    int k = 0;
    char o[24];
    int n = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) o[n++] = t[--k];
    o[n] = 0;
    say(o);
}

/* the same on the serial port, so a machine that hangs still leaves a trace */
static void line(const char *s) {
    serial_write("[gpu] ");
    serial_write(s);
    serial_write("\n");
    say(s);
    say("\n");
}

/* ---- helpers -------------------------------------------------------------------- */

static int wait_bits(uint32_t reg, uint32_t mask, uint32_t want, uint64_t us) {
    uint64_t t0 = tsc_us();
    while ((rd(reg) & mask) != want) {
        if (tsc_us() - t0 > us) return -1;
    }
    return 0;
}

static void flush_range(const void *p, uint64_t bytes) {
    const uint8_t *c = (const uint8_t *)((uint64_t)p & ~63ULL), *end = (const uint8_t *)p + bytes;
    __asm__ volatile("mfence" ::: "memory");
    for (; c < end; c += 64) __asm__ volatile("clflush (%0)" :: "r"(c) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void zero_pages(void *p, uint64_t pages) {
    uint64_t *q = (uint64_t *)p;
    for (uint64_t i = 0; i < pages * 512; i++) q[i] = 0;
}

static int is_gen9(uint16_t id) {
    uint8_t hi = (uint8_t)(id >> 8);
    /* Skylake 19xx, Kaby Lake 59xx and 87xx, Coffee Lake 3Exx, Comet Lake 9Bxx */
    return hi == 0x19 || hi == 0x59 || hi == 0x87 || hi == 0x3E || hi == 0x9B;
}

/* the context image's register list, as the engine expects to find it */
static void build_context_regs(uint32_t *regs) {
    const uint8_t *data = gen9_rcs_offsets;
    while (*data) {
        uint8_t count, flags;
        if (*data & 0x80) {
            regs += *data++ & 0x7f;
            continue;
        }
        count = *data & 0x3f;
        flags = *data >> 6;
        data++;
        *regs = MI_LOAD_REGISTER_IMM(count) | ((flags & POSTED) ? MI_LRI_FORCE_POSTED : 0);
        regs++;
        do {
            uint32_t offset = 0;
            uint8_t v;
            do {
                v = *data++;
                offset = (offset << 7) | (v & 0x7f);
            } while (v & 0x80);
            regs[0] = RCS + (offset << 2);
            regs += 2;
        } while (--count);
    }
}

/* ---- the self-test ----------------------------------------------------------------- */

static int wake(void) {
    wr(FORCEWAKE_RENDER, MASKED_OFF(0xFFFF));
    if (wait_bits(FORCEWAKE_ACK_RENDER, 1, 0, 50000) != 0) {
        line("render power domain did not let go of an old wake request");
    }
    wr(FORCEWAKE_RENDER, MASKED_ON(1));
    if (wait_bits(FORCEWAKE_ACK_RENDER, 1, 1, 50000) != 0) {
        line("render power domain did not wake up");
        return -1;
    }
    G.awake = 1;
    return 0;
}

static void sleep_gt(void) {
    if (G.awake) wr(FORCEWAKE_RENDER, MASKED_OFF(1));
    G.awake = 0;
}

static int reset_render(void) {
    wr(RCS + RING_RESET_CTL, MASKED_ON(RESET_CTL_REQUEST));
    if (wait_bits(RCS + RING_RESET_CTL, RESET_CTL_READY, RESET_CTL_READY, 10000) != 0) {
        line("render engine did not get ready for a reset");
        wr(RCS + RING_RESET_CTL, MASKED_OFF(RESET_CTL_REQUEST));
        return -1;
    }
    wr(GDRST, GRDOM_RENDER);
    if (wait_bits(GDRST, GRDOM_RENDER, 0, 500000) != 0) {
        line("render engine reset did not finish");
        return -1;
    }
    wr(RCS + RING_RESET_CTL, MASKED_OFF(RESET_CTL_REQUEST));
    return 0;
}

/* A window of free GTT entries near the top: the firmware's framebuffer and
 * whatever else it mapped sit at the bottom.  Free entries are all alike
 * (zero, or all pointing at one scratch page). */
static int64_t free_window(uint64_t pages) {
    if (G.gtt_entries < pages + 1024) return -1;
    for (uint64_t start = G.gtt_entries - pages - 16; start > G.gtt_entries / 2; start -= 4096) {
        uint64_t first = G.gsm[start];
        int ok = 1;
        for (uint64_t i = 1; i < pages && ok; i++) if (G.gsm[start + i] != first) ok = 0;
        if (ok) return (int64_t)start;
    }
    return -1;
}

static int self_test(void) {
    const uint64_t pages = 1 + CTX_PAGES + RING_PAGES + 1;     /* status page, context, ring, target */
    uint64_t phys, gtt;
    int64_t slot;
    uint8_t *mem;
    uint32_t hwsp_gtt, ctx_gtt, ring_gtt, target_gtt, *regs, *ring, *target;
    uint64_t t0;

    phys = pmm_alloc_contiguous(pages);
    if (!phys) { line("no memory for the test"); return -1; }
    mem = (uint8_t *)PHYS_TO_VIRT(phys);
    zero_pages(mem, pages);

    slot = free_window(pages);
    if (slot < 0) { line("no free room in the GPU's address table"); return -1; }
    gtt = (uint64_t)slot * 4096;
    for (uint64_t i = 0; i < pages; i++) G.gsm[slot + i] = (phys + i * 4096) | 1;   /* present */
    (void)G.gsm[slot + pages - 1];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);

    hwsp_gtt = (uint32_t)gtt;
    ctx_gtt = (uint32_t)(gtt + 4096);
    ring_gtt = (uint32_t)(gtt + (1 + CTX_PAGES) * 4096);
    target_gtt = (uint32_t)(gtt + (1 + CTX_PAGES + RING_PAGES) * 4096);
    regs = (uint32_t *)(mem + 2 * 4096);                       /* the context's page 1 */
    ring = (uint32_t *)(mem + (1 + CTX_PAGES) * 4096);
    target = (uint32_t *)(mem + (1 + CTX_PAGES + RING_PAGES) * 4096);

    /* the command: store TEST_VALUE at target, through the global GTT */
    ring[0] = MI_STORE_DWORD_IMM | MI_USE_GGTT;
    ring[1] = target_gtt;
    ring[2] = 0;
    ring[3] = TEST_VALUE;

    build_context_regs(regs);
    /* the engine's saved state is not restored (there is none yet): only
     * the ring registers below are loaded */
    regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_CTRL_INHIBIT_SYN_SWITCH | CTX_CTRL_RESTORE_INHIBIT) |
                                MASKED_OFF(CTX_CTRL_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
    regs[CTX_RING_HEAD] = 0;
    regs[CTX_RING_TAIL] = 16;
    regs[CTX_RING_START] = ring_gtt;
    regs[CTX_RING_CTL] = ((RING_PAGES - 1) * 4096) | 1;        /* size - 4 KB, valid */
    regs[CTX_MI_MODE + 1] = MASKED_OFF(STOP_RING);
    flush_range(mem, pages * 4096);

    /* the engine: execlists on, running, its status page */
    wr(RCS + RING_HWSTAM, 0xFFFFFFFF);
    wr(RCS + RING_MODE, MASKED_ON(GFX_RUN_LIST_ENABLE));
    wr(RCS + RING_MI_MODE, MASKED_OFF(STOP_RING));
    wr(RCS + RING_HWS_PGA, hwsp_gtt);
    (void)rd(RCS + RING_HWS_PGA);

    /* submit: element 1 empty, then element 0 - its low half last */
    wr(RCS + RING_ELSP, 0);
    wr(RCS + RING_ELSP, 0);
    wr(RCS + RING_ELSP, 1);                                    /* context id */
    wr(RCS + RING_ELSP, ctx_gtt | CTX_DESC_LEGACY_32B | CTX_DESC_PRIVILEGE | CTX_DESC_VALID);

    t0 = tsc_us();
    while (tsc_us() - t0 < 500000) {
        flush_range(target, 64);
        if (*(volatile uint32_t *)target == TEST_VALUE) break;
    }
    say("waited ");
    say_dec((tsc_us() - t0) / 1000);
    say(" ms; head ");
    say_hex(rd(RCS + RING_HEAD));
    say(" tail ");
    say_hex(rd(RCS + RING_TAIL));
    say(" acthd ");
    say_hex(rd(RCS + RING_ACTHD));
    say(" execlist ");
    say_hex(rd(RCS + RING_EXECLIST_STATUS));
    say(" csb ");
    say_hex(rd(RCS + RING_CSB_PTR));
    say(" ipehr ");
    say_hex(rd(RCS + RING_IPEHR));
    say("\n");
    if (*(volatile uint32_t *)target != TEST_VALUE) {
        line("the render engine did not run the test command");
        return -1;
    }
    line("the render engine ran a command: GPU command submission works");
    return 0;
}

/* ---- stage 2: address spaces of their own ------------------------------------------
 *
 * Each program that uses the GPU gets a context and a 4-level page table
 * (a "PPGTT", 48-bit like the CPU's): its batches see only what is mapped
 * there.  The test makes two, maps the same GPU address to a different page
 * in each, runs an unprivileged batch in each that writes to that address
 * and checks that each write landed in its own page and nowhere else. */

#define GEN8_PRIVATE_PAT_LO   0x40E0
#define GEN8_PRIVATE_PAT_HI   0x40E4
#define PAGE_PRESENT          (1ULL << 0)
#define PAGE_RW               (1ULL << 1)
#define PPAT_CACHED           (1ULL << 7)          /* PAT bit: private PAT entry 4, write-back */
#define CTX_PDP0_UDW          (0x30 + 1)
#define CTX_PDP0_LDW          (0x32 + 1)
#define CTX_DESC_LEGACY_64B   (3u << 3)
#define MI_BATCH_BUFFER_START_GEN8 MI_INSTR(0x31, 1)
#define   MI_BATCH_PPGTT      (1u << 8)            /* from the context's address space, unprivileged */
#define MI_BATCH_BUFFER_END   MI_INSTR(0x0a, 0)
#define MI_ARB_ON_OFF         MI_INSTR(0x08, 0)
#define GFX_OP_PIPE_CONTROL(len) ((0x3u << 29) | (0x3u << 27) | (0x2u << 24) | ((len) - 2))
#define   PIPE_CONTROL_CS_STALL         (1u << 20)
#define   PIPE_CONTROL_TLB_INVALIDATE   (1u << 18)
#define   PIPE_CONTROL_QW_WRITE         (1u << 14)
#define   PIPE_CONTROL_GLOBAL_GTT       (1u << 24)
#define VM_BATCH              0x100000ULL          /* the GPU addresses the test uses */
#define VM_DATA               0x101000ULL

/* the private PAT, as Linux sets it on gen9: entry 0 write-back LLC (page
 * tables), 4-7 write-back LLC/eLLC (memory), 3 uncached */
static void setup_ppat(void) {
    uint64_t pat = (0x3ULL | (1 << 2)) |                          /* 0: WB, LLC */
                   ((uint64_t)(0x1 | (2 << 2)) << 8) |              /* 1: WC, LLC/eLLC */
                   ((uint64_t)0x3 << 16) |                          /* 2: WB (eLLC override) */
                   ((uint64_t)0x0 << 24);                           /* 3: UC */
    for (int i = 4; i < 8; i++) pat |= (uint64_t)(0x3 | (2 << 2) | ((i - 4) << 4)) << (i * 8);
    wr(GEN8_PRIVATE_PAT_LO, (uint32_t)pat);
    wr(GEN8_PRIVATE_PAT_HI, (uint32_t)(pat >> 32));
}

static uint64_t page_zeroed(void) {
    uint64_t p = pmm_alloc();
    if (p) zero_pages(PHYS_TO_VIRT(p), 1);
    return p;
}

typedef struct {
    uint64_t pml4, pdp, pd, pt, batch, data;            /* physical pages */
    uint32_t ctx_gtt, ring_gtt;
    uint32_t *regs, *ring;
} vm_ctx_t;

/* a context whose batch, at VM_BATCH, stores value at VM_DATA; when its
 * ring is done it writes value to the GGTT address done_gtt */
static int make_vm_ctx(vm_ctx_t *c, uint32_t value, uint32_t done_gtt) {
    const uint64_t pages = CTX_PAGES + RING_PAGES;
    uint64_t phys, *e;
    uint32_t *b, *r;
    int64_t slot;
    int n = 0;

    c->pml4 = page_zeroed(); c->pdp = page_zeroed(); c->pd = page_zeroed();
    c->pt = page_zeroed(); c->batch = page_zeroed(); c->data = page_zeroed();
    if (!c->pml4 || !c->pdp || !c->pd || !c->pt || !c->batch || !c->data) return -1;
    /* the page table walk for VM_BATCH and VM_DATA: index 0 at every level
     * above the last, entries 256 and 257 in it */
    ((uint64_t *)PHYS_TO_VIRT(c->pml4))[0] = c->pdp | PAGE_PRESENT | PAGE_RW;
    ((uint64_t *)PHYS_TO_VIRT(c->pdp))[0] = c->pd | PAGE_PRESENT | PAGE_RW;
    ((uint64_t *)PHYS_TO_VIRT(c->pd))[0] = c->pt | PAGE_PRESENT | PAGE_RW;
    e = (uint64_t *)PHYS_TO_VIRT(c->pt);
    e[(VM_BATCH >> 12) & 511] = c->batch | PAGE_PRESENT | PAGE_RW | PPAT_CACHED;
    e[(VM_DATA >> 12) & 511] = c->data | PAGE_PRESENT | PAGE_RW | PPAT_CACHED;

    b = (uint32_t *)PHYS_TO_VIRT(c->batch);
    b[0] = MI_STORE_DWORD_IMM;                                   /* no GGTT bit: this address space */
    b[1] = (uint32_t)VM_DATA;
    b[2] = (uint32_t)(VM_DATA >> 32);
    b[3] = value;
    b[4] = MI_BATCH_BUFFER_END;
    b[5] = MI_NOOP;

    /* the context image and its ring, in the global GTT */
    phys = pmm_alloc_contiguous(pages);
    if (!phys) return -1;
    zero_pages(PHYS_TO_VIRT(phys), pages);
    slot = free_window(pages);
    if (slot < 0) return -1;
    for (uint64_t i = 0; i < pages; i++) G.gsm[slot + i] = (phys + i * 4096) | 1;
    (void)G.gsm[slot + pages - 1];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
    c->ctx_gtt = (uint32_t)((uint64_t)slot * 4096);
    c->ring_gtt = c->ctx_gtt + CTX_PAGES * 4096;
    c->regs = (uint32_t *)((uint8_t *)PHYS_TO_VIRT(phys) + 4096);
    c->ring = (uint32_t *)((uint8_t *)PHYS_TO_VIRT(phys) + CTX_PAGES * 4096);

    r = c->ring;
    r[n++] = GFX_OP_PIPE_CONTROL(6);                             /* forget old translations */
    r[n++] = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT;
    r[n++] = done_gtt + 8;
    r[n++] = 0;
    r[n++] = 0;
    r[n++] = 0;
    r[n++] = MI_ARB_ON_OFF | 1;
    r[n++] = MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT;
    r[n++] = (uint32_t)VM_BATCH;
    r[n++] = (uint32_t)(VM_BATCH >> 32);
    r[n++] = MI_ARB_ON_OFF;
    r[n++] = MI_NOOP;
    r[n++] = MI_STORE_DWORD_IMM | MI_USE_GGTT;                   /* the ring itself: privileged */
    r[n++] = done_gtt;
    r[n++] = 0;
    r[n++] = value;

    build_context_regs(c->regs);
    c->regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_CTRL_INHIBIT_SYN_SWITCH | CTX_CTRL_RESTORE_INHIBIT) |
                                   MASKED_OFF(CTX_CTRL_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
    c->regs[CTX_RING_HEAD] = 0;
    c->regs[CTX_RING_TAIL] = (uint32_t)n * 4;
    c->regs[CTX_RING_START] = c->ring_gtt;
    c->regs[CTX_RING_CTL] = ((RING_PAGES - 1) * 4096) | 1;
    c->regs[CTX_MI_MODE + 1] = MASKED_OFF(STOP_RING);
    c->regs[CTX_PDP0_UDW] = (uint32_t)(c->pml4 >> 32);
    c->regs[CTX_PDP0_LDW] = (uint32_t)c->pml4;

    flush_range(PHYS_TO_VIRT(phys), pages * 4096);
    flush_range(PHYS_TO_VIRT(c->pml4), 4096);
    flush_range(PHYS_TO_VIRT(c->pdp), 4096);
    flush_range(PHYS_TO_VIRT(c->pd), 4096);
    flush_range(PHYS_TO_VIRT(c->pt), 4096);
    flush_range(PHYS_TO_VIRT(c->batch), 4096);
    flush_range(PHYS_TO_VIRT(c->data), 4096);
    return 0;
}

/* Reads the context status events up to the engine's write pointer and moves
 * the read pointer there: the buffer holds 6, and once it is full the engine
 * stops switching contexts.  With wait_idle, first waits (up to 100 ms) for
 * the event that says the engine went idle. */
static void ack_csb(int wait_idle) {
    uint64_t t0 = tsc_us();
    uint32_t r = (rd(RCS + RING_CSB_PTR) >> 8) & 7;
    for (;;) {
        uint32_t w = rd(RCS + RING_CSB_PTR) & 7;
        int idle = 0;
        if (w != 7) {
            while (r != w) {
                r = r >= 5 ? 0 : r + 1;
                if (rd(RCS + RING_CSB_BUF(r)) & CSB_ACTIVE_TO_IDLE) idle = 1;
            }
            wr(RCS + RING_CSB_PTR, ((7u << 8) << 16) | (r << 8));
        }
        if (!wait_idle || idle || tsc_us() - t0 > 100000) return;
    }
}

static int run_vm_ctx(vm_ctx_t *c, uint32_t id, volatile uint32_t *done, uint32_t value) {
    uint64_t t0;
    wr(RCS + RING_ELSP, 0);
    wr(RCS + RING_ELSP, 0);
    wr(RCS + RING_ELSP, id);
    wr(RCS + RING_ELSP, c->ctx_gtt | CTX_DESC_LEGACY_64B | CTX_DESC_PRIVILEGE | CTX_DESC_VALID);
    t0 = tsc_us();
    while (tsc_us() - t0 < 500000) {
        flush_range((const void *)done, 64);
        if (*done == value) { ack_csb(1); return 0; }
    }
    return -1;
}

static void say_engine(void) {
    say("execlist ");
    say_hex(rd(RCS + RING_EXECLIST_STATUS));
    say(" context ");
    say_hex(rd(RCS + RING_EXECLIST_STATUS + 4));
    say(" acthd ");
    say_hex(rd(RCS + RING_ACTHD));
    say(" head ");
    say_hex(rd(RCS + RING_HEAD));
    say(" tail ");
    say_hex(rd(RCS + RING_TAIL));
    say(" ipehr ");
    say_hex(rd(RCS + RING_IPEHR));
    say(" csb ");
    say_hex(rd(RCS + RING_CSB_PTR));
    say("\n");
}

static uint32_t data_word(const vm_ctx_t *c) {
    volatile uint32_t *d = (volatile uint32_t *)PHYS_TO_VIRT(c->data);
    flush_range((const void *)d, 64);
    return *d;
}

static int stage2_test(void) {
    static vm_ctx_t a, b;
    const uint32_t va = 0xC0DE00A1u, vb = 0xC0DE00B2u;
    uint64_t done_phys;
    int64_t slot;
    volatile uint32_t *done;
    uint32_t done_gtt;

    setup_ppat();
    done_phys = page_zeroed();
    slot = done_phys ? free_window(1) : -1;
    if (slot < 0) { line("stage 2: no room for the test"); return -1; }
    G.gsm[slot] = done_phys | 1;
    (void)G.gsm[slot];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
    done_gtt = (uint32_t)((uint64_t)slot * 4096);
    done = (volatile uint32_t *)PHYS_TO_VIRT(done_phys);
    flush_range((const void *)done, 4096);

    if (make_vm_ctx(&a, va, done_gtt) != 0 || make_vm_ctx(&b, vb, done_gtt + 64)) {
        line("stage 2: could not set up the address spaces");
        return -1;
    }
    ack_csb(1);                                     /* stage 1's events */
    if (run_vm_ctx(&a, 2, done, va) != 0) {
        say("stage 2: context A did not finish; ");
        say_engine();
        return -1;
    }
    if (run_vm_ctx(&b, 3, done + 16, vb) != 0) {
        say("stage 2: context B did not finish; ");
        say_engine();
        return -1;
    }
    say("stage 2: page A ");
    say_hex(data_word(&a));
    say(", page B ");
    say_hex(data_word(&b));
    say("\n");
    if (data_word(&a) != va || data_word(&b) != vb) {
        line("stage 2: a batch wrote outside its own address space");
        return -1;
    }
    line("stage 2: two address spaces ran their own batches, each wrote only its own memory");
    return 0;
}

int intel_gpu_init(int disabled) {
    uint32_t bar_lo, bar_hi;
    uint64_t bar;
    uint16_t gmch;

    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d && d->vendor_id == 0x8086 && d->class_code == 0x03) { G.pci = d; break; }
    }
    if (!G.pci) {
        say("no Intel graphics\n");
        return -1;
    }
    G.found = 1;
    say("Intel graphics, device ");
    say_hex(G.pci->device_id);
    say(is_gen9(G.pci->device_id) ? " (9th generation)\n" : " (not 9th generation: not supported yet)\n");
    if (!is_gen9(G.pci->device_id)) return -1;
    if (disabled) {
        line("turned off (icda.nogpu=1)");
        return -1;
    }

    (void)pci_enable_memory_busmaster(G.pci);
    bar_lo = pci_read_config32(G.pci, 0x10);
    bar_hi = (bar_lo & 0x6) == 0x4 ? pci_read_config32(G.pci, 0x14) : 0;
    bar = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFULL);
    if (!bar) { line("no register window"); return -1; }
    G.mmio = (volatile uint8_t *)vmm_map_physical(bar, MMIO_SIZE, VMM_FLAGS_KERNEL_RW | PTE_NO_CACHE);
    if (!G.mmio) { line("register window could not be mapped"); return -1; }
    G.gsm = (volatile uint64_t *)(G.mmio + GSM_OFFSET);
    gmch = pci_read_config16(G.pci, 0x50);
    {
        uint32_t ggms = (gmch >> 6) & 3;
        G.gtt_entries = ggms ? ((1ULL << ggms) << 20) / 8 : 0;
    }
    say("address table: ");
    say_dec(G.gtt_entries * 4096 >> 20);
    say(" MB\n");
    if (!G.gtt_entries) { line("the firmware gave the GPU no address table"); return -1; }

    serial_write("[gpu] waking the render engine\n");
    if (wake() != 0) return -1;
    if (reset_render() == 0) {
        G.passed = self_test() == 0;
        /* stage 2 hand-drives context switches; the driver below has its
         * own (status events, hang reset), and Surfer tries the GPU in a
         * separate process before using it - so a failure here is
         * reported, not fatal */
        if (G.passed && stage2_test() != 0)
            line("stage 2 failed: starting the driver anyway (Surfer checks the GPU before using it)");
    }
    if (!G.passed) {
        sleep_gt();
        return -1;
    }
    /* the GPU stays awake from here: the Linux driver interface (i915.c)
     * runs it, and every submission would otherwise wake it first */
    if (i915_init() == 0) line("Linux graphics driver interface ready: /dev/dri/renderD128");
    else line("the Linux graphics driver interface did not start");
    return 0;
}

uint64_t intel_gpu_node_read(char *buf, uint64_t cap) {
    uint64_t n = 0;
    if (!cap) return 0;
    if (!G.report_len) say("GPU driver not started\n");
    while (n + 1 < cap && n < G.report_len) { buf[n] = G.report[n]; n++; }
    buf[n] = 0;
    return n;
}

/* ---- for the Linux driver interface (i915.c) ---------------------------------------- */

uint32_t igpu_rd(uint32_t reg) { return rd(reg); }
void igpu_wr(uint32_t reg, uint32_t v) { wr(reg, v); }
const pci_device_t *igpu_pci(void) { return G.pci; }
void igpu_report(const char *s) { line(s); }
void igpu_flush(const void *p, uint64_t bytes) { flush_range(p, bytes); }
void igpu_context_regs(uint32_t *regs) { build_context_regs(regs); }
int igpu_reset_render(void) { return reset_render(); }
void igpu_setup_ppat(void) { setup_ppat(); }

/* a run of free global GTT entries; the index of the first, or -1 */
int64_t igpu_ggtt_window(uint64_t pages) { return free_window(pages); }
void igpu_ggtt_set(uint64_t entry, uint64_t phys) { G.gsm[entry] = phys | 1; }
void igpu_ggtt_flush(void) {
    (void)G.gsm[0];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
}
