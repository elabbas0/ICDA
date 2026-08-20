#include "vmm.h"
#include "pmm.h"
#include "../cpu/multiboot2.h"
#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"

extern uint8_t kernel_end[];

static addr_space_t kernel_as;
static uint64_t hhdm_limit = 0;

// tracks whether the HHDM is live; 0 = use phys addr directly, 1 = use PHYS_TO_VIRT
static int hhdm_ready = 0;

// return a writable pointer to a physical page table page,
// using identity mapping before HHDM is up, PHYS_TO_VIRT after
static inline pte_t *pt_ptr(uint64_t phys) {
    return hhdm_ready ? (pte_t *)PHYS_TO_VIRT(phys) : (pte_t *)phys;
}

// print helpers (no libc available)
static void print_hex64(uint64_t v) {
    console_write_hex64(v, CONSOLE_STYLE_INFO);
}

static void print_dec64(uint64_t v) {
    console_write_dec64(v, CONSOLE_STYLE_INFO);
}

static void print_alloc_failure(const char *where) {
    console_write("VMM alloc failed at ", CONSOLE_STYLE_ERROR);
    console_write(where, CONSOLE_STYLE_ERROR);
    console_write(" free_frames=", CONSOLE_STYLE_ERROR);
    print_dec64(pmm_free_frames());
    console_write(" next_free=", CONSOLE_STYLE_ERROR);
    print_hex64(FRAME_TO_ADDR(pmm_next_free_frame()));
    console_write("\n", CONSOLE_STYLE_ERROR);
}

// allocate and zero a physical page for use as a page table
static uint64_t alloc_page_table(void) {
    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    pte_t *p = pt_ptr(phys);
    for (int i = 0; i < PT_ENTRIES; i++)
        p[i] = 0;
    return phys;
}

// map a single 2 MiB huge page virt -> phys (virt and phys must be 2 MiB aligned)
static int map_huge_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    pte_t *pml4 = pt_ptr(as->pml4_phys);
    uint64_t i4 = VA_PML4_IDX(virt);

    if (!(pml4[i4] & PTE_PRESENT)) {
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("pml4"); return -1; }
        pml4[i4] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    pte_t *pdpt = pt_ptr(PTE_FRAME(pml4[i4]));
    uint64_t i3 = VA_PDPT_IDX(virt);

    if (!(pdpt[i3] & PTE_PRESENT)) {
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("pdpt"); return -1; }
        pdpt[i3] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    pte_t *pd  = pt_ptr(PTE_FRAME(pdpt[i3]));
    uint64_t i2 = VA_PD_IDX(virt);

    // set the huge bit directly in the PD — no PT needed
    pd[i2] = (phys & 0x000FFFFFFFE00000ULL) | flags | PTE_PRESENT | PTE_HUGE;
    vmm_invlpg(virt);
    return 0;
}

// walk the 4-level page table for virt; allocate intermediate tables if alloc=1
// returns a pointer to the final PTE, or NULL if a level is missing and alloc=0
static pte_t *get_pte(addr_space_t *as, uint64_t virt, int alloc) {
    pte_t *pml4 = pt_ptr(as->pml4_phys);
    uint64_t i4 = VA_PML4_IDX(virt);

    if (!(pml4[i4] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("get_pte.pml4"); return NULL; }
        pml4[i4] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    pte_t *pdpt = pt_ptr(PTE_FRAME(pml4[i4]));
    uint64_t i3 = VA_PDPT_IDX(virt);

    if (!(pdpt[i3] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("get_pte.pdpt"); return NULL; }
        pdpt[i3] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    if (pdpt[i3] & PTE_HUGE) return NULL; // 1 GiB page, can't descend

    pte_t *pd  = pt_ptr(PTE_FRAME(pdpt[i3]));
    uint64_t i2 = VA_PD_IDX(virt);

    if (!(pd[i2] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("get_pte.pd"); return NULL; }
        pd[i2] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    if (pd[i2] & PTE_HUGE) return NULL; // 2 MiB page, can't descend

    pte_t *pt = pt_ptr(PTE_FRAME(pd[i2]));
    return &pt[VA_PT_IDX(virt)];
}

int vmm_map_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    virt &= ~0xFFFULL;
    phys &= ~0xFFFULL;
    pte_t *pte = get_pte(as, virt, 1);
    if (!pte) return -1;
    if (!(*pte & PTE_PRESENT) && as && !(flags & VMM_GLOBAL)) {
        as->mapped_pages++;
    }
    *pte = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    vmm_invlpg(virt);
    return 0;
}

void vmm_unmap_page(addr_space_t *as, uint64_t virt, int free_phys) {
    virt &= ~0xFFFULL;
    pte_t *pte = get_pte(as, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    if (free_phys) pmm_free(PTE_FRAME(*pte));
    if (as && !(*pte & PTE_GLOBAL) && as->mapped_pages > 0) {
        as->mapped_pages--;
    }
    *pte = 0;
    vmm_invlpg(virt);
}

int vmm_map_range(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    uint64_t pages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++)
        if (vmm_map_page(as, virt + i * PAGE_SIZE_4K, phys + i * PAGE_SIZE_4K, flags) != 0)
            return -1;
    return 0;
}

void vmm_unmap_range(addr_space_t *as, uint64_t virt, uint64_t size, int free_phys) {
    uint64_t pages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++)
        vmm_unmap_page(as, virt + i * PAGE_SIZE_4K, free_phys);
}

uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt) {
    pte_t *pte = get_pte(as, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return PTE_FRAME(*pte) | VA_OFFSET(virt);
}

void vmm_switch_address_space(addr_space_t *as) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

addr_space_t *vmm_kernel_address_space(void) {
    return &kernel_as;
}

void *vmm_map_physical(uint64_t phys, uint64_t size, uint64_t flags) {
    if (phys == 0 || size == 0) return 0;
    uint64_t aligned_phys = phys & ~0xFFFULL;
    uint64_t page_offset = phys & 0xFFFULL;
    uint64_t end = (phys + size + PAGE_SIZE_4K - 1) & ~0xFFFULL;

    if (end <= hhdm_limit) {
        return PHYS_TO_VIRT(phys);
    }

    for (uint64_t p = aligned_phys; p < end; p += PAGE_SIZE_4K) {
        uint64_t virt = PHYSICAL_BASE + p;
        if (vmm_virt_to_phys(&kernel_as, virt) == p) {
            continue;
        }
        if (vmm_map_page(&kernel_as, virt, p, flags | VMM_GLOBAL) != 0) {
            return 0;
        }
    }

    return (void *)(uintptr_t)(PHYSICAL_BASE + aligned_phys + page_offset);
}

addr_space_t *vmm_create_address_space(void) {
    // descriptor page and PML4 are separate so the PML4 stays page-aligned
    uint64_t desc_phys = pmm_alloc();
    if (!desc_phys) return NULL;

    uint64_t pml4_phys = alloc_page_table();
    if (!pml4_phys) { pmm_free(desc_phys); return NULL; }

    addr_space_t *as = (addr_space_t *)PHYS_TO_VIRT(desc_phys);
    as->pml4_phys = pml4_phys;

    // The kernel still executes from the low bootstrap mapping while it also
    // maintains higher-half aliases. Keep PML4[0] as a supervisor-only shared
    // mapping so interrupt/syscall entry can fetch kernel code after a ring
    // transition, then inherit the higher-half kernel mappings too.
    pte_t *new_pml4 = (pte_t *)PHYS_TO_VIRT(pml4_phys);
    pte_t *ker_pml4 = (pte_t *)PHYS_TO_VIRT(kernel_as.pml4_phys);
    new_pml4[0] = ker_pml4[0];
    for (int i = 256; i < 512; i++)
        new_pml4[i] = ker_pml4[i];

    return as;
}

// free all PT pages under a PD (user half only)
static void free_pt_range(pte_t *pd) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        if (pd[i] & PTE_HUGE) { pd[i] = 0; continue; }
        pmm_free(PTE_FRAME(pd[i]));
        pd[i] = 0;
    }
}

void vmm_destroy_address_space(addr_space_t *as) {
    if (!as || as == &kernel_as) return;

    pte_t *pml4 = (pte_t *)PHYS_TO_VIRT(as->pml4_phys);

    // walk user half only (entries 0-255)
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        pte_t *pdpt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pml4[i4]));

        for (int i3 = 0; i3 < PT_ENTRIES; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            if (pdpt[i3] & PTE_HUGE) { pdpt[i3] = 0; continue; }
            pte_t *pd = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pdpt[i3]));
            free_pt_range(pd);
            pmm_free(PTE_FRAME(pdpt[i3]));
            pdpt[i3] = 0;
        }

        pmm_free(PTE_FRAME(pml4[i4]));
        pml4[i4] = 0;
    }

    uint64_t desc_phys = VIRT_TO_PHYS((uint64_t)as);
    pmm_free(as->pml4_phys);
    pmm_free(desc_phys);
}

int vmm_init(uint64_t fb_phys, uint64_t fb_size) {
    hhdm_ready = 0;

    uint64_t pml4_phys = alloc_page_table();
    if (!pml4_phys) {
        print_alloc_failure("kernel_pml4");
        return -1;
    }
    kernel_as.pml4_phys = pml4_phys;

    uint64_t limit = pmm_total_frames() * PAGE_SIZE_4K;
    if (limit > 512ULL * 1024 * 1024) limit = 512ULL * 1024 * 1024;
    limit = (limit + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
    hhdm_limit = limit;

    for (uint64_t off = 0; off < limit; off += PAGE_SIZE_2M)
        if (map_huge_page(&kernel_as, off, off, VMM_WRITE | VMM_GLOBAL) != 0)
            return -1;

    for (uint64_t off = 0; off < limit; off += PAGE_SIZE_2M)
        if (map_huge_page(&kernel_as, PHYSICAL_BASE + off, off, VMM_WRITE | VMM_GLOBAL) != 0)
            return -1;

    if (fb_phys && fb_size) {
        uint64_t fb_start = fb_phys & ~(PAGE_SIZE_2M - 1);
        uint64_t fb_end   = (fb_phys + fb_size + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
        for (uint64_t off = fb_start; off < fb_end; off += PAGE_SIZE_2M)
            if (map_huge_page(&kernel_as, PHYSICAL_BASE + off, off,
                              VMM_WRITE | VMM_GLOBAL | PTE_NO_CACHE | PTE_WRITE_THRU) != 0)
                return -1;
    }

    uint64_t kstart = 0x100000ULL;
    uint64_t ksize  = (uint64_t)kernel_end - kstart;
    for (uint64_t off = 0; off < ksize + PAGE_SIZE_4K; off += PAGE_SIZE_4K)
        if (vmm_map_page(&kernel_as, KERNEL_VMA + off, kstart + off, VMM_FLAGS_KERNEL_RW) != 0)
            return -1;

    // enable PSE (bit 4) and PGE (bit 7) in CR4 before loading CR3
    // PSE is required for 2 MiB huge pages; PGE enables the global page flag
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 4) | (1ULL << 7);  // PSE | PGE
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    vmm_switch_address_space(&kernel_as);
    hhdm_ready = 1;

    // The framebuffer pointer still points at a physical address here.
    // Move it onto the HHDM before any further printing in the new CR3.
    fb_remap(PHYSICAL_BASE);
    return 0;
}
