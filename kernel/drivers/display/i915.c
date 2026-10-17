/* Linux's i915 driver interface, on ICDA's Intel GPU driver (9th generation).
 *
 * Mesa's Intel driver (iris) runs unmodified on top of this: it allocates
 * GEM buffers, maps them into its own GPU address space (softpin), and
 * submits batches with execbuffer2; fences are syncobjs.  What it needs and
 * nothing more:
 *
 *   buffers    pages allocated when first used, shared through mmap
 *              (i915_client_frame) and dma-buf files (PRIME, lx_drm.c)
 *   address    one 48-bit, 4-level page table per VM; unmapped addresses
 *   spaces     read a scratch page instead of faulting
 *   contexts   a logical ring context each (image and ring in the global
 *              GTT), all on the render engine
 *   ordering   every request gets the next global sequence number and runs
 *              strictly in that order, so a batch never runs before one it
 *              may depend on - another process's included
 *   completion a breadcrumb after each batch writes its sequence number; a
 *              kernel thread (and every waiter) moves the queue on
 *   hangs      no progress for 3 s: the render engine is reset, the
 *              context at fault is banned and every waiter released
 *
 * The workarounds, the MOCS table and the command sequences around each
 * batch follow the MIT-licensed i915 driver of Linux and OpenBSD
 * (gt/intel_workarounds.c, intel_mocs.c, gen8_engine_cs.c), as do the uAPI
 * structures below (include/uapi/drm/i915_drm.h, drm.h):
 *   Copyright (c) 2014-2023 Intel Corporation.  Permission is hereby granted,
 *   free of charge, to any person obtaining a copy of this software ... to
 *   deal in the Software without restriction ... The above copyright notice
 *   and this permission notice shall be included in all copies or
 *   substantial portions of the Software. */

#include "i915.h"
#include "intel_gpu.h"
#include "../serial/serial.h"
#include "../../cpu/tsc.h"
#include "../../memory/heap.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#include "../../proc/sched.h"
#include "../../syscall/uaccess.h"

/* ---- Linux errno values ---------------------------------------------------------- */
#define EPERM_   1
#define ENOENT_  2
#define EINTR_   4
#define EIO_     5
#define ENOMEM_  12
#define EFAULT_  14
#define EBUSY_   16
#define ENODEV_  19
#define EINVAL_  22
#define ENOSPC_  28
#define ETIME_   62
#define EOPNOTSUPP_ 95

/* ---- registers and commands -------------------------------------------------------- */
#define RCS                    0x2000
#define RING_HWS_PGA           (RCS + 0x80)
#define RING_HWSTAM            (RCS + 0x98)
#define RING_MI_MODE           (RCS + 0x9C)
#define RING_ELSP              (RCS + 0x230)
#define RING_CSB_PTR           (RCS + 0x3A0)
#define RING_CSB_BUF(i)        (RCS + 0x370 + (i) * 8)
#define RING_MODE              (RCS + 0x29C)
#define RING_TIMESTAMP         (RCS + 0x358)
#define STOP_RING              (1u << 8)
#define GFX_RUN_LIST_ENABLE    (1u << 15)
#define MASKED_ON(b)           (((b) << 16) | (b))
#define MASKED_OFF(b)          ((b) << 16)
#define MASKED_FIELD(m, v)     (((m) << 16) | (v))

#define MI_INSTR(op, f)        (((op) << 23) | (f))
#define MI_NOOP                0
#define MI_USER_INTERRUPT      MI_INSTR(0x02, 0)
#define MI_ARB_CHECK           MI_INSTR(0x05, 0)
#define MI_ARB_ON_OFF          MI_INSTR(0x08, 0)
#define MI_LOAD_REGISTER_IMM(n) MI_INSTR(0x22, 2 * (n) - 1)
#define MI_BATCH_BUFFER_START_GEN8 MI_INSTR(0x31, 1)
#define GFX_OP_PIPE_CONTROL(len) ((0x3u << 29) | (0x3u << 27) | (0x2u << 24) | ((len) - 2))
#define PC_GLOBAL_GTT          (1u << 24)
#define PC_STORE_DATA_INDEX    (1u << 21)
#define PC_CS_STALL            (1u << 20)
#define PC_TLB_INVALIDATE      (1u << 18)
#define PC_QW_WRITE            (1u << 14)
#define PC_RT_FLUSH            (1u << 12)
#define PC_INSTR_INVALIDATE    (1u << 11)
#define PC_TEX_INVALIDATE      (1u << 10)
#define PC_FLUSH_ENABLE        (1u << 7)
#define PC_DC_FLUSH            (1u << 5)
#define PC_VF_INVALIDATE       (1u << 4)
#define PC_CONST_INVALIDATE    (1u << 3)
#define PC_STATE_INVALIDATE    (1u << 2)
#define PC_DEPTH_FLUSH         (1u << 0)
#define PPHWSP_SCRATCH         (0x34 * 4)

/* the logical ring context */
#define CTX_PAGES              22
#define RING_PAGES             16
#define RING_BYTES             (RING_PAGES * 4096)
#define CTX_CONTEXT_CONTROL    (0x02 + 1)
#define CTX_RING_HEAD          (0x04 + 1)
#define CTX_RING_TAIL          (0x06 + 1)
#define CTX_RING_START         (0x08 + 1)
#define CTX_RING_CTL           (0x0a + 1)
#define CTX_PDP0_UDW           (0x30 + 1)
#define CTX_PDP0_LDW           (0x32 + 1)
#define CTX_MI_MODE            0x54
#define CTX_CTRL_RESTORE_INHIBIT  (1u << 0)
#define CTX_CTRL_RS_CTX_ENABLE    (1u << 1)
#define CTX_CTRL_SAVE_INHIBIT     (1u << 2)
#define CTX_CTRL_INHIBIT_SYN      (1u << 3)
#define DESC_VALID             (1u << 0)
#define DESC_LEGACY_64B        (3u << 3)
#define DESC_PRIVILEGE         (1u << 8)

/* page tables */
#define PTE_PRESENT            (1ULL << 0)
#define PTE_RW                 (1ULL << 1)
#define PTE_CACHED             (1ULL << 7)      /* private PAT entry 4: write-back LLC/eLLC */
#define VA_MASK                ((1ULL << 48) - 1)

/* ---- uAPI structures ------------------------------------------------------------- */
struct drm_version { int32_t major, minor, patch; uint64_t name_len, name; uint64_t date_len, date; uint64_t desc_len, desc; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_syncobj_create { uint32_t handle, flags; };
struct drm_syncobj_destroy { uint32_t handle, pad; };
struct drm_syncobj_wait { uint64_t handles; int64_t timeout_nsec; uint32_t count_handles, flags, first_signaled, pad; uint64_t deadline_nsec; };
struct drm_syncobj_array { uint64_t handles; uint32_t count_handles, pad; };
struct i915_getparam { int32_t param; uint32_t pad; uint64_t value; };
struct i915_gem_create { uint64_t size; uint32_t handle, pad; };
struct i915_gem_create_ext { uint64_t size; uint32_t handle, flags; uint64_t extensions; };
struct i915_gem_mmap_offset { uint32_t handle, pad; uint64_t offset, flags, extensions; };
struct i915_gem_set_domain { uint32_t handle, read_domains, write_domain; };
struct i915_gem_busy { uint32_t handle, busy; };
struct i915_gem_set_tiling { uint32_t handle, tiling_mode, stride, swizzle_mode; };
struct i915_gem_get_tiling { uint32_t handle, tiling_mode, swizzle_mode, phys_swizzle_mode; };
struct i915_gem_get_aperture { uint64_t aper_size, aper_available_size; };
struct i915_gem_madvise { uint32_t handle, madv, retained; };
struct i915_gem_wait { uint32_t bo_handle, flags; int64_t timeout_ns; };
struct i915_ctx_create_ext { uint32_t ctx_id, flags; uint64_t extensions; };
struct i915_ctx_destroy { uint32_t ctx_id, pad; };
struct i915_ctx_param { uint32_t ctx_id, size; uint64_t param, value; };
struct i915_reg_read { uint64_t offset, val; };
struct i915_reset_stats { uint32_t ctx_id, flags, reset_count, batch_active, batch_pending, pad; };
struct i915_query { uint32_t num_items, flags; uint64_t items_ptr; };
struct i915_query_item { uint64_t query_id; int32_t length; uint32_t flags; uint64_t data_ptr; };
struct i915_vm_control { uint64_t extensions; uint32_t flags, vm_id; };
struct i915_caching { uint32_t handle, caching; };
struct i915_execbuffer2 { uint64_t buffers_ptr; uint32_t buffer_count, batch_start_offset, batch_len, DR1, DR4,
                          num_cliprects; uint64_t cliprects_ptr, flags, rsvd1, rsvd2; };
struct i915_exec_object2 { uint32_t handle, relocation_count; uint64_t relocs_ptr, alignment, offset, flags, rsvd1, rsvd2; };
struct i915_exec_fence { uint32_t handle, flags; };
struct i915_user_extension { uint64_t next_extension; uint32_t name, flags; uint32_t rsvd[4]; };
struct i915_ctx_create_ext_setparam { struct i915_user_extension base; struct i915_ctx_param param; };

#define I915_EXEC_RING_MASK     0x3f
#define I915_EXEC_BATCH_FIRST   (1u << 18)
#define I915_EXEC_FENCE_ARRAY   (1u << 19)
#define I915_EXEC_FENCE_IN      (1u << 16)
#define I915_EXEC_FENCE_OUT     (1u << 17)
#define I915_EXEC_USE_EXTENSIONS (1u << 21)
#define EXEC_OBJECT_WRITE       (1u << 2)
#define EXEC_OBJECT_PINNED      (1u << 4)
#define I915_EXEC_FENCE_WAIT    (1u << 0)
#define I915_EXEC_FENCE_SIGNAL  (1u << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1u << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1u << 1)

/* ---- state ---------------------------------------------------------------------------- */

struct binding;

struct i915_obj {
    uint32_t id;
    int refs;
    uint64_t size, npages;
    uint64_t *pages;                 /* physical pages; 0 until first used */
    uint64_t last_seqno, last_write;
    uint32_t tiling, stride, caching;
    struct binding *binds;
    struct i915_obj *next_dead;
};

typedef struct vm {
    uint32_t id;
    int refs;                        /* the client's handle and each context */
    uint64_t pml4;
    uint64_t last_seqno;
    struct binding *binds;
    struct vm *next_dead;
} vm_t;

typedef struct binding {
    vm_t *vm;
    i915_obj_t *obj;
    uint64_t va;
    struct binding *next_in_vm, *next_in_obj;
} binding_t;

typedef struct gctx {
    uint32_t id;
    vm_t *vm;
    uint64_t phys;                   /* image (CTX_PAGES) then ring, contiguous */
    int64_t ggtt_slot;
    uint32_t ggtt, ring_ggtt;
    uint32_t *regs, *ring;
    uint32_t tail;                   /* where the next request goes */
    uint32_t submitted_tail;
    int fresh, banned, guilty, closed;
    uint64_t last_seqno, switch_seqno;
    struct { uint64_t seqno; uint32_t start, end; } reqs[512];
    uint32_t req_first, req_count;
    struct gctx *next_dead;
    struct i915_client *owner;
} gctx_t;

typedef struct { int used, signaled; uint64_t seqno; } syncobj_t;

struct i915_client {
    i915_obj_t **objs;  uint32_t nobjs;
    gctx_t **ctxs;      uint32_t nctxs;
    vm_t **vms;         uint32_t nvms;
    syncobj_t *syncs;   uint32_t nsyncs;
    uint32_t resets;
};

static struct {
    int ready;
    int64_t window;                   /* first global GTT entry of ours */
    uint64_t window_bits[256];        /* 16384 pages */
    uint64_t hws_phys, seq_phys;
    uint32_t hws_gtt, seq_gtt;
    volatile uint32_t *seqno;         /* the breadcrumb */
    uint64_t next_seqno;              /* last one handed out */
    gctx_t *hw_ctx;
    uint64_t hw_last;                 /* last seqno given to the hardware */
    int ack_pending;
    uint32_t csb_read;
    uint64_t progress_seqno, progress_tick;
    uint32_t resets;
    struct { gctx_t *ctx; uint64_t seqno; uint32_t end; } pending[2048];
    uint32_t pend_first, pend_count;
    uint64_t scratch[4];              /* page, then tables of the scratch entries one level up each */
    i915_obj_t *dead_objs;
    gctx_t *dead_ctxs;
    vm_t *dead_vms;
    uint32_t next_obj_id;
    uint16_t device, revision;
} E;

int (*i915_interrupted)(void);        /* the Linux layer's "a signal is waiting" */
uint64_t (*i915_now_ns)(void);

static void zero(void *p, uint64_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint64_t n) { uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s; while (n--) *a++ = *b++; }
static uint64_t now_ns(void) { return i915_now_ns ? i915_now_ns() : tsc_us() * 1000; }
static uint64_t completed(void) {
    /* 32-bit breadcrumb, widened against the 64-bit counter */
    uint32_t lo = *E.seqno;
    uint64_t c = (E.next_seqno & ~0xFFFFFFFFULL) | lo;
    if (c > E.next_seqno) c -= 1ULL << 32;
    return c;
}

/* ---- the global GTT window ------------------------------------------------------------- */

#define WINDOW_PAGES 16384

static int64_t gtt_alloc(uint64_t pages) {
    for (uint64_t s = 0; s + pages <= WINDOW_PAGES; s++) {
        uint64_t i = 0;
        while (i < pages && !(E.window_bits[(s + i) / 64] & (1ULL << ((s + i) % 64)))) i++;
        if (i == pages) {
            for (i = 0; i < pages; i++) E.window_bits[(s + i) / 64] |= 1ULL << ((s + i) % 64);
            return (int64_t)s;
        }
        s += i;
    }
    return -1;
}

static void gtt_free(int64_t s, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) E.window_bits[(s + i) / 64] &= ~(1ULL << ((s + i) % 64));
}

static uint32_t gtt_map(int64_t s, uint64_t phys, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) igpu_ggtt_set((uint64_t)(E.window + s) + i, phys + i * 4096);
    igpu_ggtt_flush();
    return (uint32_t)((uint64_t)(E.window + s) * 4096);
}

static uint64_t page_zeroed(void) {
    uint64_t p = pmm_alloc();
    if (p) zero(PHYS_TO_VIRT(p), 4096);
    return p;
}

/* ---- buffers ------------------------------------------------------------------------- */

static i915_obj_t *obj_new(uint64_t size) {
    i915_obj_t *o;
    if (!size || size > (4ULL << 30)) return 0;
    o = (i915_obj_t *)kmalloc(sizeof(*o));
    if (!o) return 0;
    zero(o, sizeof(*o));
    o->size = (size + 4095) & ~4095ULL;
    o->npages = o->size / 4096;
    o->pages = (uint64_t *)kmalloc(o->npages * 8);
    if (!o->pages) { kfree(o); return 0; }
    zero(o->pages, o->npages * 8);
    o->refs = 1;
    o->id = ++E.next_obj_id;
    o->caching = 1;
    return o;
}

static uint64_t obj_page(i915_obj_t *o, uint64_t i) {
    if (i >= o->npages) return 0;
    if (!o->pages[i]) o->pages[i] = page_zeroed();
    return o->pages[i];
}

static int obj_fill(i915_obj_t *o) {
    for (uint64_t i = 0; i < o->npages; i++) if (!obj_page(o, i)) return -ENOMEM_;
    return 0;
}

static void unbind(binding_t *b);

static void obj_destroy(i915_obj_t *o) {
    while (o->binds) unbind(o->binds);
    for (uint64_t i = 0; i < o->npages; i++) if (o->pages[i]) pmm_free(o->pages[i]);
    kfree(o->pages);
    kfree(o);
}

void i915_obj_unref(i915_obj_t *o) {
    if (!o || --o->refs > 0) return;
    /* the GPU may still be using it: freed when its last request is done */
    o->next_dead = E.dead_objs;
    E.dead_objs = o;
}

uint64_t i915_obj_frame(i915_obj_t *o, uint64_t off) { return o ? obj_page(o, off / 4096) : 0; }
uint64_t i915_obj_size(i915_obj_t *o) { return o ? o->size : 0; }

/* ---- address spaces ---------------------------------------------------------------------- */

static int scratch_init(void) {
    for (int l = 0; l < 4; l++) {
        uint64_t *t;
        E.scratch[l] = page_zeroed();
        if (!E.scratch[l]) return -1;
        if (!l) continue;
        t = (uint64_t *)PHYS_TO_VIRT(E.scratch[l]);
        for (int i = 0; i < 512; i++)
            t[i] = E.scratch[l - 1] | PTE_PRESENT | PTE_RW | (l == 1 ? PTE_CACHED : 0);
    }
    return 0;
}

/* a table of level lvl (3 = PDP ... 1 = PT) whose entries all point at the
 * scratch one level down */
static uint64_t table_new(int lvl) {
    uint64_t p = pmm_alloc();
    uint64_t *t;
    if (!p) return 0;
    t = (uint64_t *)PHYS_TO_VIRT(p);
    for (int i = 0; i < 512; i++) t[i] = E.scratch[lvl - 1] | PTE_PRESENT | PTE_RW | (lvl == 1 ? PTE_CACHED : 0);
    return p;
}

static vm_t *vm_new(uint32_t id) {
    vm_t *v = (vm_t *)kmalloc(sizeof(*v));
    if (!v) return 0;
    zero(v, sizeof(*v));
    v->pml4 = table_new(4);
    if (!v->pml4) { kfree(v); return 0; }
    v->id = id;
    v->refs = 1;
    return v;
}

static uint64_t *pte_for(vm_t *v, uint64_t va, int make) {
    uint64_t *t = (uint64_t *)PHYS_TO_VIRT(v->pml4);
    for (int lvl = 4; lvl > 1; lvl--) {
        uint64_t idx = (va >> (12 + 9 * (lvl - 1))) & 511, next = t[idx] & ~0xFFFULL & 0x7FFFFFFFFFULL;
        if (next == E.scratch[lvl - 1]) {
            if (!make) return 0;
            next = table_new(lvl - 1);
            if (!next) return 0;
            t[idx] = next | PTE_PRESENT | PTE_RW;
        }
        t = (uint64_t *)PHYS_TO_VIRT(next);
    }
    return &t[(va >> 12) & 511];
}

static void unbind(binding_t *b) {
    binding_t **l;
    for (uint64_t i = 0; i < b->obj->npages; i++) {
        uint64_t *pte = pte_for(b->vm, b->va + i * 4096, 0);
        if (pte) *pte = E.scratch[0] | PTE_PRESENT | PTE_RW | PTE_CACHED;
    }
    for (l = &b->vm->binds; *l; l = &(*l)->next_in_vm) if (*l == b) { *l = b->next_in_vm; break; }
    for (l = &b->obj->binds; *l; l = &(*l)->next_in_obj) if (*l == b) { *l = b->next_in_obj; break; }
    kfree(b);
}

/* obj at va in v (softpin) */
static int bind(vm_t *v, i915_obj_t *o, uint64_t va) {
    binding_t *b;
    for (b = o->binds; b; b = b->next_in_obj) {
        if (b->vm != v) continue;
        if (b->va == va) return 0;
        unbind(b);
        break;
    }
    /* whatever was mapped there before goes */
    for (b = v->binds; b; ) {
        binding_t *n = b->next_in_vm;
        if (b->va < va + o->size && va < b->va + b->obj->size) unbind(b);
        b = n;
    }
    if (obj_fill(o) != 0) return -ENOMEM_;
    for (uint64_t i = 0; i < o->npages; i++) {
        uint64_t *pte = pte_for(v, va + i * 4096, 1);
        if (!pte) return -ENOMEM_;
        *pte = o->pages[i] | PTE_PRESENT | PTE_RW | PTE_CACHED;
    }
    b = (binding_t *)kmalloc(sizeof(*b));
    if (!b) return -ENOMEM_;
    b->vm = v; b->obj = o; b->va = va;
    b->next_in_vm = v->binds; v->binds = b;
    b->next_in_obj = o->binds; o->binds = b;
    return 0;
}

static void free_tables(uint64_t t, int lvl) {
    uint64_t *e = (uint64_t *)PHYS_TO_VIRT(t);
    if (lvl > 1)
        for (int i = 0; i < 512; i++) {
            uint64_t next = e[i] & ~0xFFFULL & 0x7FFFFFFFFFULL;
            if (next != E.scratch[lvl - 1]) free_tables(next, lvl - 1);
        }
    pmm_free(t);
}

static void vm_destroy(vm_t *v) {
    while (v->binds) unbind(v->binds);
    free_tables(v->pml4, 4);
    kfree(v);
}

static void vm_unref(vm_t *v) {
    if (!v || --v->refs > 0) return;
    v->next_dead = E.dead_vms;
    E.dead_vms = v;
}

/* ---- the render engine ------------------------------------------------------------------ */

/* Skylake's context workarounds (gen9_ctx_workarounds_init): masked writes,
 * loaded into every new context */
static const uint32_t ctx_was[][2] = {
    { 0x7014, MASKED_ON(1u << 13) },                                /* COMMON_SLICE_CHICKEN2: PBE hash */
    { 0xe194, MASKED_ON((1u << 8) | (1u << 4) | (1u << 2)) },       /* HALF_SLICE_CHICKEN7 */
    { 0xe4f0, MASKED_ON((1u << 15) | (1u << 8)) },                  /* ROW_CHICKEN */
    { 0x7004, MASKED_ON((1u << 6) | (1u << 1)) },                   /* CACHE_MODE_1 */
    { 0xe188, MASKED_OFF(1u << 3) },                                /* HALF_SLICE_CHICKEN5 */
    { 0x7300, MASKED_ON((1u << 5) | (1u << 15) | (1u << 4)) },      /* HDC_CHICKEN0 */
    { 0xe184, MASKED_ON(1u << 1) },                                 /* HALF_SLICE_CHICKEN3 */
    { 0xe180, MASKED_ON(1u << 13) },                                /* HALF_SLICE_CHICKEN2 */
    { 0x2580, MASKED_FIELD(0x7u, 0x4u) },                           /* CS_CHICKEN1: preemption levels */
};
#define N_CTX_WAS (sizeof(ctx_was) / sizeof(ctx_was[0]))

static uint32_t mocs_control(int i) { return i == 0 ? 0x09 : i == 2 ? 0x3B : i == 63 ? 0x37 : 0x30; }
static uint32_t mocs_l3cc(int i) { return (i == 0 || i == 63) ? 0x10 : 0x30; }

static void rmw(uint32_t reg, uint32_t clear, uint32_t set) { igpu_wr(reg, (igpu_rd(reg) & ~clear) | set); }

/* the engine's registers as the driver wants them; after a reset too */
static void engine_setup(void) {
    uint16_t hi = (uint16_t)(E.device >> 8);
    igpu_setup_ppat();
    for (int i = 0; i < 64; i++) igpu_wr(0xC800 + i * 4, mocs_control(i));
    for (int i = 0; i < 32; i++) igpu_wr(0xB020 + i * 4, mocs_l3cc(2 * i) | (mocs_l3cc(2 * i + 1) << 16));
    /* GT and render workarounds (gen9_gt_, skl_gt_, rcs_engine_wa_init) */
    rmw(0x4090, 0, (hi == 0x3E || hi == 0x9B ? 0 : (1u << 8)) | (1u << 25));  /* GAM_ECOCHK */
    rmw(0x4DDC, 0, (1u << 31) | (1u << 27));                                   /* MMCD_MISC_CTRL */
    rmw(0x940C, 0, 1u << 14);                                                  /* UCGCTL4 */
    igpu_wr(0x20E0, MASKED_ON(1u << 14));                                      /* FF_SLICE_CS_CHICKEN1 */
    rmw(0xB004, 0, 1u << 7);                                                   /* GARBCNTL: GAPS TSV credit */
    igpu_wr(0x20D4, MASKED_ON(1u << 2));                                       /* CSFE_CHICKEN1_RCS */
    rmw(0xB11C, 1u << 8, 1u << 2);                                             /* SCRATCH1 */
    rmw(0xB118, 1u << 22, 1u << 21);                                           /* L3SQCREG4 */
    rmw(0xB008, 1u << 0, 0);                                                   /* SCRATCH_LNCF1 */
    /* execlists, the status page */
    igpu_wr(RING_HWSTAM, 0xFFFFFFFF);
    igpu_wr(RING_MODE, MASKED_ON(GFX_RUN_LIST_ENABLE));
    igpu_wr(RING_MI_MODE, MASKED_OFF(STOP_RING));
    igpu_wr(RING_HWS_PGA, E.hws_gtt);
    (void)igpu_rd(RING_HWS_PGA);
    E.csb_read = igpu_rd(RING_CSB_PTR) & 7;
    E.ack_pending = 0;
}

/* context status events since last time: any one acknowledges a submission */
static void read_csb(void) {
    uint32_t w = igpu_rd(RING_CSB_PTR) & 7;
    if (w == 7 || w == E.csb_read) return;
    while (E.csb_read != w) {
        E.csb_read = E.csb_read >= 5 ? 0 : E.csb_read + 1;
        (void)igpu_rd(RING_CSB_BUF(E.csb_read));
        E.ack_pending = 0;
    }
    igpu_wr(RING_CSB_PTR, MASKED_FIELD(0x7u << 8, E.csb_read << 8));
}

static int elsp(gctx_t *c) {
    uint64_t t0 = tsc_us();
    while (E.ack_pending) {
        read_csb();
        if (tsc_us() - t0 > 20000) {
            /* no status event for the last submission: noted, not fatal - a
             * real hang is caught by the progress check */
            static int told;
            if (!told++) igpu_report("the render engine did not acknowledge a submission (noted once)");
            E.ack_pending = 0;
        }
    }
    c->regs[CTX_RING_TAIL] = c->submitted_tail;
    igpu_flush(&c->regs[CTX_RING_TAIL], 4);
    E.ack_pending = 1;
    igpu_wr(RING_ELSP, 0);
    igpu_wr(RING_ELSP, 0);
    igpu_wr(RING_ELSP, c->id);
    igpu_wr(RING_ELSP, c->ggtt | DESC_LEGACY_64B | DESC_PRIVILEGE | DESC_VALID);
    return 0;
}

static void reset_engine(const char *why);

/* the next requests in sequence to the hardware: more of the running
 * context's, or - once it is done - the next context's */
static void gpu_schedule(void) {
    uint64_t done = completed();
    int was_idle = !(E.hw_ctx && E.hw_last > done);
    if (!E.pend_count) return;
    gctx_t *next = E.pending[E.pend_first].ctx;
    if (E.hw_ctx && E.hw_last > done && next != E.hw_ctx) return;   /* still busy with another */
    if (next != E.hw_ctx && E.hw_ctx) E.hw_ctx->switch_seqno = E.pending[E.pend_first].seqno;
    while (E.pend_count && E.pending[E.pend_first].ctx == next) {
        next->submitted_tail = E.pending[E.pend_first].end;
        E.hw_last = E.pending[E.pend_first].seqno;
        E.pend_first = (E.pend_first + 1) % 2048;
        E.pend_count--;
    }
    E.hw_ctx = next;
    if (was_idle) {                               /* the clock for "no progress" starts now */
        E.progress_seqno = done;
        E.progress_tick = sched_ticks();
    }
    if (elsp(next) != 0) reset_engine("the render engine stopped taking work");
}

static void free_dead(void);

/* moves everything on: called by the driver's thread and by every waiter */
static void engine_poll(void) {
    uint64_t done;
    if (!E.ready) return;
    read_csb();
    done = completed();
    if (done != E.progress_seqno) {
        E.progress_seqno = done;
        E.progress_tick = sched_ticks();
    } else if (E.hw_last > done && sched_ticks() - E.progress_tick > 300) {
        reset_engine("a GPU program ran for 3 s without finishing");
        done = completed();
    }
    /* requests dropped by a reset end nowhere: once all else is done they
     * count as done too, so nobody waits for them for ever */
    if (!E.pend_count && E.hw_last <= done && done < E.next_seqno) {
        *E.seqno = (uint32_t)E.next_seqno;
        igpu_flush((const void *)E.seqno, 4);
        E.hw_last = E.next_seqno;
    }
    gpu_schedule();
    free_dead();
}

static void reset_engine(const char *why) {
    igpu_report(why);
    if (igpu_reset_render() != 0) igpu_report("the render engine reset failed");
    engine_setup();
    if (E.hw_ctx) {
        E.hw_ctx->banned = 1;
        E.hw_ctx->guilty++;
        if (E.hw_ctx->owner) E.hw_ctx->owner->resets++;
        E.hw_ctx->fresh = 1;              /* its image is lost: start it afresh */
    }
    /* and what a banned context still had queued is dropped */
    {
        uint32_t k = 0;
        for (uint32_t i = 0; i < E.pend_count; i++) {
            uint32_t from = (E.pend_first + i) % 2048, to = (E.pend_first + k) % 2048;
            if (E.pending[from].ctx->banned) continue;
            E.pending[to] = E.pending[from];
            k++;
        }
        E.pend_count = k;
    }
    /* what was in flight counts as done; waiters see an error through the ban */
    *E.seqno = (uint32_t)E.hw_last;
    igpu_flush((const void *)E.seqno, 4);
    E.hw_ctx = 0;
    E.resets++;
    E.progress_tick = sched_ticks();
}

/* waits until seqno is done; timeout in ns, -1 for ever */
static int wait_seqno(uint64_t seqno, int64_t timeout_ns) {
    uint64_t start = now_ns();
    for (;;) {
        engine_poll();
        if (completed() >= seqno) return 0;
        if (timeout_ns == 0) return -ETIME_;
        if (timeout_ns > 0 && (int64_t)(now_ns() - start) >= timeout_ns) return -ETIME_;
        if (i915_interrupted && i915_interrupted()) return -EINTR_;
        sched_sleep(1);
    }
}

int i915_obj_wait(i915_obj_t *o, int64_t timeout_ns) { return o ? wait_seqno(o->last_seqno, timeout_ns) : 0; }

static void i915_thread(void) {
    for (;;) {
        engine_poll();
        sched_sleep(1);
    }
}

/* ---- contexts ------------------------------------------------------------------------- */

static gctx_t *ctx_new(vm_t *vm, uint32_t id) {
    const uint64_t pages = CTX_PAGES + RING_PAGES;
    gctx_t *c = (gctx_t *)kmalloc(sizeof(*c));
    if (!c) return 0;
    zero(c, sizeof(*c));
    c->phys = pmm_alloc_contiguous(pages);
    c->ggtt_slot = c->phys ? gtt_alloc(pages) : -1;
    if (c->ggtt_slot < 0) {
        if (c->phys) pmm_free_range(c->phys, pages);
        kfree(c);
        return 0;
    }
    zero(PHYS_TO_VIRT(c->phys), pages * 4096);
    c->ggtt = gtt_map(c->ggtt_slot, c->phys, pages);
    c->ring_ggtt = c->ggtt + CTX_PAGES * 4096;
    c->regs = (uint32_t *)((uint8_t *)PHYS_TO_VIRT(c->phys) + 4096);
    c->ring = (uint32_t *)((uint8_t *)PHYS_TO_VIRT(c->phys) + CTX_PAGES * 4096);
    c->vm = vm;
    vm->refs++;
    c->id = id;
    c->fresh = 1;
    return c;
}

/* a fresh image: nothing of an earlier run restored, the ring empty */
static void ctx_image(gctx_t *c) {
    zero(c->regs, 4096);
    igpu_context_regs(c->regs);
    c->regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_CTRL_INHIBIT_SYN | CTX_CTRL_RESTORE_INHIBIT) |
                                   MASKED_OFF(CTX_CTRL_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
    c->regs[CTX_RING_HEAD] = c->tail;
    c->regs[CTX_RING_TAIL] = c->tail;
    c->regs[CTX_RING_START] = c->ring_ggtt;
    c->regs[CTX_RING_CTL] = ((RING_BYTES - 4096) & 0x1FF000) | 1;
    c->regs[CTX_MI_MODE + 1] = MASKED_OFF(STOP_RING);
    c->regs[CTX_PDP0_UDW] = (uint32_t)(c->vm->pml4 >> 32);
    c->regs[CTX_PDP0_LDW] = (uint32_t)c->vm->pml4;
    igpu_flush(c->regs, 4096);
}

static void ctx_close(gctx_t *c) {
    c->closed = 1;
    c->next_dead = E.dead_ctxs;
    E.dead_ctxs = c;
}

/* contexts, buffers and address spaces no longer used, once the GPU is
 * done with them - a context only after the engine has moved to another
 * one, since leaving it writes its image */
static void free_dead(void) {
    uint64_t done = completed();
    gctx_t **cp = &E.dead_ctxs;
    i915_obj_t **op = &E.dead_objs;
    vm_t **vp = &E.dead_vms;
    while (*cp) {
        gctx_t *c = *cp;
        int pending = 0;
        for (uint32_t i = 0; i < E.pend_count && !pending; i++) if (E.pending[(E.pend_first + i) % 2048].ctx == c) pending = 1;
        if (!pending && c != E.hw_ctx && c->last_seqno <= done && (!c->switch_seqno || c->switch_seqno <= done)) {
            *cp = c->next_dead;
            gtt_free(c->ggtt_slot, CTX_PAGES + RING_PAGES);
            pmm_free_range(c->phys, CTX_PAGES + RING_PAGES);
            vm_unref(c->vm);
            kfree(c);
        } else {
            cp = &c->next_dead;
        }
    }
    while (*op) {
        i915_obj_t *o = *op;
        if (o->last_seqno <= done) { *op = o->next_dead; obj_destroy(o); }
        else op = &o->next_dead;
    }
    while (*vp) {
        vm_t *v = *vp;
        if (v->last_seqno <= done) { *vp = v->next_dead; vm_destroy(v); }
        else vp = &v->next_dead;
    }
}

/* ---- requests --------------------------------------------------------------------------- */

static void ring_retire(gctx_t *c) {
    uint64_t done = completed();
    while (c->req_count && c->reqs[c->req_first].seqno <= done) {
        c->req_first = (c->req_first + 1) % 512;
        c->req_count--;
    }
}

static uint32_t ring_space(gctx_t *c) {
    uint32_t head;
    ring_retire(c);
    if (!c->req_count) return RING_BYTES - 8;
    head = c->reqs[c->req_first].start;
    return (head + RING_BYTES - c->tail - 8) % RING_BYTES;
}

/* waits (moving the engine on) until cond holds or the wait fails */
#define WAIT_FOR(cond) do {                                             \
        uint64_t t0_ = now_ns();                                        \
        while (!(cond)) {                                               \
            engine_poll();                                              \
            if (cond) break;                                            \
            if (i915_interrupted && i915_interrupted()) return -EINTR_; \
            if (now_ns() - t0_ > 10000000000ULL) return -EIO_;          \
            sched_sleep(1);                                             \
        }                                                               \
    } while (0)

/* the commands of one request, around the batch at batch_va */
static int build_request(gctx_t *c, uint64_t batch_va, uint64_t seqno, uint32_t *cmd) {
    int n = 0;
    if (c->fresh) {
        /* a new context: the workarounds and the cache table, once */
        cmd[n++] = MI_LOAD_REGISTER_IMM(N_CTX_WAS);
        for (uint32_t i = 0; i < N_CTX_WAS; i++) { cmd[n++] = ctx_was[i][0]; cmd[n++] = ctx_was[i][1]; }
        cmd[n++] = MI_LOAD_REGISTER_IMM(64);
        for (int i = 0; i < 64; i++) { cmd[n++] = 0xC800 + i * 4; cmd[n++] = mocs_control(i); }
        cmd[n++] = MI_LOAD_REGISTER_IMM(32);
        for (int i = 0; i < 32; i++) { cmd[n++] = 0xB020 + i * 4; cmd[n++] = mocs_l3cc(2 * i) | (mocs_l3cc(2 * i + 1) << 16); }
        if (n & 1) cmd[n++] = MI_NOOP;
    }
    /* a null pipe control before invalidating the vertex cache (gen9), then
     * every read cache and the address translations invalidated */
    cmd[n++] = GFX_OP_PIPE_CONTROL(6); cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = GFX_OP_PIPE_CONTROL(6);
    cmd[n++] = PC_CS_STALL | PC_TLB_INVALIDATE | PC_INSTR_INVALIDATE | PC_TEX_INVALIDATE | PC_VF_INVALIDATE |
               PC_CONST_INVALIDATE | PC_STATE_INVALIDATE | PC_QW_WRITE | PC_STORE_DATA_INDEX;
    cmd[n++] = PPHWSP_SCRATCH; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    /* the batch, unprivileged, in the context's address space */
    cmd[n++] = MI_ARB_ON_OFF | 1;
    cmd[n++] = MI_BATCH_BUFFER_START_GEN8 | (1u << 8);
    cmd[n++] = (uint32_t)batch_va;
    cmd[n++] = (uint32_t)(batch_va >> 32);
    cmd[n++] = MI_ARB_ON_OFF;
    cmd[n++] = MI_NOOP;
    /* everything written out, then the breadcrumb */
    cmd[n++] = GFX_OP_PIPE_CONTROL(6);
    cmd[n++] = PC_CS_STALL | PC_TLB_INVALIDATE | PC_RT_FLUSH | PC_DEPTH_FLUSH | PC_DC_FLUSH;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = GFX_OP_PIPE_CONTROL(6);
    cmd[n++] = PC_QW_WRITE | PC_GLOBAL_GTT | PC_FLUSH_ENABLE | PC_CS_STALL;
    cmd[n++] = E.seq_gtt; cmd[n++] = 0; cmd[n++] = (uint32_t)seqno; cmd[n++] = 0;
    cmd[n++] = MI_USER_INTERRUPT;
    cmd[n++] = MI_ARB_ON_OFF | 1;
    cmd[n++] = MI_ARB_CHECK;                 /* WaIdleLiteRestore: never tail == head */
    cmd[n++] = MI_NOOP;
    return n;
}

static int64_t submit(gctx_t *c, uint64_t batch_va, uint64_t *seqno_out) {
    uint32_t cmd[320], bytes, fill, start;
    uint64_t seqno;
    int n;
    if (c->fresh) {
        /* nothing of it can be in flight: fresh contexts never ran, banned
         * ones are refused before this */
        c->tail = 0;
        c->req_count = 0;
        ctx_image(c);
    }
    WAIT_FOR(E.pend_count < 2048 && c->req_count < 512);
    seqno = E.next_seqno + 1;
    n = build_request(c, batch_va, seqno, cmd);
    bytes = (uint32_t)n * 4;
    fill = c->tail + bytes > RING_BYTES ? RING_BYTES - c->tail : 0;
    WAIT_FOR(ring_space(c) >= bytes + fill);
    start = c->tail;
    if (fill) {
        zero((uint8_t *)c->ring + c->tail, fill);         /* MI_NOOP to the end, then from the start */
        igpu_flush((uint8_t *)c->ring + c->tail, fill);
        c->tail = 0;
    }
    copy((uint8_t *)c->ring + c->tail, cmd, bytes);
    igpu_flush((uint8_t *)c->ring + c->tail, bytes);
    c->tail = (c->tail + bytes) % RING_BYTES;
    c->fresh = 0;
    E.next_seqno = seqno;
    c->last_seqno = seqno;
    c->vm->last_seqno = seqno;
    c->reqs[(c->req_first + c->req_count) % 512].seqno = seqno;
    c->reqs[(c->req_first + c->req_count) % 512].start = start;
    c->reqs[(c->req_first + c->req_count) % 512].end = c->tail;
    c->req_count++;
    E.pending[(E.pend_first + E.pend_count) % 2048].ctx = c;
    E.pending[(E.pend_first + E.pend_count) % 2048].seqno = seqno;
    E.pending[(E.pend_first + E.pend_count) % 2048].end = c->tail;
    E.pend_count++;
    *seqno_out = seqno;
    gpu_schedule();
    return 0;
}

/* ---- clients: handles, contexts, address spaces, syncobjs ------------------------------ */

static int grow(void **arr, uint32_t *n, uint32_t size) {
    uint32_t nn = *n ? *n * 2 : 16;
    void *a = kmalloc((uint64_t)nn * size);
    if (!a) return -1;
    zero(a, (uint64_t)nn * size);
    if (*arr) { copy(a, *arr, (uint64_t)*n * size); kfree(*arr); }
    *arr = a;
    *n = nn;
    return 0;
}

static i915_obj_t *obj_lookup(i915_client_t *c, uint32_t h) { return h && h <= c->nobjs ? c->objs[h - 1] : 0; }

static int handle_add(i915_client_t *c, i915_obj_t *o, uint32_t *h) {
    for (uint32_t i = 0;; i++) {
        if (i == c->nobjs && grow((void **)&c->objs, &c->nobjs, sizeof(i915_obj_t *)) != 0) return -ENOMEM_;
        if (!c->objs[i]) { c->objs[i] = o; *h = i + 1; return 0; }
    }
}

static vm_t *vm_lookup(i915_client_t *c, uint32_t id) { return id && id <= c->nvms ? c->vms[id - 1] : 0; }

static gctx_t *ctx_lookup(i915_client_t *c, uint32_t id) {
    if (id >= c->nctxs) return 0;
    if (!c->ctxs[id] && id == 0) c->ctxs[0] = ctx_new(c->vms[0], 0);    /* the default context */
    if (c->ctxs[id]) c->ctxs[id]->owner = c;
    return c->ctxs[id];
}

static syncobj_t *sync_lookup(i915_client_t *c, uint32_t h) { return h && h <= c->nsyncs && c->syncs[h - 1].used ? &c->syncs[h - 1] : 0; }
static int sync_done(syncobj_t *s) { return s->signaled || (s->seqno && completed() >= s->seqno); }

i915_client_t *i915_open(void) {
    i915_client_t *c;
    if (!E.ready) return 0;
    c = (i915_client_t *)kmalloc(sizeof(*c));
    if (!c) return 0;
    zero(c, sizeof(*c));
    if (grow((void **)&c->vms, &c->nvms, sizeof(vm_t *)) != 0 ||
        grow((void **)&c->ctxs, &c->nctxs, sizeof(gctx_t *)) != 0 ||
        !(c->vms[0] = vm_new(1))) {
        if (c->vms) kfree(c->vms);
        if (c->ctxs) kfree(c->ctxs);
        kfree(c);
        return 0;
    }
    return c;
}

void i915_close(i915_client_t *c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->nobjs; i++) if (c->objs[i]) i915_obj_unref(c->objs[i]);
    for (uint32_t i = 0; i < c->nctxs; i++) if (c->ctxs[i]) { c->ctxs[i]->owner = 0; ctx_close(c->ctxs[i]); }
    for (uint32_t i = 0; i < c->nvms; i++) if (c->vms[i]) vm_unref(c->vms[i]);
    if (c->objs) kfree(c->objs);
    if (c->ctxs) kfree(c->ctxs);
    if (c->vms) kfree(c->vms);
    if (c->syncs) kfree(c->syncs);
    kfree(c);
}

i915_obj_t *i915_obj_from_handle(i915_client_t *c, uint32_t handle) {
    i915_obj_t *o = obj_lookup(c, handle);
    if (o) o->refs++;
    return o;
}

int i915_handle_for_obj(i915_client_t *c, i915_obj_t *o, uint32_t *handle) {
    for (uint32_t i = 0; i < c->nobjs; i++) if (c->objs[i] == o) { *handle = i + 1; return 0; }
    o->refs++;
    if (handle_add(c, o, handle) != 0) { o->refs--; return -ENOMEM_; }
    return 0;
}

/* the frame behind a GEM mmap offset: the object's id in the upper half */
uint64_t i915_client_frame(i915_client_t *c, uint64_t off) {
    uint32_t id = (uint32_t)(off >> 32);
    for (uint32_t i = 0; i < c->nobjs; i++)
        if (c->objs[i] && c->objs[i]->id == id) return obj_page(c->objs[i], (off & 0xFFFFFFFFULL) / 4096);
    return 0;
}

/* ---- the ioctls -------------------------------------------------------------------------- */

static int64_t put_user_str(uint64_t uptr, uint64_t *len, const char *s) {
    uint64_t n = 0, cap = *len;
    while (s[n]) n++;
    *len = n;
    if (uptr && cap && copy_to_user((void *)uptr, s, n < cap ? n : cap)) return -EFAULT_;
    return 0;
}

static int64_t getparam(struct i915_getparam *g) {
    int32_t v;
    switch (g->param) {
    case 4:  v = E.device; break;                    /* CHIPSET_ID */
    case 32: v = E.revision; break;                  /* REVISION */
    case 5: case 9: case 17: case 19: case 21: case 24: case 25: case 26: case 37:
    case 43: case 45: case 48: case 49: case 50:
        v = 1; break;                                /* GEM, EXECBUF2, LLC, WAIT_TIMEOUT, PRIME_VMAP_FLUSH,
                                                        PINNED, NO_RELOC, HANDLE_LUT, SOFTPIN, EXEC_ASYNC,
                                                        CAPTURE, BATCH_FIRST, FENCE_ARRAY, CONTEXT_ISOLATION */
    case 18: v = 3; break;                           /* full 48-bit PPGTT */
    case 30: v = 1; break;                           /* MMAP_VERSION */
    case 40: v = 4; break;                           /* MMAP_GTT_VERSION: mmap_offset */
    case 51: v = 12000000; break;                    /* CS_TIMESTAMP_FREQUENCY: 12 MHz on gen9 */
    case 10: case 11: case 22: case 20: case 41: case 53: case 55: case 56:
        v = 0; break;                                /* no BSD/BLT/VEBOX, semaphores, scheduler,
                                                        submit/timeline fences, userptr probe */
    case 58: return -ENODEV_;                        /* PXP_STATUS */
    default: return -EINVAL_;                        /* topology: Mesa uses its own table */
    }
    return copy_to_user((void *)g->value, &v, 4) ? -EFAULT_ : 0;
}

static int64_t ctx_setparam(i915_client_t *cl, gctx_t *c, struct i915_ctx_param *p) {
    switch (p->param) {
    case 0x9: {                                      /* VM */
        vm_t *v = vm_lookup(cl, (uint32_t)p->value);
        if (!v) return -ENOENT_;
        if (c->req_count || !c->fresh) return -EBUSY_;
        vm_unref(c->vm);
        c->vm = v;
        v->refs++;
        return 0;
    }
    case 0x1: case 0x2: case 0x4: case 0x5: case 0x6: case 0x8: case 0xb:
        return 0;                                    /* ban period, no zeromap, no capture, bannable,
                                                        priority, recoverable, persistence */
    case 0xd: return p->value ? -ENODEV_ : 0;        /* protected content */
    default: return -EINVAL_;
    }
}

static int64_t ctx_create(i915_client_t *c, struct i915_ctx_create_ext *a) {
    gctx_t *ctx;
    uint32_t id;
    if (a->flags & ~3u) return -EINVAL_;
    for (id = 1;; id++) {
        if (id >= c->nctxs && grow((void **)&c->ctxs, &c->nctxs, sizeof(gctx_t *)) != 0) return -ENOMEM_;
        if (!c->ctxs[id]) break;
    }
    ctx = ctx_new(c->vms[0], id);
    if (!ctx) return -ENOMEM_;
    ctx->owner = c;
    if (a->flags & 1) {                              /* USE_EXTENSIONS */
        uint64_t ext = a->extensions;
        for (int k = 0; ext && k < 16; k++) {
            struct i915_ctx_create_ext_setparam sp;
            int64_t r;
            if (copy_from_user(&sp, (const void *)ext, sizeof(sp))) { ctx_close(ctx); return -EFAULT_; }
            if (sp.base.name != 0) { ctx_close(ctx); return -EINVAL_; }      /* only SETPARAM */
            r = ctx_setparam(c, ctx, &sp.param);
            if (r) { ctx_close(ctx); return r; }
            ext = sp.base.next_extension;
        }
    }
    c->ctxs[id] = ctx;
    a->ctx_id = id;
    return 0;
}

static int64_t execbuf(i915_client_t *c, struct i915_execbuffer2 *eb) {
    struct i915_exec_object2 *objs;
    gctx_t *ctx;
    i915_obj_t *batch;
    uint64_t batch_va, seqno = 0;
    uint32_t n = eb->buffer_count, ring = (uint32_t)(eb->flags & I915_EXEC_RING_MASK), bi;
    int64_t r = 0;
    if (!n || n > 16384) return -EINVAL_;
    if (ring > 1 && ring != 3) return -EINVAL_;      /* render (and the blitter, done by render) */
    if (eb->flags & (I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT | I915_EXEC_USE_EXTENSIONS)) return -EINVAL_;
    ctx = ctx_lookup(c, (uint32_t)eb->rsvd1);
    if (!ctx) return -ENOENT_;
    if (ctx->banned) return -EIO_;
    objs = (struct i915_exec_object2 *)kmalloc((uint64_t)n * sizeof(*objs));
    if (!objs) return -ENOMEM_;
    if (copy_from_user(objs, (const void *)eb->buffers_ptr, (uint64_t)n * sizeof(*objs))) { kfree(objs); return -EFAULT_; }
    /* the fences it waits for must exist: the order is the submission order */
    if (eb->flags & I915_EXEC_FENCE_ARRAY) {
        for (uint32_t i = 0; i < eb->num_cliprects && !r; i++) {
            struct i915_exec_fence f;
            syncobj_t *s;
            if (copy_from_user(&f, (const void *)(eb->cliprects_ptr + i * sizeof(f)), sizeof(f))) r = -EFAULT_;
            else if (!(s = sync_lookup(c, f.handle))) r = -ENOENT_;
            else if ((f.flags & I915_EXEC_FENCE_WAIT) && !s->signaled && !s->seqno) r = -EINVAL_;
        }
    }
    for (uint32_t i = 0; i < n && !r; i++) {
        i915_obj_t *o = obj_lookup(c, objs[i].handle);
        if (!o) r = -ENOENT_;
        else if (objs[i].relocation_count || !(objs[i].flags & EXEC_OBJECT_PINNED)) r = -EINVAL_;
        else r = bind(ctx->vm, o, objs[i].offset & VA_MASK);
    }
    if (r) { kfree(objs); return r; }
    bi = (eb->flags & I915_EXEC_BATCH_FIRST) ? 0 : n - 1;
    batch = obj_lookup(c, objs[bi].handle);
    if (eb->batch_start_offset >= batch->size) { kfree(objs); return -EINVAL_; }
    batch_va = (objs[bi].offset & VA_MASK) + eb->batch_start_offset;
    r = submit(ctx, batch_va, &seqno);
    if (!r) {
        for (uint32_t i = 0; i < n; i++) {
            i915_obj_t *o = obj_lookup(c, objs[i].handle);
            o->last_seqno = seqno;
            if (objs[i].flags & EXEC_OBJECT_WRITE) o->last_write = seqno;
        }
        if (eb->flags & I915_EXEC_FENCE_ARRAY)
            for (uint32_t i = 0; i < eb->num_cliprects; i++) {
                struct i915_exec_fence f;
                syncobj_t *s;
                if (copy_from_user(&f, (const void *)(eb->cliprects_ptr + i * sizeof(f)), sizeof(f))) continue;
                if ((f.flags & I915_EXEC_FENCE_SIGNAL) && (s = sync_lookup(c, f.handle))) { s->seqno = seqno; s->signaled = 0; }
            }
    }
    kfree(objs);
    return r;
}

static int64_t syncobj_wait(i915_client_t *c, struct drm_syncobj_wait *w) {
    uint32_t *h;
    int64_t r = 0;
    if (!w->count_handles || w->count_handles > 4096) return -EINVAL_;
    h = (uint32_t *)kmalloc(w->count_handles * 4ULL);
    if (!h) return -ENOMEM_;
    if (copy_from_user(h, (const void *)w->handles, w->count_handles * 4ULL)) { kfree(h); return -EFAULT_; }
    for (;;) {
        uint32_t ready = 0, first = ~0u;
        engine_poll();
        for (uint32_t i = 0; i < w->count_handles; i++) {
            syncobj_t *s = sync_lookup(c, h[i]);
            if (!s) { r = -ENOENT_; break; }
            if (!s->signaled && !s->seqno && !(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) { r = -EINVAL_; break; }
            if (sync_done(s)) { ready++; if (first == ~0u) first = i; }
        }
        if (r) break;
        if ((w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? ready == w->count_handles : ready > 0) {
            w->first_signaled = first;
            break;
        }
        if ((int64_t)now_ns() >= w->timeout_nsec) { r = -ETIME_; break; }   /* the timeout is a point in time */
        if (i915_interrupted && i915_interrupted()) { r = -EINTR_; break; }
        sched_sleep(1);
    }
    kfree(h);
    return r;
}

static int64_t syncobj_array(i915_client_t *c, struct drm_syncobj_array *a, int signal) {
    for (uint32_t i = 0; i < a->count_handles; i++) {
        uint32_t hd;
        syncobj_t *s;
        if (copy_from_user(&hd, (const void *)(a->handles + i * 4ULL), 4)) return -EFAULT_;
        if (!(s = sync_lookup(c, hd))) return -ENOENT_;
        s->signaled = signal;
        s->seqno = 0;
    }
    return 0;
}

int64_t i915_ioctl(i915_client_t *c, uint32_t nr, void *karg) {
    if (!c) return -ENODEV_;
    switch (nr) {
    case 0x00: {                                     /* VERSION */
        struct drm_version *v = (struct drm_version *)karg;
        v->major = 1; v->minor = 6; v->patch = 0;
        if (put_user_str(v->name, &v->name_len, "i915") || put_user_str(v->date, &v->date_len, "20230929") ||
            put_user_str(v->desc, &v->desc_len, "Intel Graphics")) return -EFAULT_;
        return 0;
    }
    case 0x09: {                                     /* GEM_CLOSE */
        struct drm_gem_close *a = (struct drm_gem_close *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        c->objs[a->handle - 1] = 0;
        i915_obj_unref(o);
        return 0;
    }
    case 0x0a: case 0x0b: return -ENODEV_;           /* FLINK, OPEN: not on a render node */
    case 0x0c: {                                     /* GET_CAP */
        struct drm_get_cap *g = (struct drm_get_cap *)karg;
        switch (g->capability) {
        case 0x5:  g->value = 3; return 0;           /* PRIME import and export */
        case 0x6:  g->value = 1; return 0;           /* TIMESTAMP_MONOTONIC */
        case 0x13: g->value = 1; return 0;           /* SYNCOBJ */
        case 0x14: g->value = 0; return 0;           /* SYNCOBJ_TIMELINE */
        default: return -EINVAL_;
        }
    }
    case 0x0d: return -EINVAL_;                      /* SET_CLIENT_CAP */
    case 0xBF: {                                     /* SYNCOBJ_CREATE */
        struct drm_syncobj_create *a = (struct drm_syncobj_create *)karg;
        for (uint32_t i = 0;; i++) {
            if (i == c->nsyncs && grow((void **)&c->syncs, &c->nsyncs, sizeof(syncobj_t)) != 0) return -ENOMEM_;
            if (!c->syncs[i].used) {
                c->syncs[i].used = 1;
                c->syncs[i].signaled = (a->flags & 1) != 0;
                c->syncs[i].seqno = 0;
                a->handle = i + 1;
                return 0;
            }
        }
    }
    case 0xC0: {                                     /* SYNCOBJ_DESTROY */
        syncobj_t *s = sync_lookup(c, ((struct drm_syncobj_destroy *)karg)->handle);
        if (!s) return -ENOENT_;
        s->used = 0;
        return 0;
    }
    case 0xC3: return syncobj_wait(c, (struct drm_syncobj_wait *)karg);
    case 0xC4: return syncobj_array(c, (struct drm_syncobj_array *)karg, 0);      /* RESET */
    case 0xC5: return syncobj_array(c, (struct drm_syncobj_array *)karg, 1);      /* SIGNAL */
    case 0xC1: case 0xC2: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCF:
        return -EINVAL_;                             /* syncobj files and timelines */
    case 0x46: return getparam((struct i915_getparam *)karg);
    case 0x57: {                                     /* GEM_BUSY */
        struct i915_gem_busy *a = (struct i915_gem_busy *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        engine_poll();
        a->busy = o->last_seqno > completed() ? (o->last_write > completed() ? 0x10001u : 0x10000u) : 0;
        return 0;
    }
    case 0x58: return 0;                             /* THROTTLE */
    case 0x5b: case 0x7c: {                          /* GEM_CREATE, GEM_CREATE_EXT */
        struct i915_gem_create_ext *a = (struct i915_gem_create_ext *)karg;
        i915_obj_t *o;
        uint32_t h;
        if (nr == 0x7c) {
            uint64_t ext = a->extensions;
            for (int k = 0; ext && k < 8; k++) {
                struct i915_user_extension ue;
                if (copy_from_user(&ue, (const void *)ext, sizeof(ue))) return -EFAULT_;
                if (ue.name == 1) return -ENODEV_;           /* protected content */
                if (ue.name != 0) return -EINVAL_;           /* memory regions: all system memory here */
                ext = ue.next_extension;
            }
        }
        o = obj_new(a->size);
        if (!o) return -ENOMEM_;
        if (handle_add(c, o, &h) != 0) { obj_destroy(o); return -ENOMEM_; }
        a->size = o->size;
        a->handle = h;
        return 0;
    }
    case 0x5e: return -ENODEV_;                      /* GEM_MMAP: mmap_offset instead */
    case 0x5f: {                                     /* SET_DOMAIN: the CPU wants it, the GPU done */
        i915_obj_t *o = obj_lookup(c, ((struct i915_gem_set_domain *)karg)->handle);
        if (!o) return -ENOENT_;
        return wait_seqno(o->last_seqno, -1);
    }
    case 0x60: return 0;                             /* SW_FINISH */
    case 0x61: {                                     /* SET_TILING */
        struct i915_gem_set_tiling *a = (struct i915_gem_set_tiling *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        if (a->tiling_mode > 2) return -EINVAL_;
        o->tiling = a->tiling_mode;
        o->stride = a->stride;
        a->swizzle_mode = 0;
        return 0;
    }
    case 0x62: {                                     /* GET_TILING */
        struct i915_gem_get_tiling *a = (struct i915_gem_get_tiling *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        a->tiling_mode = o->tiling;
        a->swizzle_mode = 0;
        a->phys_swizzle_mode = 0;
        return 0;
    }
    case 0x63: {                                     /* GET_APERTURE */
        struct i915_gem_get_aperture *a = (struct i915_gem_get_aperture *)karg;
        a->aper_size = 4ULL << 30;
        a->aper_available_size = 4ULL << 30;
        return 0;
    }
    case 0x64: {                                     /* MMAP_OFFSET */
        struct i915_gem_mmap_offset *a = (struct i915_gem_mmap_offset *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        if (a->flags > 4 || a->extensions) return -EINVAL_;
        a->offset = (uint64_t)o->id << 32;
        return 0;
    }
    case 0x66: {                                     /* MADVISE */
        struct i915_gem_madvise *a = (struct i915_gem_madvise *)karg;
        if (!obj_lookup(c, a->handle)) return -ENOENT_;
        a->retained = 1;
        return 0;
    }
    case 0x69: return execbuf(c, (struct i915_execbuffer2 *)karg);
    case 0x6c: {                                     /* GEM_WAIT */
        struct i915_gem_wait *a = (struct i915_gem_wait *)karg;
        i915_obj_t *o = obj_lookup(c, a->bo_handle);
        uint64_t t0 = now_ns();
        int64_t r;
        if (!o) return -ENOENT_;
        r = wait_seqno(o->last_seqno, a->timeout_ns < 0 ? -1 : a->timeout_ns);
        if (a->timeout_ns > 0) {
            int64_t left = a->timeout_ns - (int64_t)(now_ns() - t0);
            a->timeout_ns = left > 0 ? left : 0;
        }
        return r;
    }
    case 0x6d: return ctx_create(c, (struct i915_ctx_create_ext *)karg);
    case 0x6e: {                                     /* CONTEXT_DESTROY */
        uint32_t id = ((struct i915_ctx_destroy *)karg)->ctx_id;
        if (!id || id >= c->nctxs || !c->ctxs[id]) return -ENOENT_;
        ctx_close(c->ctxs[id]);
        c->ctxs[id] = 0;
        return 0;
    }
    case 0x6f: {                                     /* SET_CACHING */
        struct i915_caching *a = (struct i915_caching *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        o->caching = a->caching;
        return 0;
    }
    case 0x70: {                                     /* GET_CACHING */
        struct i915_caching *a = (struct i915_caching *)karg;
        i915_obj_t *o = obj_lookup(c, a->handle);
        if (!o) return -ENOENT_;
        a->caching = o->caching;
        return 0;
    }
    case 0x71: {                                     /* REG_READ: the render engine's timestamp */
        struct i915_reg_read *a = (struct i915_reg_read *)karg;
        uint32_t lo, hi;
        if ((a->offset & ~1ULL) != RING_TIMESTAMP) return -EINVAL_;
        do { hi = igpu_rd(RING_TIMESTAMP + 4); lo = igpu_rd(RING_TIMESTAMP); } while (hi != igpu_rd(RING_TIMESTAMP + 4));
        a->val = ((uint64_t)hi << 32) | lo;
        return 0;
    }
    case 0x72: {                                     /* GET_RESET_STATS */
        struct i915_reset_stats *a = (struct i915_reset_stats *)karg;
        gctx_t *ctx = ctx_lookup(c, a->ctx_id);
        if (!ctx) return -ENOENT_;
        a->reset_count = E.resets;
        a->batch_active = ctx->guilty;
        a->batch_pending = 0;
        return 0;
    }
    case 0x73: return -ENODEV_;                      /* USERPTR */
    case 0x74: {                                     /* CONTEXT_GETPARAM */
        struct i915_ctx_param *p = (struct i915_ctx_param *)karg;
        gctx_t *ctx = p->ctx_id ? ctx_lookup(c, p->ctx_id) : 0;
        if (p->ctx_id && !ctx) return -ENOENT_;
        p->size = 0;
        switch (p->param) {
        case 0x3: p->value = 1ULL << 48; return 0;                     /* GTT_SIZE */
        case 0x9: p->value = ctx ? ctx->vm->id : 1; return 0;          /* VM */
        case 0x5: case 0x8: case 0xb: p->value = 1; return 0;          /* bannable, recoverable, persistence */
        case 0x6: case 0x4: case 0xd: p->value = 0; return 0;          /* priority, no capture, protected */
        default: return -EINVAL_;
        }
    }
    case 0x75: {                                     /* CONTEXT_SETPARAM */
        struct i915_ctx_param *p = (struct i915_ctx_param *)karg;
        gctx_t *ctx = ctx_lookup(c, p->ctx_id);
        if (!ctx) return -ENOENT_;
        return ctx_setparam(c, ctx, p);
    }
    case 0x79: {                                     /* QUERY: none answered; Mesa falls back */
        struct i915_query *q = (struct i915_query *)karg;
        for (uint32_t i = 0; i < q->num_items; i++) {
            struct i915_query_item it;
            if (copy_from_user(&it, (const void *)(q->items_ptr + i * sizeof(it)), sizeof(it))) return -EFAULT_;
            it.length = -EINVAL_;
            if (copy_to_user((void *)(q->items_ptr + i * sizeof(it)), &it, sizeof(it))) return -EFAULT_;
        }
        return 0;
    }
    case 0x7a: {                                     /* VM_CREATE */
        struct i915_vm_control *a = (struct i915_vm_control *)karg;
        for (uint32_t i = 0;; i++) {
            if (i == c->nvms && grow((void **)&c->vms, &c->nvms, sizeof(vm_t *)) != 0) return -ENOMEM_;
            if (!c->vms[i]) {
                if (!(c->vms[i] = vm_new(i + 1))) return -ENOMEM_;
                a->vm_id = i + 1;
                return 0;
            }
        }
    }
    case 0x7b: {                                     /* VM_DESTROY */
        uint32_t id = ((struct i915_vm_control *)karg)->vm_id;
        vm_t *v = vm_lookup(c, id);
        if (!v || id == 1) return -ENOENT_;
        c->vms[id - 1] = 0;
        vm_unref(v);
        return 0;
    }
    default:
        return -EINVAL_;
    }
}

/* ---- start ------------------------------------------------------------------------------ */

int i915_available(void) { return E.ready; }

int i915_init(void) {
    const pci_device_t *pci = igpu_pci();
    int64_t s;
    if (!pci) return -1;
    E.device = pci->device_id;
    E.revision = pci->revision_id;
    E.window = igpu_ggtt_window(WINDOW_PAGES);
    if (E.window < 0) { igpu_report("no room in the GPU's address table for the driver"); return -1; }
    if (scratch_init() != 0) return -1;
    E.hws_phys = page_zeroed();
    E.seq_phys = page_zeroed();
    if (!E.hws_phys || !E.seq_phys) return -1;
    s = gtt_alloc(1); E.hws_gtt = gtt_map(s, E.hws_phys, 1);
    s = gtt_alloc(1); E.seq_gtt = gtt_map(s, E.seq_phys, 1);
    E.seqno = (volatile uint32_t *)PHYS_TO_VIRT(E.seq_phys);
    E.next_seqno = 0;
    if (igpu_reset_render() != 0) { igpu_report("the render engine did not reset for the driver"); return -1; }
    engine_setup();
    E.ready = 1;
    (void)proc_create_kernel(i915_thread);
    return 0;
}
