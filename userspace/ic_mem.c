#include "ic_mem.h"
#include "icda_sys.h"

#define MEM_ALIGN      16ULL
#define MEM_HDR        16ULL
#define MEM_MIN_BLOCK  48ULL
#define MEM_ARENA      (1024ULL * 1024ULL)
#define MEM_BIG        (256ULL * 1024ULL)
#define MEM_USED       1ULL
#define MEM_MAPPED     2ULL
#define MEM_FLAGS      15ULL

typedef struct mem_block {
    uint64_t size;
    uint64_t prev_size;
    struct mem_block *next_free;
    struct mem_block *prev_free;
} mem_block_t;

static mem_block_t *mem_free_head;

static uint64_t mem_round(uint64_t n, uint64_t a) {
    return (n + a - 1) & ~(a - 1);
}

static uint64_t blk_size(const mem_block_t *b) {
    return b->size & ~MEM_FLAGS;
}

static mem_block_t *blk_next(mem_block_t *b) {
    return (mem_block_t *)((uint8_t *)b + blk_size(b));
}

static mem_block_t *blk_prev(mem_block_t *b) {
    return (mem_block_t *)((uint8_t *)b - b->prev_size);
}

static void free_unlink(mem_block_t *b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else mem_free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->next_free = b->prev_free = 0;
}

static void free_push(mem_block_t *b) {
    b->prev_free = 0;
    b->next_free = mem_free_head;
    if (mem_free_head) mem_free_head->prev_free = b;
    mem_free_head = b;
}

static void set_size(mem_block_t *b, uint64_t size, uint64_t flags) {
    b->size = size | flags;
    blk_next(b)->prev_size = size;
}

static int mem_add_arena(uint64_t need) {
    uint64_t bytes = need + 2 * MEM_HDR > MEM_ARENA ? mem_round(need + 2 * MEM_HDR, 4096) : MEM_ARENA;
    uint8_t *base = (uint8_t *)icda_vm_alloc(bytes);
    mem_block_t *b, *end;
    if (!base) return -1;
    b = (mem_block_t *)base;
    end = (mem_block_t *)(base + bytes - MEM_HDR);
    b->prev_size = 0;
    end->size = MEM_USED;
    set_size(b, bytes - MEM_HDR, 0);
    free_push(b);
    return 0;
}

static void split(mem_block_t *b, uint64_t want) {
    uint64_t have = blk_size(b);
    if (have - want >= MEM_MIN_BLOCK) {
        mem_block_t *rest = (mem_block_t *)((uint8_t *)b + want);
        set_size(b, want, b->size & MEM_FLAGS);
        set_size(rest, have - want, 0);
        free_push(rest);
    }
}

static void *big_alloc(uint64_t size) {
    uint64_t bytes = mem_round(size + MEM_HDR, 4096);
    mem_block_t *b = (mem_block_t *)icda_vm_alloc(bytes);
    if (!b) return 0;
    b->size = bytes | MEM_MAPPED | MEM_USED;
    b->prev_size = 0;
    return (uint8_t *)b + MEM_HDR;
}

void *ic_malloc(uint64_t size) {
    uint64_t want;
    mem_block_t *b;
    if (size == 0) size = 1;
    if (size >= MEM_BIG) return big_alloc(size);
    want = mem_round(size + MEM_HDR, MEM_ALIGN);
    if (want < MEM_MIN_BLOCK) want = MEM_MIN_BLOCK;
    for (int pass = 0; pass < 2; pass++) {
        for (b = mem_free_head; b; b = b->next_free) {
            if (blk_size(b) >= want) {
                free_unlink(b);
                b->size |= MEM_USED;
                split(b, want);
                return (uint8_t *)b + MEM_HDR;
            }
        }
        if (pass == 0 && mem_add_arena(want) != 0) return 0;
    }
    return 0;
}

void ic_free(void *ptr) {
    mem_block_t *b, *n;
    if (!ptr) return;
    b = (mem_block_t *)((uint8_t *)ptr - MEM_HDR);
    if (!(b->size & MEM_USED)) return;
    if (b->size & MEM_MAPPED) {
        icda_vm_free(b, blk_size(b));
        return;
    }
    b->size &= ~MEM_USED;
    n = blk_next(b);
    if (!(n->size & MEM_USED)) {
        free_unlink(n);
        set_size(b, blk_size(b) + blk_size(n), 0);
    }
    if (b->prev_size) {
        mem_block_t *p = blk_prev(b);
        if (!(p->size & MEM_USED)) {
            free_unlink(p);
            set_size(p, blk_size(p) + blk_size(b), 0);
            b = p;
        }
    }
    free_push(b);
}

uint64_t ic_mem_usable(const void *ptr) {
    const mem_block_t *b;
    if (!ptr) return 0;
    b = (const mem_block_t *)((const uint8_t *)ptr - MEM_HDR);
    return blk_size(b) - MEM_HDR;
}

void *ic_calloc(uint64_t count, uint64_t size) {
    uint64_t total = count * size;
    uint8_t *p;
    if (size && total / size != count) return 0;
    p = (uint8_t *)ic_malloc(total);
    if (p) {
        for (uint64_t i = 0; i < total; i++) p[i] = 0;
    }
    return p;
}

void *ic_realloc(void *ptr, uint64_t size) {
    mem_block_t *b;
    uint64_t have, want;
    uint8_t *fresh;
    if (!ptr) return ic_malloc(size);
    if (size == 0) {
        ic_free(ptr);
        return 0;
    }
    b = (mem_block_t *)((uint8_t *)ptr - MEM_HDR);
    have = ic_mem_usable(ptr);
    if (size <= have) return ptr;
    if (!(b->size & MEM_MAPPED) && size < MEM_BIG) {
        mem_block_t *n = blk_next(b);
        want = mem_round(size + MEM_HDR, MEM_ALIGN);
        if (!(n->size & MEM_USED) && blk_size(b) + blk_size(n) >= want) {
            free_unlink(n);
            set_size(b, blk_size(b) + blk_size(n), MEM_USED);
            split(b, want);
            return ptr;
        }
    }
    fresh = (uint8_t *)ic_malloc(size);
    if (!fresh) return 0;
    for (uint64_t i = 0; i < have; i++) fresh[i] = ((uint8_t *)ptr)[i];
    ic_free(ptr);
    return fresh;
}
