/* Host stub: "physical" addresses are host pointers. */
#include <stdint.h>
#define PAGE_SIZE 4096ULL
uint64_t pmm_alloc_contiguous_below(uint64_t count, uint64_t max_addr);
uint64_t pmm_alloc_contiguous_aligned_below(uint64_t count, uint64_t align, uint64_t max_addr);
void pmm_free_range(uint64_t addr, uint64_t count);
