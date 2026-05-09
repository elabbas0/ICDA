#include "heap.h"

#include "pmm.h"
#include "vmm.h"

#define KERNEL_HEAP_BASE  0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_LIMIT 0xFFFFFFFF98000000ULL
#define HEAP_GROW_PAGES   4ULL
#define HEAP_ALIGN        16ULL

typedef struct heap_block {
    uint64_t size;
    struct heap_block *next;
    struct heap_block *prev;
    uint8_t free;
} heap_block_t;

static heap_block_t *heap_head = 0;
static uint64_t heap_top = KERNEL_HEAP_BASE;
static uint64_t heap_total = 0;
static uint64_t heap_used = 0;
static int heap_ready = 0;

static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void zero_bytes(void *ptr, uint64_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static heap_block_t *block_from_ptr(void *ptr) {
    return (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
}

static void split_block(heap_block_t *block, uint64_t size) {
    uint64_t remaining = block->size - size;
    heap_block_t *next;

    if (remaining <= sizeof(heap_block_t) + HEAP_ALIGN) {
        return;
    }

    next = (heap_block_t *)((uint8_t *)(block + 1) + size);
    next->size = remaining - sizeof(heap_block_t);
    next->next = block->next;
    next->prev = block;
    next->free = 1;

    if (next->next) {
        next->next->prev = next;
    }

    block->next = next;
    block->size = size;
}

static void coalesce_block(heap_block_t *block) {
    if (block->next && block->next->free) {
        heap_block_t *next = block->next;
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    if (block->prev && block->prev->free) {
        heap_block_t *prev = block->prev;
        prev->size += sizeof(heap_block_t) + block->size;
        prev->next = block->next;
        if (prev->next) {
            prev->next->prev = prev;
        }
    }
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
        zero_bytes((void *)(uintptr_t)heap_top, PAGE_SIZE_4K);
        heap_top += PAGE_SIZE_4K;
    }

    block = (heap_block_t *)(uintptr_t)base;
    block->size = grow_size - sizeof(heap_block_t);
    block->next = 0;
    block->prev = 0;
    block->free = 1;

    if (!heap_head) {
        heap_head = block;
    } else {
        heap_block_t *tail = heap_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = block;
        block->prev = tail;
        if (tail->free) {
            tail->size += sizeof(heap_block_t) + block->size;
            tail->next = 0;
        }
    }

    heap_total += grow_size;
    return 0;
}

int heap_init(void) {
    heap_head = 0;
    heap_top = KERNEL_HEAP_BASE;
    heap_total = 0;
    heap_used = 0;
    heap_ready = 1;
    return heap_grow(PAGE_SIZE_4K);
}

void *kmalloc(size_t size) {
    heap_block_t *block;
    uint64_t need;

    if (!heap_ready || size == 0) {
        return 0;
    }

    need = align_up((uint64_t)size, HEAP_ALIGN);

retry:
    block = heap_head;
    while (block) {
        if (block->free && block->size >= need) {
            split_block(block, need);
            block->free = 0;
            heap_used += block->size;
            return block + 1;
        }
        block = block->next;
    }

    if (heap_grow(need) != 0) {
        return 0;
    }
    goto retry;
}

void kfree(void *ptr) {
    heap_block_t *block;

    if (!ptr) {
        return;
    }

    block = block_from_ptr(ptr);
    if (block->free) {
        return;
    }

    block->free = 1;
    heap_used -= block->size;
    coalesce_block(block);
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
    heap_block_t *block;
    void *new_ptr;
    uint64_t need;

    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return 0;
    }

    block = block_from_ptr(ptr);
    need = align_up((uint64_t)size, HEAP_ALIGN);
    if (block->size >= need) {
        split_block(block, need);
        return ptr;
    }

    new_ptr = kmalloc(size);
    if (!new_ptr) {
        return 0;
    }
    copy_bytes(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

uint64_t heap_bytes_total(void) {
    return heap_total;
}

uint64_t heap_bytes_used(void) {
    return heap_used;
}
