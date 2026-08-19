#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

// page sizes
#define PAGE_SIZE_4K   0x1000ULL
#define PAGE_SIZE_2M   0x200000ULL
#define PAGE_SIZE_1G   0x40000000ULL

// x86-64 page table entry flags
#define PTE_PRESENT    (1ULL << 0)   // page is present
#define PTE_WRITE      (1ULL << 1)   // read/write (if 0, read-only)
#define PTE_USER       (1ULL << 2)   // user-accessible (ring 3)
#define PTE_WRITE_THRU (1ULL << 3)   // write-through caching
#define PTE_NO_CACHE   (1ULL << 4)   // disable caching
#define PTE_ACCESSED   (1ULL << 5)   // set by cpu on access
#define PTE_DIRTY      (1ULL << 6)   // set by cpu on write
#define PTE_HUGE       (1ULL << 7)   // 2 MiB (in PD) or 1 GiB (in PDPT)
#define PTE_GLOBAL     (1ULL << 8)   // not flushed on CR3 switch
#define PTE_NX         (1ULL << 63)  // no-execute (requires EFER.NXE)

// extract the physical address from an entry
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_FRAME(e)   ((e) & PTE_ADDR_MASK)

// virtual address index decomposition (9-9-9-9-12)
#define VA_PML4_IDX(v)  (((v) >> 39) & 0x1FF)
#define VA_PDPT_IDX(v)  (((v) >> 30) & 0x1FF)
#define VA_PD_IDX(v)    (((v) >> 21) & 0x1FF)
#define VA_PT_IDX(v)    (((v) >> 12) & 0x1FF)
#define VA_OFFSET(v)    ((v) & 0xFFF)

// bits [63:48] must equal bit 47 on x86-64
#define CANONICAL(v)    (((v) & (1ULL<<47)) ? ((v) | 0xFFFF000000000000ULL) : ((v) & 0x0000FFFFFFFFFFFFULL))

// virtual memory layout
//   0x0000000000000000 - 0x00007FFFFFFFFFFF : user space  (128 TiB)
//   0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF : kernel space (128 TiB)
#define PHYSICAL_BASE   0xFFFF800000000000ULL   // higher-half direct map
#define KERNEL_VMA      0xFFFFFFFF80000000ULL   // kernel image (-2 GiB)

// convert physical <-> virtual via the direct map
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + PHYSICAL_BASE))
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - PHYSICAL_BASE)

// 64-bit page table entry; 512 entries per table = 4 KiB
typedef uint64_t pte_t;
#define PT_ENTRIES  512

typedef struct { pte_t e[PT_ENTRIES]; } __attribute__((aligned(PAGE_SIZE_4K))) page_table_t;

// address space identified by its PML4 physical address
typedef struct {
    uint64_t pml4_phys;
    /* Number of 4 KiB pages mapped (tracked for task-manager memory
     * accounting; only increments for newly allocated mappings). */
    uint64_t mapped_pages;
} addr_space_t;

// public flag aliases
#define VMM_PRESENT  PTE_PRESENT
#define VMM_WRITE    PTE_WRITE
#define VMM_USER     PTE_USER
#define VMM_HUGE     PTE_HUGE
#define VMM_NX       PTE_NX
#define VMM_GLOBAL   PTE_GLOBAL

// common flag combinations
#define VMM_FLAGS_KERNEL_RW  (VMM_PRESENT | VMM_WRITE | VMM_GLOBAL)
#define VMM_FLAGS_KERNEL_RO  (VMM_PRESENT | VMM_GLOBAL)
#define VMM_FLAGS_USER_RW    (VMM_PRESENT | VMM_WRITE | VMM_USER)
#define VMM_FLAGS_USER_RO    (VMM_PRESENT | VMM_USER)

// must be called after pmm_init(); fb_phys/fb_size map the framebuffer into the HHDM
int  vmm_init(uint64_t fb_phys, uint64_t fb_size);

// map a single 4 KiB page virt -> phys; returns 0 on success, -1 on failure
int  vmm_map_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags);

// unmap a single page; free_phys=1 releases the physical frame to the PMM
void vmm_unmap_page(addr_space_t *as, uint64_t virt, int free_phys);

// map a contiguous range; size is rounded up to PAGE_SIZE_4K
int  vmm_map_range(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

// unmap a contiguous range
void vmm_unmap_range(addr_space_t *as, uint64_t virt, uint64_t size, int free_phys);

// walk page tables and return the physical address for virt, or 0 if unmapped
uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt);

// allocate a new user address space with kernel mappings inherited; NULL on fail
addr_space_t *vmm_create_address_space(void);

// free all user-space page table pages; kernel mappings are left intact
void vmm_destroy_address_space(addr_space_t *as);

// load an address space into CR3
void vmm_switch_address_space(addr_space_t *as);

// return the kernel address space
addr_space_t *vmm_kernel_address_space(void);

// make a physical range accessible through the higher-half direct map.
// returns the virtual base address for the requested physical address.
void *vmm_map_physical(uint64_t phys, uint64_t size, uint64_t flags);

// invalidate a single TLB entry
static inline void vmm_invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// flush the entire TLB by reloading CR3
static inline void vmm_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// print mapped page count and PMM free frames
void vmm_print_stats(addr_space_t *as);

#endif
