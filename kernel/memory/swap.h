#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include "vmm.h"
#include "../drivers/storage/block.h"

/* A user page written out to the swap area leaves a page table entry that
 * is not present, carries PTE_SWAP, holds the page's slot in the address
 * bits, and keeps the page's write, user, no-execute and copy-on-write bits
 * for when it is read back. */
#define PTE_SWAP       (1ULL << 11)
#define PTE_SWAP_KEEP  (PTE_WRITE | PTE_USER | VMM_COW | PTE_NX)

static inline int pte_is_swap(pte_t e) {
    return !(e & PTE_PRESENT) && (e & PTE_SWAP);
}

static inline uint64_t pte_swap_slot(pte_t e) {
    return (e & PTE_ADDR_MASK) >> 12;
}

/* the swap entry for slot, keeping what of the page's entry it keeps */
static inline pte_t pte_swap_make(uint64_t slot, pte_t from) {
    return (slot << 12) | (from & PTE_SWAP_KEEP) | PTE_SWAP;
}

/* one run of the swap area: count pages from sector lba on */
typedef struct {
    uint64_t lba;
    uint64_t pages;
} swap_run_t;

#define SWAP_MAX_RUNS 64

/* Swaps to these sectors of dev (what: how to name it in the log).  0, or
 * -1 when it cannot (another area is in use, no memory for the slot table). */
int  swap_on(block_device_t *dev, const swap_run_t *runs, int nruns, const char *what);

/* the swap area is on dev: its disk is not formatted or repartitioned meanwhile */
int swap_uses_device(const block_device_t *dev);
uint64_t swap_total_pages(void);
uint64_t swap_free_pages(void);
/* pages written out and read back since boot */
void swap_counts(uint64_t *outs, uint64_t *ins);

/* Writes out up to want pages programs have not used lately, and frees
 * their memory: the frame allocator calls it when memory runs out (urgent)
 * or below its reserve.  Does nothing in an interrupt, without the kernel
 * lock, or while it is already at work, nor for the window manager unless
 * urgent.  Returns the pages freed. */
uint64_t swap_out(uint64_t want, int urgent);

/* The entry *pte (at va in as) holds a swapped page: reads it back and maps
 * it.  1 done, 0 *pte is no swap entry, -1 no memory for it, -2 the disk
 * failed. */
int  swap_in(addr_space_t *as, uint64_t va, pte_t *pte);

/* an entry holding a slot is gone (unmapped, its address space ended) /
 * copied (fork: both now hold the slot) */
void swap_entry_free(addr_space_t *as, pte_t e);
void swap_entry_dup(addr_space_t *as, pte_t e);

#endif
