#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>


#define PAGE_SIZE_4K   0x1000ULL
#define PAGE_SIZE_2M   0x200000ULL
#define PAGE_SIZE_1G   0x40000000ULL


#define PTE_PRESENT    (1ULL << 0)   
#define PTE_WRITE      (1ULL << 1)   
#define PTE_USER       (1ULL << 2)   
#define PTE_WRITE_THRU (1ULL << 3)   
#define PTE_NO_CACHE   (1ULL << 4)   
#define PTE_ACCESSED   (1ULL << 5)   
#define PTE_DIRTY      (1ULL << 6)   
#define PTE_HUGE       (1ULL << 7)   
#define PTE_GLOBAL     (1ULL << 8)   
#define PTE_NX         (1ULL << 63)  


#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_FRAME(e)   ((e) & PTE_ADDR_MASK)


#define VA_PML4_IDX(v)  (((v) >> 39) & 0x1FF)
#define VA_PDPT_IDX(v)  (((v) >> 30) & 0x1FF)
#define VA_PD_IDX(v)    (((v) >> 21) & 0x1FF)
#define VA_PT_IDX(v)    (((v) >> 12) & 0x1FF)
#define VA_OFFSET(v)    ((v) & 0xFFF)


#define CANONICAL(v)    (((v) & (1ULL<<47)) ? ((v) | 0xFFFF000000000000ULL) : ((v) & 0x0000FFFFFFFFFFFFULL))




#define PHYSICAL_BASE   0xFFFF800000000000ULL   
#define KERNEL_VMA      0xFFFFFFFF80000000ULL   


#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + PHYSICAL_BASE))
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - PHYSICAL_BASE)


typedef uint64_t pte_t;
#define PT_ENTRIES  512

typedef struct { pte_t e[PT_ENTRIES]; } __attribute__((aligned(PAGE_SIZE_4K))) page_table_t;


typedef struct {
    uint64_t pml4_phys;
    

    uint64_t mapped_pages;
} addr_space_t;


#define VMM_PRESENT  PTE_PRESENT
#define VMM_WRITE    PTE_WRITE
#define VMM_USER     PTE_USER
#define VMM_HUGE     PTE_HUGE
#define VMM_NX       PTE_NX
#define VMM_GLOBAL   PTE_GLOBAL





#define VMM_WC       (1ULL << 59)


#define VMM_FLAGS_KERNEL_RW  (VMM_PRESENT | VMM_WRITE | VMM_GLOBAL)
#define VMM_FLAGS_KERNEL_RO  (VMM_PRESENT | VMM_GLOBAL)
#define VMM_FLAGS_USER_RW    (VMM_PRESENT | VMM_WRITE | VMM_USER)
#define VMM_FLAGS_USER_RO    (VMM_PRESENT | VMM_USER)


int  vmm_init(uint64_t fb_phys, uint64_t fb_size);


int  vmm_map_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags);


void vmm_unmap_page(addr_space_t *as, uint64_t virt, int free_phys);


int  vmm_map_range(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);


void vmm_unmap_range(addr_space_t *as, uint64_t virt, uint64_t size, int free_phys);


uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt);


int vmm_page_writable(addr_space_t *as, uint64_t virt);


addr_space_t *vmm_create_address_space(void);


void vmm_destroy_address_space(addr_space_t *as);


void vmm_switch_address_space(addr_space_t *as);


addr_space_t *vmm_kernel_address_space(void);



void *vmm_map_physical(uint64_t phys, uint64_t size, uint64_t flags);


static inline void vmm_invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}


static inline void vmm_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}


void vmm_print_stats(addr_space_t *as);

#endif
