/* Swap: when memory runs short, pages programs have not used lately are
 * written to the swap area and their memory goes to whoever needs it; a
 * program touching such a page waits while it is read back.
 *
 * The swap area is one or more runs of sectors on a disk (a swap partition,
 * or the clusters of /swapfile on ICDA's partition).  Each 4 KB page of it
 * is a slot; refs counts the page table entries holding a slot (two or more
 * after a fork, until each program reads its copy back).
 *
 * What is written out: a user program's page that only it uses - not shared
 * memory or the framebuffer (VMM_NOFREE), not a file's cached page or one a
 * fork still shares (frame refcount above one), not a Linux program's shared
 * mapping, and nothing of the window manager's, which has to stay quick.
 * Pages are picked like a clock: the hand passes over each program's pages
 * in turn, and a page used since the hand last passed (its accessed bit)
 * gets another round.
 *
 * Everything here runs under the kernel lock, the disk transfers too (the
 * drivers wait for the disk); BATCH pages at a time, so the lock is not held
 * for long.  Slots are handed out in order, so a batch usually goes to
 * consecutive slots and is written with one transfer. */

#include "swap.h"
#include "pmm.h"
#include "../cpu/smp.h"
#include "../proc/sched.h"
#include "../proc/process.h"
#include "../linux/lx_vm.h"
#include "../drivers/serial/serial.h"

#define BATCH       16
#define SCAN_LIMIT  (1u << 18)       /* entries looked at per call: two passes of 512 MB */

static block_device_t *sdev;
static swap_run_t runs[SWAP_MAX_RUNS];
static uint64_t run_first[SWAP_MAX_RUNS];   /* the first slot of each run */
static int nruns;
static uint64_t total, nfree, next_slot;
static uint16_t *refs;
static uint32_t spp;                         /* sectors per page */
static uint8_t *stage;                       /* BATCH pages: a batch gathered for one write */
static int busy;
static uint64_t outs, ins;

/* the clock hand: the program it is in, and where */
static uint64_t hand_pid, hand_va;
/* a refill that found nothing to write out (after a fork most pages are
 * shared, which are not swapped): no refill before this tick */
static uint64_t refill_after;

static void log_num(uint64_t v) {
    char d[24], s[24];
    int k = 0, n = 0;
    do d[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) s[n++] = d[--k];
    s[n] = 0;
    serial_write(s);
}

int swap_on(block_device_t *dev, const swap_run_t *r, int n, const char *what) {
    uint64_t pages = 0, table_pages, phys;
    if (total || !dev || !dev->read || !dev->write || n <= 0 || n > SWAP_MAX_RUNS) return -1;
    if (!dev->sector_size || 4096 % dev->sector_size) return -1;
    for (int i = 0; i < n; i++) pages += r[i].pages;
    if (pages < 64) return -1;
    table_pages = (pages * sizeof(uint16_t) + 4095) / 4096;
    phys = pmm_alloc_contiguous(table_pages);
    if (!phys) return -1;
    refs = (uint16_t *)PHYS_TO_VIRT(phys);
    for (uint64_t i = 0; i < pages; i++) refs[i] = 0;
    phys = pmm_alloc_contiguous(BATCH);          /* without it, a page per transfer */
    stage = phys ? (uint8_t *)PHYS_TO_VIRT(phys) : 0;
    for (int i = 0; i < n; i++) {
        runs[i] = r[i];
        run_first[i] = i ? run_first[i - 1] + runs[i - 1].pages : 0;
    }
    nruns = n;
    sdev = dev;
    spp = 4096 / dev->sector_size;
    nfree = pages;
    next_slot = 0;
    total = pages;                   /* last: swap_out works from here on */
    {
        /* a reserve of 1/64 of the memory, 2 to 64 MB, kept free for whoever needs it at once */
        uint64_t low = pmm_usable_frames() / 64;
        if (low < 512) low = 512;
        if (low > 16384) low = 16384;
        pmm_set_swap(swap_out, low);
    }
    serial_write("[swap] on: ");
    serial_write(what ? what : "?");
    serial_write(", ");
    log_num(pages * 4096 / (1024 * 1024));
    serial_write(" MB\n");
    return 0;
}

uint64_t swap_total_pages(void) { return total; }
int swap_uses_device(const block_device_t *dev) { return total && dev && dev == sdev; }
uint64_t swap_free_pages(void) { return total ? nfree : 0; }

void swap_counts(uint64_t *o, uint64_t *i) {
    if (o) *o = outs;
    if (i) *i = ins;
}

/* ---- slots ------------------------------------------------------------------------------ */

static uint64_t slot_alloc(void) {
    if (!nfree) return (uint64_t)-1;
    for (uint64_t k = 0; k < total; k++) {
        uint64_t s = next_slot + k;
        if (s >= total) s -= total;
        if (!refs[s]) {
            refs[s] = 1;
            nfree--;
            next_slot = s + 1 < total ? s + 1 : 0;
            return s;
        }
    }
    return (uint64_t)-1;
}

/* a count that reached 0xFFFF stays there: the slot is kept until the
 * next start rather than freed while a table entry still holds it */
static void slot_put(uint64_t s) {
    if (s >= total || !refs[s] || refs[s] == 0xFFFF) return;
    if (--refs[s] == 0) nfree++;
}

/* an entry goes from as (0: the whole space is going) */
void swap_entry_free(addr_space_t *as, pte_t e) {
    if (!pte_is_swap(e)) return;
    slot_put(pte_swap_slot(e));
    if (as && as->swapped_pages) as->swapped_pages--;
}

/* a copy of an entry goes into as (fork) */
void swap_entry_dup(addr_space_t *as, pte_t e) {
    uint64_t s = pte_swap_slot(e);
    if (!pte_is_swap(e)) return;
    if (s < total && refs[s] && refs[s] < 0xFFFF) refs[s]++;
    if (as) as->swapped_pages++;
}

/* the run of the area slot s is in, -1 if none */
static int run_of(uint64_t s) {
    for (int i = 0; i < nruns; i++)
        if (s >= run_first[i] && s < run_first[i] + runs[i].pages) return i;
    return -1;
}

/* n slots from s on, which are in one run */
static int slots_io(uint64_t s, uint64_t n, void *buf, int write) {
    int r = run_of(s);
    uint64_t lba;
    if (r < 0 || s + n > run_first[r] + runs[r].pages) return -1;
    lba = runs[r].lba + (s - run_first[r]) * spp;
    return write ? sdev->write(sdev->context, lba, (uint32_t)(n * spp), buf)
                 : sdev->read(sdev->context, lba, (uint32_t)(n * spp), buf);
}

static void copy_page(void *dst, const void *src) {
    uint64_t n = 4096 / 8;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

/* ---- writing out ------------------------------------------------------------------------ */

typedef struct {
    pte_t   *pte;
    uint64_t va;
    pte_t    old;
    uint64_t slot;
} victim_t;

typedef struct {
    process_t   *p;
    addr_space_t *as;
    victim_t     v[BATCH];
    int          n;
    uint64_t     want, got, scanned;
    int          full;               /* no slot left */
    uint64_t     shared_start, shared_end;   /* a Linux program's shared mapping at or after va */
} scan_t;

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* the batch's entries are swap entries now: once no processor can still
 * write the pages, they go to their slots and their memory is freed */
static void flush(scan_t *s) {
    int here = s->as && (read_cr3() & PTE_ADDR_MASK) == s->as->pml4_phys;
    if (!s->n) return;
    for (int i = 0; i < s->n; i++)
        if (here) vmm_invlpg(s->v[i].va);
    tlb_shootdown(s->as);
    /* in slot order (they are handed out in order, so this seldom moves any) */
    for (int i = 1; i < s->n; i++) {
        victim_t t = s->v[i];
        int j = i;
        while (j > 0 && s->v[j - 1].slot > t.slot) {
            s->v[j] = s->v[j - 1];
            j--;
        }
        s->v[j] = t;
    }
    for (int i = 0; i < s->n;) {
        int j = i + 1, ok;
        /* consecutive slots of one run: gathered, and written at once */
        while (stage && j < s->n && s->v[j].slot == s->v[j - 1].slot + 1 && run_of(s->v[j].slot) == run_of(s->v[i].slot))
            j++;
        if (j - i == 1) {
            ok = slots_io(s->v[i].slot, 1, PHYS_TO_VIRT(PTE_FRAME(s->v[i].old)), 1) == 0;
        } else {
            for (int k = i; k < j; k++) copy_page(stage + (uint64_t)(k - i) * 4096, PHYS_TO_VIRT(PTE_FRAME(s->v[k].old)));
            ok = slots_io(s->v[i].slot, (uint64_t)(j - i), stage, 1) == 0;
        }
        for (int k = i; k < j; k++) {
            victim_t *v = &s->v[k];
            if (!ok) {
                /* the disk failed: the page stays where it was */
                __atomic_store_n(v->pte, v->old, __ATOMIC_RELEASE);
                slot_put(v->slot);
                continue;
            }
            pmm_free(PTE_FRAME(v->old));
            s->as->swapped_pages++;
            outs++;
            s->got++;
        }
        i = j;
    }
    s->n = 0;
}

static int swappable_process(const process_t *p) {
    if (!p || p->kind != PROCESS_USER || p->ex_wm) return 0;
    if (p->state == PROCESS_NEW || p->state == PROCESS_EXITED || p->state == PROCESS_REAPED) return 0;
    return p->addr_space && p->addr_space != vmm_kernel_address_space();
}

/* one entry the hand passes: 1 to stop (the batch is full, enough was found,
 * or no slot is left) */
static int consider(scan_t *s, pte_t *pte, uint64_t va) {
    pte_t e = *pte;
    uint64_t frame, slot;
    s->scanned++;
    if (!(e & PTE_PRESENT) || !(e & PTE_USER) || (e & (VMM_NOFREE | PTE_HUGE))) return 0;
    if (s->p->linux_personality) {
        while (s->shared_end && va >= s->shared_end)
            if (!lxvm_next_shared(s->p, va, &s->shared_start, &s->shared_end)) s->shared_end = 0;
        if (s->shared_end && va >= s->shared_start) return 0;
    }
    frame = PTE_FRAME(e);
    if (pmm_refcount(frame) != 1) return 0;
    if (e & PTE_ACCESSED) {
        /* used since the hand last passed: another round */
        __atomic_and_fetch(pte, ~PTE_ACCESSED, __ATOMIC_RELAXED);
        return 0;
    }
    slot = slot_alloc();
    if (slot == (uint64_t)-1) {
        s->full = 1;
        return 1;
    }
    e = __atomic_exchange_n(pte, pte_swap_make(slot, e), __ATOMIC_ACQ_REL);
    s->v[s->n].pte = pte;
    s->v[s->n].va = va;
    s->v[s->n].old = e;
    s->v[s->n].slot = slot;
    s->n++;
    if (s->n == BATCH) flush(s);
    return s->got + (uint64_t)s->n >= s->want;
}

/* the hand's pass over one program, from va on; returns where it stopped,
 * or 0 when it reached the program's end */
static uint64_t pass(scan_t *s, uint64_t va) {
    pte_t *pml4 = (pte_t *)PHYS_TO_VIRT(s->as->pml4_phys);
    const uint64_t end = 0x0000800000000000ULL;
    s->shared_end = 0;
    if (s->p->linux_personality && !lxvm_next_shared(s->p, va, &s->shared_start, &s->shared_end)) s->shared_end = 0;
    va &= ~0xFFFULL;
    while (va < end) {
        uint64_t i4 = VA_PML4_IDX(va), i3, i2, i1;
        pte_t *pdpt, *pd, *pt;
        if (s->scanned >= SCAN_LIMIT) return va;
        if (!(pml4[i4] & PTE_PRESENT)) { va = (va | ((1ULL << 39) - 1)) + 1; continue; }
        pdpt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pml4[i4]));
        i3 = VA_PDPT_IDX(va);
        if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) { va = (va | ((1ULL << 30) - 1)) + 1; continue; }
        pd = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pdpt[i3]));
        i2 = VA_PD_IDX(va);
        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) { va = (va | ((1ULL << 21) - 1)) + 1; continue; }
        pt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pd[i2]));
        for (i1 = VA_PT_IDX(va); i1 < 512; i1++, va += PAGE_SIZE_4K) {
            if (!(pt[i1] & PTE_PRESENT)) continue;
            if (consider(s, &pt[i1], va)) return va + PAGE_SIZE_4K;
        }
    }
    return 0;
}

uint64_t swap_out(uint64_t want, int urgent) {
    cpu_t *c = this_cpu();
    scan_t s;
    process_t *p, *start;
    int laps = 0;
    if (!total || busy || !want || !c->bkl_held || !c->current || c->current->in_irq) return 0;
    /* the window manager uses the reserve and leaves its refilling to the
     * others: the desktop does not wait for the disk unless memory is gone */
    if (!urgent && c->current->owner && c->current->owner->ex_wm) return 0;
    if (!urgent && sched_ticks() < refill_after) return 0;
    busy = 1;
    s.n = 0;
    s.want = want;
    s.got = 0;
    s.scanned = 0;
    s.full = 0;
    /* the hand's program, or the first if it has gone */
    for (p = sched_first_process(); p && p->pid != hand_pid; p = p->next_all) {}
    if (!p) {
        p = sched_first_process();
        hand_va = 0;
    }
    start = p;
    while (p && s.got + (uint64_t)s.n < want && !s.full && s.scanned < SCAN_LIMIT) {
        uint64_t stop = 0;
        if (swappable_process(p)) {
            s.p = p;
            s.as = p->addr_space;
            stop = pass(&s, p->pid == hand_pid ? hand_va : 0);
            flush(&s);
        }
        if (stop) {
            hand_pid = p->pid;
            hand_va = stop;
            break;
        }
        p = p->next_all ? p->next_all : sched_first_process();
        hand_pid = p ? p->pid : 0;
        hand_va = 0;
        /* two laps: the first clears accessed bits, the second finds them still clear */
        if (p == start && ++laps >= 2) break;
    }
    flush(&s);
    if (!s.got && !urgent) refill_after = sched_ticks() + 10;        /* 100 ms */
    busy = 0;
    return s.got;
}

/* ---- reading back ----------------------------------------------------------------------- */

int swap_in(addr_space_t *as, uint64_t va, pte_t *pte) {
    pte_t e = *pte, now;
    uint64_t frame, slot, flags;
    if (!pte_is_swap(e)) return 0;
    slot = pte_swap_slot(e);
    frame = pmm_alloc();                     /* may write other pages out first */
    if (!frame) return -1;
    now = *pte;
    if (now != e) {
        /* something else read it back meanwhile */
        pmm_free(frame);
        return (now & PTE_PRESENT) ? 1 : -1;
    }
    if (slots_io(slot, 1, PHYS_TO_VIRT(frame), 0) != 0) {
        pmm_free(frame);
        return -2;
    }
    slot_put(slot);
    if (as && as->swapped_pages) as->swapped_pages--;
    flags = (e & PTE_SWAP_KEEP) | PTE_PRESENT | PTE_ACCESSED;
    __atomic_store_n(pte, (frame & PTE_ADDR_MASK) | flags, __ATOMIC_RELEASE);
    if (as && (read_cr3() & PTE_ADDR_MASK) == as->pml4_phys) vmm_invlpg(va & ~0xFFFULL);
    ins++;
    return 1;
}
