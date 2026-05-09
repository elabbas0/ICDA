#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE        4096ULL
#define PAGE_SHIFT       12
#define ADDR_TO_FRAME(a) ((a) >> PAGE_SHIFT)
#define FRAME_TO_ADDR(f) ((f) << PAGE_SHIFT)

void     pmm_init(void *multiboot_info);
uint64_t pmm_alloc();
uint64_t pmm_alloc_contiguous(uint64_t count);
void     pmm_free(uint64_t addr);
void     pmm_free_range(uint64_t addr, uint64_t count);
uint64_t pmm_free_frames();
uint64_t pmm_total_frames();
uint64_t pmm_next_free_frame();
void     pmm_print_stats();

#endif
