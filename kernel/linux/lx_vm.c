/* Linux personality memory: mmap regions with pages filled on first touch.
 *
 * A process's mappings are a sorted list of regions (start, end, protection,
 * the file and offset behind them).  mmap only records a region; the page
 * fault handler (and the kernel's user-copy helpers) give a page its memory
 * when it is first touched: zeroes, or the file's bytes.  So reserving
 * gigabytes (JavaScriptCore's heaps do) costs nothing until used.
 *
 * PROT_NONE pages that already have memory stay mapped without the user
 * bit: the process cannot touch them, and their contents survive until
 * mprotect makes them accessible again.  Pages are executable unless
 * nothing says so (no NX), which JIT compilers need anyway.
 *
 * The main program, its brk heap and the stack are mapped by the loader and
 * are not regions here; munmap / mprotect / mremap fall back to acting on
 * their pages directly. */
#include "lx_vm.h"
#include "lx_internal.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"

#define PAGE        PAGE_SIZE_4K
#define PAGE_MASK   (~(PAGE - 1))
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define E_NOMEM     12
#define E_FAULT     14
#define E_INVAL     22
#define E_EXIST     17

/* where mmap places regions it chooses: above the main program and its brk
 * heap, below the stack */
#define AREA_START  0x70000000ULL
#define AREA_END    0x00007F0000000000ULL
/* ICDA's own shared memory windows (ipc/shm.h) live here; mmap stays out */
#define SHM_WIN_START 0x0000010000000000ULL
#define SHM_WIN_END   (SHM_WIN_START + 32ULL * 16ULL * 1024ULL * 1024ULL)

typedef struct lx_vma {
    uint64_t        start, end;
    uint32_t        prot;
    uint32_t        flags;      /* LXVM_* */
    vfs_node_t     *node;       /* file behind it, or 0 */
    uint64_t        off;        /* file offset of start */
    lx_file_t      *file;       /* memory object behind it (memfd, shared anonymous); a reference */
    struct lx_vma  *next;
} lx_vma_t;

static lx_vma_t *new_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, vfs_node_t *node, uint64_t off) {
    lx_vma_t *v = (lx_vma_t *)kmalloc(sizeof(lx_vma_t));
    if (!v) return 0;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->node = node;
    v->off = off;
    v->file = 0;
    v->next = 0;
    return v;
}

/* a copy of v covering [start, end) */
static lx_vma_t *vma_part(const lx_vma_t *v, uint64_t start, uint64_t end) {
    lx_vma_t *n = new_vma(start, end, v->prot, v->flags, v->node, (v->node || v->file) ? v->off + (start - v->start) : 0);
    if (n && v->file) {
        n->file = v->file;
        lxi_file_ref(n->file);
    }
    return n;
}

static void vma_free(lx_vma_t *v) {
    if (v->file) lxi_file_unref(v->file);
    kfree(v);
}

static lx_vma_t *find(process_t *p, uint64_t addr) {
    for (lx_vma_t *v = (lx_vma_t *)p->lx_vmas; v && v->start <= addr; v = v->next)
        if (addr < v->end) return v;
    return 0;
}

static void insert(process_t *p, lx_vma_t *n) {
    lx_vma_t **pp = (lx_vma_t **)&p->lx_vmas;
    while (*pp && (*pp)->start < n->start) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

/* splits v so that a region begins at at (start < at < end) */
static int split(lx_vma_t *v, uint64_t at) {
    lx_vma_t *b;
    if (at <= v->start || at >= v->end) return 0;
    b = vma_part(v, at, v->end);
    if (!b) return -1;
    v->end = at;
    b->next = v->next;
    v->next = b;
    return 0;
}

/* makes region boundaries fall at start and end */
static int carve(process_t *p, uint64_t start, uint64_t end) {
    for (lx_vma_t *v = (lx_vma_t *)p->lx_vmas; v && v->start < end; v = v->next) {
        if (v->start < start && start < v->end && split(v, start) != 0) return -1;
        if (v->start < end && end < v->end && split(v, end) != 0) return -1;
    }
    return 0;
}

/* ---- pages ------------------------------------------------------------------ */

static uint64_t pte_flags(uint32_t prot) {
    if (!prot) return VMM_PRESENT;                       /* kept, but out of the process's reach */
    return (prot & PROT_WRITE) ? VMM_FLAGS_USER_RW : VMM_FLAGS_USER_RO;
}

static void drop_page(addr_space_t *as, uint64_t va, pte_t *pte, void *ctx) {
    (void)va; (void)ctx;
    if (!(*pte & VMM_NOFREE)) pmm_free(PTE_FRAME(*pte));
    *pte = 0;
    vmm_note_unmapped(as);
}

typedef struct { uint32_t prot; int shared; } prot_ctx_t;

/* shared pages stay shared when writable; private ones that other mappings
 * still use (fork) become copy-on-write */
static void reprotect_page(addr_space_t *as, uint64_t va, pte_t *pte, void *ctx) {
    prot_ctx_t *c = (prot_ctx_t *)ctx;
    uint32_t prot = c->prot;
    uint64_t phys = PTE_FRAME(*pte), fl = pte_flags(prot);
    (void)as; (void)va;
    if ((prot & PROT_WRITE) && !c->shared && ((*pte & VMM_COW) || pmm_refcount(phys) > 1)) fl = VMM_FLAGS_USER_RO | VMM_COW;
    *pte = phys | fl | (*pte & VMM_NOFREE) | VMM_PRESENT;
}

typedef struct { uint64_t from, to; addr_space_t *as; } move_ctx_t;

static void move_page(addr_space_t *as, uint64_t va, pte_t *pte, void *ctx) {
    move_ctx_t *m = (move_ctx_t *)ctx;
    uint64_t e = *pte;
    (void)as;
    *pte = 0;
    vmm_note_unmapped(m->as);
    vmm_map_page(m->as, va - m->from + m->to, PTE_FRAME(e), e & ~PTE_ADDR_MASK & ~PTE_PRESENT);
}

/* ---- file page cache --------------------------------------------------------
 * Mapped files (programs' libraries) are read in 256 KB chunks into frames
 * the cache keeps; mappings use those frames copy-on-write, so every
 * process running a library shares one copy of its code.  Reading a file at
 * an offset can be slow (FAT walks the cluster chain for each read), which
 * a page-at-a-time loader made quadratic.  An entry is valid while the
 * file's size and modification time stay what they were. */
#define PC_BUCKETS  16384
#define PC_CHUNK    64                   /* pages read at once */
#define PC_MAX      (192ULL * 1024)      /* frames kept: 768 MB */

typedef struct pcent {
    vfs_node_t   *node;
    uint64_t      idx, size, mtime, phys;
    struct pcent *next;
} pcent_t;

static pcent_t *pc_tab[PC_BUCKETS];
static uint64_t pc_pages;

static uint64_t pc_hash(const vfs_node_t *n, uint64_t idx) {
    uint64_t h = ((uint64_t)(uintptr_t)n >> 4) * 0x9E3779B97F4A7C15ULL ^ idx * 0xC2B2AE3D27D4EB4FULL;
    return (h >> 20) % PC_BUCKETS;
}

static pcent_t *pc_find(vfs_node_t *n, uint64_t idx, uint64_t size, uint64_t mtime) {
    pcent_t **pp = &pc_tab[pc_hash(n, idx)];
    while (*pp) {
        pcent_t *e = *pp;
        if (e->node == n && e->idx == idx) {
            if (e->size == size && e->mtime == mtime) return e;
            *pp = e->next;                       /* the file changed: drop the old page */
            pmm_free(e->phys);
            kfree(e);
            pc_pages--;
            return 0;
        }
        pp = &e->next;
    }
    return 0;
}

/* the cached frame for page idx of a file (one reference for the caller), 0 if none */
static uint64_t pc_frame(vfs_node_t *n, uint64_t idx) {
    uint64_t size = vfs_node_size(n), mtime = vfs_node_modified(n), pages = (size + PAGE - 1) / PAGE;
    uint64_t first, count;
    pcent_t *e;
    char *buf;
    if (idx >= pages) return 0;
    e = pc_find(n, idx, size, mtime);
    if (e) {
        pmm_ref(e->phys);
        return e->phys;
    }
    if (pc_pages + PC_CHUNK > PC_MAX) return 0;
    first = idx & ~(uint64_t)(PC_CHUNK - 1);
    count = pages - first < PC_CHUNK ? pages - first : PC_CHUNK;
    buf = (char *)kmalloc(count * PAGE);
    if (!buf) return 0;
    {
        uint64_t want = count * PAGE;
        if (first * PAGE + want > size) want = size - first * PAGE;
        if (vfs_node_read_at(n, first * PAGE, buf, want) < 0) {
            kfree(buf);
            return 0;
        }
        for (uint64_t i = want; i < count * PAGE; i++) buf[i] = 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t phys;
        pcent_t *ne;
        if (pc_find(n, first + i, size, mtime)) continue;
        phys = pmm_alloc();
        ne = phys ? (pcent_t *)kmalloc(sizeof(pcent_t)) : 0;
        if (!ne) {
            if (phys) pmm_free(phys);
            break;
        }
        for (uint64_t k = 0; k < PAGE / 8; k++) ((uint64_t *)PHYS_TO_VIRT(phys))[k] = ((uint64_t *)(buf + i * PAGE))[k];
        ne->node = n;
        ne->idx = first + i;
        ne->size = size;
        ne->mtime = mtime;
        ne->phys = phys;
        ne->next = pc_tab[pc_hash(n, first + i)];
        pc_tab[pc_hash(n, first + i)] = ne;
        pc_pages++;
    }
    kfree(buf);
    e = pc_find(n, idx, size, mtime);
    if (!e) return 0;
    pmm_ref(e->phys);
    return e->phys;
}

/* gives the page at va its memory (zeroes or the file's bytes) */
static int populate(process_t *p, lx_vma_t *v, uint64_t va) {
    uint64_t phys, src = 0;
    char *mem;
    if (v->file) {
        /* a memory object: shared mappings use its frame, private ones a copy */
        src = v->file->ops->frame ? v->file->ops->frame(v->file->obj, v->off + (va - v->start)) : 0;
        if (!src) return -1;
        if (v->flags & LXVM_SHARED) {
            pmm_ref(src);
            if (vmm_map_page(p->addr_space, va, src, pte_flags(v->prot)) != 0) {
                pmm_free(src);
                return -1;
            }
            return 0;
        }
    }
    if (v->node) {
        /* a file: the page cache's frame, copied only when written */
        uint64_t cached = pc_frame(v->node, (v->off + (va - v->start)) / PAGE);
        if (cached) {
            uint64_t fl = (v->prot & PROT_WRITE) ? (VMM_FLAGS_USER_RO | VMM_COW) : pte_flags(v->prot);
            if (vmm_map_page(p->addr_space, va, cached, fl) != 0) {
                pmm_free(cached);
                return -1;
            }
            return 0;
        }
    }
    phys = pmm_alloc();
    if (!phys) return -1;
    mem = (char *)PHYS_TO_VIRT(phys);
    for (int i = 0; i < (int)(PAGE / 8); i++) ((uint64_t *)mem)[i] = src ? ((uint64_t *)PHYS_TO_VIRT(src))[i] : 0;
    if (v->node) {
        uint64_t off = v->off + (va - v->start), size = vfs_node_size(v->node);
        if (off < size) (void)vfs_node_read_at(v->node, off, mem, size - off < PAGE ? size - off : PAGE);
    }
    if (vmm_map_page(p->addr_space, va, phys, pte_flags(v->prot)) != 0) {
        pmm_free(phys);
        return -1;
    }
    return 0;
}

/* ---- faults ------------------------------------------------------------------ */

int lxvm_fault(process_t *p, uint64_t addr, int write) {
    lx_vma_t *v;
    uint64_t va = addr & PAGE_MASK, phys;
    if (!p || !p->linux_personality || !p->addr_space) return 0;
    v = find(p, addr);
    if (!v || !v->prot || (write && !(v->prot & PROT_WRITE)) || !(v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return 0;
    phys = vmm_virt_to_phys(p->addr_space, va);
    if (!phys) return populate(p, v, va) == 0;
    /* present: protection changed since it was mapped, or copy-on-write */
    {
        prot_ctx_t c = { v->prot, (v->flags & LXVM_SHARED) != 0 };
        vmm_walk_user(p->addr_space, va, va + PAGE, reprotect_page, &c);
    }
    if (write && !vmm_page_writable(p->addr_space, va) && vmm_cow_break(p->addr_space, va) != 0) return 0;
    return 1;
}

/* ---- mmap / munmap / mprotect / mremap / madvise ----------------------------------- */

static int range_free(process_t *p, uint64_t start, uint64_t end) {
    lx_vma_t *v;
    if (start < AREA_START || end > AREA_END || end <= start) return 0;   /* below: the program and its brk heap */
    if (start < SHM_WIN_END && end > SHM_WIN_START) return 0;
    for (v = (lx_vma_t *)p->lx_vmas; v && v->start < end; v = v->next)
        if (v->end > start) return 0;
    return 1;
}

static uint64_t find_hole(process_t *p, uint64_t len, uint64_t align) {
    uint64_t at = AREA_START;
    lx_vma_t *v = (lx_vma_t *)p->lx_vmas;
    if (align < PAGE) align = PAGE;
    for (;;) {
        at = (at + align - 1) & ~(align - 1);
        if (at + len < at || at + len > AREA_END) return 0;
        if (at < SHM_WIN_END && at + len > SHM_WIN_START) {
            at = SHM_WIN_END;
            continue;
        }
        while (v && v->end <= at) v = v->next;          /* regions wholly before */
        if (!v || at + len <= v->start) return at;
        at = v->end;                                     /* overlaps v: try after it */
    }
}

static void clear_range(process_t *p, uint64_t start, uint64_t end, int free_pages) {
    lx_vma_t **pp = (lx_vma_t **)&p->lx_vmas;
    (void)carve(p, start, end);
    while (*pp) {
        lx_vma_t *v = *pp;
        if (v->start >= start && v->end <= end) {
            *pp = v->next;
            vma_free(v);
            continue;
        }
        pp = &v->next;
    }
    if (free_pages) vmm_walk_user(p->addr_space, start, end, drop_page, 0);
}

int64_t lxvm_mmap(process_t *p, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, vfs_node_t *node,
                  lx_file_t *file, uint64_t off) {
    uint64_t size = (len + PAGE - 1) & PAGE_MASK, start;
    lx_vma_t *v;
    if (!len || !size || (off & (PAGE - 1))) return -E_INVAL;
    if (flags & LXVM_FIXED) {
        if ((addr & (PAGE - 1)) || addr + size < addr || addr + size > USER_STACK_LIMIT) return -E_INVAL;
        if ((flags & LXVM_NOREPLACE) && !range_free(p, addr, addr + size)) return -E_EXIST;
        clear_range(p, addr, addr + size, 1);
        start = addr;
    } else if (addr && !(addr & (PAGE - 1)) && range_free(p, addr, addr + size)) {
        start = addr;                                    /* the hint is free */
    } else {
        start = find_hole(p, size, PAGE);
        if (!start) return -E_NOMEM;
    }
    v = new_vma(start, start + size, prot, flags & (LXVM_SHARED | LXVM_PRIVATE | LXVM_ANON), node, off);
    if (!v) return -E_NOMEM;
    if (file) {
        v->file = file;
        lxi_file_ref(file);
    }
    insert(p, v);
    if ((flags & LXVM_POPULATE) && prot)
        for (uint64_t va = start; va < start + size; va += PAGE)
            if (!vmm_virt_to_phys(p->addr_space, va)) (void)populate(p, v, va);
    return (int64_t)start;
}

int lxvm_munmap(process_t *p, uint64_t addr, uint64_t len) {
    uint64_t end = addr + ((len + PAGE - 1) & PAGE_MASK);
    if ((addr & (PAGE - 1)) || !len || end < addr || end > USER_STACK_LIMIT) return -E_INVAL;
    clear_range(p, addr, end, 1);
    return 0;
}

int lxvm_mprotect(process_t *p, uint64_t addr, uint64_t len, uint32_t prot) {
    uint64_t end = addr + ((len + PAGE - 1) & PAGE_MASK);
    if (addr & (PAGE - 1)) return -E_INVAL;
    if (end <= addr) return 0;
    if (carve(p, addr, end) != 0) return -E_NOMEM;
    {
        prot_ctx_t c = { prot, 0 };
        vmm_walk_user(p->addr_space, addr, end, reprotect_page, &c);
    }
    for (lx_vma_t *v = (lx_vma_t *)p->lx_vmas; v && v->start < end; v = v->next) {
        if (v->start < addr || v->end > end) continue;
        v->prot = prot;
        if (v->flags & LXVM_SHARED) {          /* shared pages stay writable, not copy-on-write */
            prot_ctx_t c = { prot, 1 };
            vmm_walk_user(p->addr_space, v->start, v->end, reprotect_page, &c);
        }
    }
    return 0;
}

/* the main thread's stack counts as one mapping (musl measures it with mremap) */
static int in_stack(uint64_t a) {
    return a >= USER_STACK_LIMIT && a < USER_STACK_TOP;
}

int64_t lxvm_mremap(process_t *p, uint64_t old, uint64_t old_len, uint64_t new_len, uint32_t flags, uint64_t new_addr) {
    uint64_t olds = (old_len + PAGE - 1) & PAGE_MASK, news = (new_len + PAGE - 1) & PAGE_MASK;
    lx_vma_t *v;
    if ((old & (PAGE - 1)) || !news) return -E_INVAL;
    v = find(p, old);
    if (!v) {
        if (in_stack(old)) return news <= olds ? (int64_t)old : -E_NOMEM;
        return -E_FAULT;
    }
    if (!olds) return -E_INVAL;                          /* duplicating shared mappings: not supported */
    if (old + olds > v->end) return -E_FAULT;
    if (news <= olds) {
        if (news < olds) clear_range(p, old + news, old + olds, 1);
        return (int64_t)old;
    }
    if (!(flags & LXVM_MREMAP_FIXED) && old + olds == v->end && range_free(p, v->end, old + news)) {
        v->end = old + news;                             /* grows in place */
        return (int64_t)old;
    }
    if (!(flags & LXVM_MREMAP_MAYMOVE)) return -E_NOMEM;
    {
        uint64_t to;
        lx_vma_t *n;
        move_ctx_t m;
        if (flags & LXVM_MREMAP_FIXED) {
            if (new_addr & (PAGE - 1)) return -E_INVAL;
            clear_range(p, new_addr, new_addr + news, 1);
            to = new_addr;
        } else {
            to = find_hole(p, news, PAGE);
            if (!to) return -E_NOMEM;
        }
        n = vma_part(v, old, old + olds);
        if (n) {
            n->start = to;
            n->end = to + news;
        }
        if (!n) return -E_NOMEM;
        m.from = old;
        m.to = to;
        m.as = p->addr_space;
        vmm_walk_user(p->addr_space, old, old + olds, move_page, &m);
        clear_range(p, old, old + olds, 0);
        insert(p, n);
        return (int64_t)to;
    }
}

int lxvm_madvise(process_t *p, uint64_t addr, uint64_t len, int advice) {
    uint64_t end = addr + ((len + PAGE - 1) & PAGE_MASK);
    if (addr & (PAGE - 1)) return -E_INVAL;
    /* MADV_DONTNEED, MADV_FREE: the memory goes; the next touch gets zeroes
     * (or the file's bytes again) */
    if ((advice == 4 || advice == 8) && end > addr) {
        for (lx_vma_t *v = (lx_vma_t *)p->lx_vmas; v && v->start < end; v = v->next) {
            uint64_t a = v->start > addr ? v->start : addr, b = v->end < end ? v->end : end;
            if (a < b) vmm_walk_user(p->addr_space, a, b, drop_page, 0);
        }
    }
    return 0;
}

int lxvm_mapped(process_t *p, uint64_t addr) {
    return find(p, addr) != 0 || in_stack(addr);
}

/* ---- process lifetime ---------------------------------------------------------------- */

int lxvm_fork(process_t *parent, process_t *child) {
    lx_vma_t **tail = (lx_vma_t **)&child->lx_vmas;
    child->lx_vmas = 0;
    for (lx_vma_t *v = (lx_vma_t *)parent->lx_vmas; v; v = v->next) {
        lx_vma_t *n = vma_part(v, v->start, v->end);
        if (!n) return -1;
        *tail = n;
        tail = &n->next;
    }
    /* the copy made every writable page copy-on-write; shared regions are
     * shared again in both */
    for (lx_vma_t *v = (lx_vma_t *)parent->lx_vmas; v; v = v->next) {
        if ((v->flags & LXVM_SHARED) && (v->prot & PROT_WRITE)) {
            prot_ctx_t c = { v->prot, 1 };
            vmm_walk_user(parent->addr_space, v->start, v->end, reprotect_page, &c);
            vmm_walk_user(child->addr_space, v->start, v->end, reprotect_page, &c);
        }
    }
    return 0;
}

void lxvm_free(process_t *p) {
    lx_vma_t *v = (lx_vma_t *)p->lx_vmas;
    p->lx_vmas = 0;
    while (v) {
        lx_vma_t *n = v->next;
        vma_free(v);
        v = n;
    }
}

/* The kernel's user-copy helpers: memory for a page of the current process
 * the first time the kernel writes or reads it for the process. */
int lxvm_fault_current(uint64_t addr, int write) {
    return lxvm_fault(sched_current_process(), addr, write);
}

/* /proc/self/maps lines into buf (returns the length) */
uint64_t lxvm_maps(process_t *p, char *buf, uint64_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint64_t n = 0;
    for (lx_vma_t *v = (lx_vma_t *)p->lx_vmas; v; v = v->next) {
        char line[96];
        int k = 0;
        for (int s = 0; s < 2; s++) {
            uint64_t x = s ? v->end : v->start;
            for (int i = 44; i >= 0; i -= 4) line[k++] = hex[(x >> i) & 15];
            line[k++] = s ? ' ' : '-';
        }
        line[k++] = (v->prot & PROT_READ) ? 'r' : '-';
        line[k++] = (v->prot & PROT_WRITE) ? 'w' : '-';
        line[k++] = (v->prot & PROT_EXEC) ? 'x' : '-';
        line[k++] = (v->flags & LXVM_SHARED) ? 's' : 'p';
        {
            const char *rest = " 00000000 00:00 0\n";
            for (const char *c = rest; *c; c++) line[k++] = *c;
        }
        if (n + (uint64_t)k > cap) break;
        for (int i = 0; i < k; i++) buf[n + (uint64_t)i] = line[i];
        n += (uint64_t)k;
    }
    return n;
}

/* The loader's segments (program, dynamic loader): already mapped; recorded
 * so mmap places nothing on top of them. */
int lxvm_reserve(process_t *p, uint64_t start, uint64_t end, uint32_t prot) {
    lx_vma_t *v;
    start &= PAGE_MASK;
    end = (end + PAGE - 1) & PAGE_MASK;
    if (end <= start) return 0;
    v = new_vma(start, end, prot, LXVM_PRIVATE | LXVM_ANON, 0, 0);
    if (!v) return -1;
    insert(p, v);
    return 0;
}

/* frees a region list taken off a process (execve keeps the old one until
 * the new image has loaded) */
void lxvm_free_list(void *list) {
    lx_vma_t *v = (lx_vma_t *)list;
    while (v) {
        lx_vma_t *n = v->next;
        vma_free(v);
        v = n;
    }
}

/* "name+0xoffset" for an address in a mapped file (crash reports) */
void lxvm_describe(process_t *p, uint64_t addr, char *out, uint64_t cap) {
    static const char hex[] = "0123456789abcdef";
    lx_vma_t *v = p ? find(p, addr) : 0;
    const char *name = v && v->node ? vfs_node_name(v->node) : v ? "(anonymous)" : "(not mapped)";
    uint64_t off = v ? (v->node ? v->off : 0) + (addr - v->start) : addr, n = 0;
    char num[20];
    int k = 0;
    while (*name && n + 1 < cap) out[n++] = *name++;
    if (n + 4 < cap) {
        out[n++] = '+';
        out[n++] = '0';
        out[n++] = 'x';
    }
    do { num[k++] = hex[off & 15]; off >>= 4; } while (off && k < 16);
    while (k && n + 1 < cap) out[n++] = num[--k];
    out[n] = 0;
}

/* 1 if addr is in an executable mapping of a file (a return address, in
 * crash reports) */
int lxvm_is_code(process_t *p, uint64_t addr) {
    lx_vma_t *v = p ? find(p, addr) : 0;
    return v && v->node && (v->prot & PROT_EXEC);
}
