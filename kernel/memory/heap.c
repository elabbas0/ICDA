#include "heap.h"

#include "pmm.h"
#include "vmm.h"

/* The kernel heap.
 *
 * Small allocations (up to 2 KB) come from size classes: each class keeps a
 * list of free objects, refilled by carving a chunk from the large-block
 * heap, so kmalloc and kfree are a few instructions however many objects
 * are alive (the page cache alone keeps tens of thousands).
 *
 * Larger allocations are blocks in an address-ordered list (so freed
 * neighbours merge) with a second list of just the free blocks, which
 * first-fit searches.  The heap grows at its top as needed. */

#define KERNEL_HEAP_BASE  0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_LIMIT 0xFFFFFFFF98000000ULL
#define HEAP_GROW_PAGES   16ULL
#define HEAP_ALIGN        16ULL

#define MAGIC_LARGE 0x4C415247U          /* "LARG" */
#define MAGIC_SMALL 0x534D4C4CU          /* "SMLL" */

typedef struct heap_block {
    uint64_t size;                       /* payload bytes */
    struct heap_block *next, *prev;      /* neighbours by address */
    struct heap_block *fnext, *fprev;    /* free list (free blocks only) */
    uint32_t free;
    uint32_t magic;                      /* right before the payload */
} heap_block_t;

/* header of a size-class object, also right before the payload */
typedef struct {
    void    *next_free;                  /* while free */
    uint32_t cls;
    uint32_t magic;
} small_hdr_t;

static const uint32_t class_size[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024, 1280, 1536, 2048
};
#define NCLASSES (sizeof(class_size) / sizeof(class_size[0]))
#define SMALL_MAX 2048U
static uint8_t class_of[SMALL_MAX / 16 + 1];   /* (need / 16) -> class */
static small_hdr_t *class_free[NCLASSES];
#define END_OF_LIST ((void *)1)            /* next_free of a list's last object (0 means in use) */

static heap_block_t *heap_head = 0, *heap_tail = 0, *free_head = 0;
static uint64_t heap_top = KERNEL_HEAP_BASE;
static uint64_t heap_total = 0;
static uint64_t heap_used = 0;
static int heap_ready = 0;

static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void zero_bytes(void *ptr, uint64_t size) {
    __asm__ volatile("rep stosb" : "+D"(ptr), "+c"(size) : "a"(0) : "memory");
}

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(size) : : "memory");
}

/* ---- large blocks ---------------------------------------------------------- */

static void free_push(heap_block_t *b) {
    b->free = 1;
    b->fprev = 0;
    b->fnext = free_head;
    if (free_head) free_head->fprev = b;
    free_head = b;
}

static void free_unlink(heap_block_t *b) {
    if (b->fprev) b->fprev->fnext = b->fnext;
    else free_head = b->fnext;
    if (b->fnext) b->fnext->fprev = b->fprev;
    b->fnext = b->fprev = 0;
    b->free = 0;
}

/* b's address neighbour after it is absorbed into b */
static void absorb_next(heap_block_t *b) {
    heap_block_t *n = b->next;
    b->size += sizeof(heap_block_t) + n->size;
    b->next = n->next;
    if (b->next) b->next->prev = b;
    else heap_tail = b;
}

/* a free block from what is left of b past size bytes (b is in use) */
static void split_block(heap_block_t *block, uint64_t size) {
    uint64_t remaining = block->size - size;
    heap_block_t *next;

    if (remaining <= sizeof(heap_block_t) + HEAP_ALIGN) return;

    next = (heap_block_t *)((uint8_t *)(block + 1) + size);
    next->size = remaining - sizeof(heap_block_t);
    next->next = block->next;
    next->prev = block;
    next->magic = MAGIC_LARGE;
    if (next->next) next->next->prev = next;
    else heap_tail = next;
    block->next = next;
    block->size = size;
    if (next->next && next->next->free) {
        free_unlink(next->next);
        absorb_next(next);
    }
    free_push(next);
}

static int heap_grow(uint64_t min_payload) {
    uint64_t bytes = align_up(min_payload + sizeof(heap_block_t), PAGE_SIZE_4K);
    uint64_t pages = bytes / PAGE_SIZE_4K;
    uint64_t grow_pages = pages > HEAP_GROW_PAGES ? pages : HEAP_GROW_PAGES;
    uint64_t grow_size = grow_pages * PAGE_SIZE_4K;
    uint64_t base = heap_top;
    heap_block_t *block;

    if (heap_top + grow_size > KERNEL_HEAP_LIMIT) {
        return -1;
    }

    for (uint64_t i = 0; i < grow_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            return -1;
        }
        if (vmm_map_page(vmm_kernel_address_space(), heap_top, phys, VMM_FLAGS_KERNEL_RW) != 0) {
            pmm_free(phys);
            return -1;
        }
        heap_top += PAGE_SIZE_4K;
    }

    block = (heap_block_t *)(uintptr_t)base;
    block->size = grow_size - sizeof(heap_block_t);
    block->next = 0;
    block->prev = heap_tail;
    block->magic = MAGIC_LARGE;
    heap_total += grow_size;

    if (!heap_head) heap_head = block;
    if (heap_tail) heap_tail->next = block;
    heap_tail = block;
    if (block->prev && block->prev->free &&
        (uint8_t *)(block->prev + 1) + block->prev->size == (uint8_t *)block) {
        heap_block_t *prev = block->prev;
        free_unlink(prev);
        absorb_next(prev);
        free_push(prev);
    } else {
        free_push(block);
    }
    return 0;
}

static void *large_alloc(uint64_t need) {
    for (;;) {
        for (heap_block_t *b = free_head; b; b = b->fnext) {
            if (b->size >= need) {
                free_unlink(b);
                split_block(b, need);
                heap_used += b->size;
                return b + 1;
            }
        }
        if (heap_grow(need) != 0) return 0;
    }
}

static void large_free(heap_block_t *block) {
    if (block->free) return;
    heap_used -= block->size;
    if (block->next && block->next->free) {
        free_unlink(block->next);
        absorb_next(block);
    }
    if (block->prev && block->prev->free) {
        heap_block_t *prev = block->prev;
        free_unlink(prev);
        absorb_next(prev);
        free_push(prev);
        return;
    }
    free_push(block);
}

/* ---- size classes ------------------------------------------------------------- */

static int refill(uint32_t cls) {
    uint64_t stride = sizeof(small_hdr_t) + class_size[cls];
    uint64_t count = stride <= 256 ? 256 : stride <= 1024 ? 64 : 16;
    uint8_t *chunk = (uint8_t *)large_alloc(stride * count);
    if (!chunk) return -1;
    for (uint64_t i = 0; i < count; i++) {
        small_hdr_t *h = (small_hdr_t *)(chunk + i * stride);
        h->cls = cls;
        h->magic = MAGIC_SMALL;
        h->next_free = class_free[cls] ? (void *)class_free[cls] : END_OF_LIST;
        class_free[cls] = h;
    }
    return 0;
}

int heap_init(void) {
    uint32_t c = 0;
    heap_head = heap_tail = free_head = 0;
    heap_top = KERNEL_HEAP_BASE;
    heap_total = 0;
    heap_used = 0;
    for (uint32_t i = 0; i <= SMALL_MAX / 16; i++) {
        while (class_size[c] < i * 16) c++;
        class_of[i] = (uint8_t)c;
    }
    for (uint32_t i = 0; i < NCLASSES; i++) class_free[i] = 0;
    heap_ready = 1;
    return heap_grow(PAGE_SIZE_4K);
}

void *kmalloc(size_t size) {
    uint64_t need;

    if (!heap_ready || size == 0) {
        return 0;
    }

    need = align_up((uint64_t)size, HEAP_ALIGN);
    if (need <= SMALL_MAX) {
        uint32_t cls = class_of[need / 16];
        small_hdr_t *h = class_free[cls];
        if (!h) {
            if (refill(cls) != 0) return 0;
            h = class_free[cls];
        }
        class_free[cls] = h->next_free == END_OF_LIST ? 0 : (small_hdr_t *)h->next_free;
        h->next_free = 0;
        heap_used += class_size[cls];
        zero_bytes(h + 1, class_size[cls]);     /* as fresh heap memory was */
        return h + 1;
    }
    {
        void *p = large_alloc(need);
        if (p) zero_bytes(p, need);
        return p;
    }
}

void kfree(void *ptr) {
    uint32_t magic;

    if (!ptr) {
        return;
    }

    magic = ((uint32_t *)ptr)[-1];
    if (magic == MAGIC_SMALL) {
        small_hdr_t *h = (small_hdr_t *)ptr - 1;
        if (h->next_free) return;                                   /* already free */
        heap_used -= class_size[h->cls];
        h->next_free = class_free[h->cls] ? (void *)class_free[h->cls] : END_OF_LIST;
        class_free[h->cls] = h;
        return;
    }
    if (magic == MAGIC_LARGE) large_free((heap_block_t *)ptr - 1);
}

static uint64_t usable_size(void *ptr) {
    uint32_t magic = ((uint32_t *)ptr)[-1];
    if (magic == MAGIC_SMALL) return class_size[((small_hdr_t *)ptr - 1)->cls];
    return ((heap_block_t *)ptr - 1)->size;
}

void *kcalloc(size_t count, size_t size) {
    uint64_t total = (uint64_t)count * (uint64_t)size;
    void *ptr = kmalloc((size_t)total);
    if (!ptr) {
        return 0;
    }
    zero_bytes(ptr, total);
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    void *new_ptr;
    uint64_t have;

    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return 0;
    }

    have = usable_size(ptr);
    if (have >= size) {
        return ptr;
    }

    new_ptr = kmalloc(size);
    if (!new_ptr) {
        return 0;
    }
    copy_bytes(new_ptr, ptr, have);
    kfree(ptr);
    return new_ptr;
}

uint64_t heap_bytes_total(void) {
    return heap_total;
}

uint64_t heap_bytes_used(void) {
    return heap_used;
}
