#include "vmm.h"
#include "pmm.h"
#include "swap.h"
#include "../cpu/multiboot2.h"
#include "../cpu/pat.h"
#include "../cpu/smp.h"
#include "../drivers/console/console.h"
#include "../drivers/serial/serial.h"
#include "../drivers/display/framebuffer.h"

extern uint8_t kernel_end[];

static addr_space_t kernel_as;
static uint64_t hhdm_limit = 0;


static int hhdm_ready = 0;



static inline pte_t *pt_ptr(uint64_t phys) {
    return hhdm_ready ? (pte_t *)PHYS_TO_VIRT(phys) : (pte_t *)phys;
}


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


static uint64_t alloc_page_table(void) {
    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    pte_t *p = pt_ptr(phys);
    for (int i = 0; i < PT_ENTRIES; i++)
        p[i] = 0;
    return phys;
}


static int map_huge_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    


    if (flags & VMM_WC) {
        flags &= ~(VMM_WC | PTE_NO_CACHE | PTE_WRITE_THRU);
        flags |= (1ULL << 12);  
    }

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

    
    pd[i2] = (phys & 0x000FFFFFFFE00000ULL) | flags | PTE_PRESENT | PTE_HUGE;
    vmm_invlpg(virt);
    return 0;
}



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
    if (pdpt[i3] & PTE_HUGE) return NULL; 

    pte_t *pd  = pt_ptr(PTE_FRAME(pdpt[i3]));
    uint64_t i2 = VA_PD_IDX(virt);

    if (!(pd[i2] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        uint64_t p = alloc_page_table();
        if (!p) { print_alloc_failure("get_pte.pd"); return NULL; }
        pd[i2] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    if ((pd[i2] & PTE_HUGE) && alloc && as != &kernel_as) {
        uint64_t p = alloc_page_table();
        uint64_t cr3;
        if (!p) { print_alloc_failure("get_pte.split"); return NULL; }
        pd[i2] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    if (pd[i2] & PTE_HUGE) return NULL; 

    pte_t *pt = pt_ptr(PTE_FRAME(pd[i2]));
    return &pt[VA_PT_IDX(virt)];
}

int vmm_map_page(addr_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    


    if (flags & VMM_WC) {
        flags &= ~(VMM_WC | PTE_NO_CACHE | PTE_WRITE_THRU);
        flags |= (1ULL << 7);  
    }

    virt &= ~0xFFFULL;
    phys &= ~0xFFFULL;
    pte_t *pte = get_pte(as, virt, 1);
    if (!pte) return -1;
    if (pte_is_swap(*pte)) {
        swap_entry_free(as, *pte);              /* the new page replaces the swapped one */
    } else if (!(*pte & PTE_PRESENT) && as && !(flags & VMM_GLOBAL)) {
        as->mapped_pages++;
    }
    {
        pte_t old = *pte, nv = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
        *pte = nv;
        vmm_invlpg(virt);
        if ((old & PTE_PRESENT) && old != nv && virt < 0x0000800000000000ULL) tlb_shootdown(as);
    }
    return 0;
}
void vmm_unmap_page(addr_space_t *as, uint64_t virt, int free_phys) {
    virt &= ~0xFFFULL;
    pte_t *pte = get_pte(as, virt, 0);
    if (pte && pte_is_swap(*pte)) {
        swap_entry_free(as, *pte);
        *pte = 0;
        if (as && as->mapped_pages > 0) as->mapped_pages--;
        return;
    }
    if (!pte || !(*pte & PTE_PRESENT)) return;
    if (free_phys) pmm_free(PTE_FRAME(*pte));
    if (as && !(*pte & PTE_GLOBAL) && as->mapped_pages > 0) {
        as->mapped_pages--;
    }
    *pte = 0;
    vmm_invlpg(virt);
    tlb_shootdown(as);
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

/* A user page written to swap is read back: whoever asks for the page's
 * frame is about to use it. */
uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt) {
    pte_t *pte = get_pte(as, virt, 0);
    if (pte && pte_is_swap(*pte) && virt < 0x0000800000000000ULL && swap_in(as, virt, pte) < 0) return 0;
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return PTE_FRAME(*pte) | VA_OFFSET(virt);
}

/* there is a page at virt, in memory or written to swap (reads nothing back) */
int vmm_has_page(addr_space_t *as, uint64_t virt) {
    pte_t *pte = get_pte(as, virt & ~0xFFFULL, 0);
    return pte && ((*pte & PTE_PRESENT) || pte_is_swap(*pte));
}

/* the page at virt is in memory (mincore) */
int vmm_resident(addr_space_t *as, uint64_t virt) {
    pte_t *pte = get_pte(as, virt & ~0xFFFULL, 0);
    return pte && (*pte & PTE_PRESENT);
}

/* a fault at virt on a page written to swap: 1 it is back, 0 it was not
 * swapped, -1 no memory to read it into, -2 the disk failed */
int vmm_swap_fault(addr_space_t *as, uint64_t virt) {
    pte_t *pte;
    if (!as || virt >= 0x0000800000000000ULL) return 0;
    pte = get_pte(as, virt & ~0xFFFULL, 0);
    if (!pte || !pte_is_swap(*pte)) return 0;
    return swap_in(as, virt & ~0xFFFULL, pte);
}

/* puts a swap entry at virt (a Linux program's mremap moving its pages) */
int vmm_set_swap_entry(addr_space_t *as, uint64_t virt, pte_t e) {
    pte_t *pte = get_pte(as, virt & ~0xFFFULL, 1);
    if (!pte || !pte_is_swap(e)) return -1;
    if (pte_is_swap(*pte)) swap_entry_free(as, *pte);
    else if (*pte & PTE_PRESENT) return -1;
    else as->mapped_pages++;
    *pte = e;
    as->swapped_pages++;
    return 0;
}






int vmm_page_writable(addr_space_t *as, uint64_t virt) {
    pte_t *pte;

    if (!as || virt >= 0x0000800000000000ULL) {
        return 0;
    }
    pte = get_pte(as, virt & ~0xFFFULL, 0);
    if (pte && pte_is_swap(*pte) && swap_in(as, virt & ~0xFFFULL, pte) < 0) return 0;
    if (!pte || !(*pte & PTE_PRESENT)) {
        return 0;
    }
    return (*pte & PTE_WRITE) ? 1 : 0;
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
    if (end < phys) return 0; 

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
    
    uint64_t desc_phys = pmm_alloc();
    if (!desc_phys) return NULL;

    uint64_t pml4_phys = alloc_page_table();
    if (!pml4_phys) { pmm_free(desc_phys); return NULL; }

    addr_space_t *as = (addr_space_t *)PHYS_TO_VIRT(desc_phys);
    as->pml4_phys = pml4_phys;

    
    
    
    
    pte_t *new_pml4 = (pte_t *)PHYS_TO_VIRT(pml4_phys);
    pte_t *ker_pml4 = (pte_t *)PHYS_TO_VIRT(kernel_as.pml4_phys);
    if (ker_pml4[0] & PTE_PRESENT) {
        pte_t *kpdpt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(ker_pml4[0]));
        uint64_t pdpt_phys = alloc_page_table();
        pte_t *pdpt;
        if (!pdpt_phys) { pmm_free(pml4_phys); pmm_free(desc_phys); return NULL; }
        pdpt = (pte_t *)PHYS_TO_VIRT(pdpt_phys);
        for (int i = 0; i < PT_ENTRIES; i++) {
            pte_t *kpd, *pd;
            uint64_t pd_phys;
            if (!(kpdpt[i] & PTE_PRESENT) || (kpdpt[i] & PTE_HUGE)) continue;
            kpd = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(kpdpt[i]));
            pd_phys = alloc_page_table();
            if (!pd_phys) continue;
            pd = (pte_t *)PHYS_TO_VIRT(pd_phys);
            for (int j = 0; j < PT_ENTRIES; j++) {
                if (kpd[j] & PTE_HUGE) pd[j] = kpd[j];
            }
            pdpt[i] = pd_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        new_pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    for (int i = 256; i < 512; i++)
        new_pml4[i] = ker_pml4[i];

    return as;
}


static void free_user_pd(pte_t *pd) {
    for (int i2 = 0; i2 < PT_ENTRIES; i2++) {
        pte_t *pt;
        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) continue;
        pt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pd[i2]));
        for (int i1 = 0; i1 < PT_ENTRIES; i1++) {
            if ((pt[i1] & PTE_PRESENT) && (pt[i1] & PTE_USER) && !(pt[i1] & VMM_NOFREE)) {
                pmm_free(PTE_FRAME(pt[i1]));
            } else if (pte_is_swap(pt[i1])) {
                swap_entry_free(0, pt[i1]);         /* the space is going */
            }
        }
        pmm_free(PTE_FRAME(pd[i2]));
        pd[i2] = 0;
    }
}

/* Frees every private table in the lower half and every user page that is
 * not marked VMM_NOFREE (shared memory, framebuffer).  Huge entries are the
 * kernel's identity map and are left alone.  The caller must not be running
 * on this address space. */
void vmm_destroy_address_space(addr_space_t *as) {
    pte_t *pml4;
    if (!as || as == &kernel_as) return;
    pml4 = (pte_t *)PHYS_TO_VIRT(as->pml4_phys);
    for (int i4 = 0; i4 < 256; i4++) {
        pte_t *pdpt;
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        pdpt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pml4[i4]));
        for (int i3 = 0; i3 < PT_ENTRIES; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) continue;
            free_user_pd((pte_t *)PHYS_TO_VIRT(PTE_FRAME(pdpt[i3])));
            pmm_free(PTE_FRAME(pdpt[i3]));
            pdpt[i3] = 0;
        }
        pmm_free(PTE_FRAME(pml4[i4]));
        pml4[i4] = 0;
    }
    pmm_free(as->pml4_phys);
    pmm_free(VIRT_TO_PHYS((uint64_t)as));
}

/* Copies every user page of src into a fresh address space.  Pages marked
 * VMM_NOFREE are shared, not copied. */
addr_space_t *vmm_clone_user(addr_space_t *src) {
    addr_space_t *dst = vmm_create_address_space();
    pte_t *pml4;
    if (!dst || !src) return dst;
    pml4 = (pte_t *)PHYS_TO_VIRT(src->pml4_phys);
    for (uint64_t i4 = 0; i4 < 256; i4++) {
        pte_t *pdpt;
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        pdpt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pml4[i4]));
        for (uint64_t i3 = 0; i3 < PT_ENTRIES; i3++) {
            pte_t *pd;
            if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) continue;
            pd = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pdpt[i3]));
            for (uint64_t i2 = 0; i2 < PT_ENTRIES; i2++) {
                pte_t *pt;
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) continue;
                pt = (pte_t *)PHYS_TO_VIRT(PTE_FRAME(pd[i2]));
                for (uint64_t i1 = 0; i1 < PT_ENTRIES; i1++) {
                    pte_t e = pt[i1];
                    uint64_t va = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    uint64_t flags = e & ~PTE_ADDR_MASK & ~(PTE_ACCESSED | PTE_DIRTY);
                    uint64_t phys;
                    if (pte_is_swap(e)) {
                        /* both hold the slot; each reads its own copy back */
                        pte_t *cp = get_pte(dst, va, 1);
                        if (!cp) goto fail;
                        swap_entry_dup(dst, e);
                        *cp = e;
                        dst->mapped_pages++;
                        continue;
                    }
                    if (!(e & PTE_PRESENT) || !(e & PTE_USER)) continue;
                    phys = PTE_FRAME(e);
                    if (!(e & VMM_NOFREE)) {
                        /* Share the frame; writable pages become copy-on-write in both. */
                        if (e & (PTE_WRITE | VMM_COW)) {
                            flags = (flags & ~PTE_WRITE) | VMM_COW;
                            pt[i1] = (e & ~PTE_WRITE) | VMM_COW;
                        }
                        pmm_ref(phys);
                    }
                    if (vmm_map_page(dst, va, phys, flags) != 0) {
                        if (!(e & VMM_NOFREE)) pmm_free(phys);
                        goto fail;
                    }
                }
            }
        }
    }
    {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    tlb_shootdown(src);                    /* its writable pages are copy-on-write now */
    return dst;
fail:
    vmm_destroy_address_space(dst);
    return NULL;
}

/* Resolves a write to a copy-on-write page: the last owner just regains
 * write access, otherwise the page is copied. */
int vmm_cow_break(addr_space_t *as, uint64_t virt) {
    pte_t *pte = get_pte(as, virt & ~0xFFFULL, 0);
    uint64_t frame, flags;
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & VMM_COW)) return -1;
    frame = PTE_FRAME(*pte);
    flags = ((*pte & ~PTE_ADDR_MASK) & ~(VMM_COW | PTE_ACCESSED | PTE_DIRTY)) | PTE_WRITE;
    if (pmm_refcount(frame) > 1) {
        uint64_t copy = pmm_alloc();
        const uint64_t *s = (const uint64_t *)PHYS_TO_VIRT(frame);
        uint64_t *d;
        if (!copy) return -1;
        d = (uint64_t *)PHYS_TO_VIRT(copy);
        for (int k = 0; k < (int)(PAGE_SIZE_4K / 8); k++) d[k] = s[k];
        pmm_free(frame);
        frame = copy;
    }
    *pte = frame | flags;
    vmm_invlpg(virt & ~0xFFFULL);
    tlb_shootdown(as);
    return 0;
}


int vmm_init(uint64_t fb_phys, uint64_t fb_size) {
    hhdm_ready = 0;

    uint64_t pml4_phys = alloc_page_table();
    if (!pml4_phys) {
        print_alloc_failure("kernel_pml4");
        return -1;
    }
    kernel_as.pml4_phys = pml4_phys;

    uint64_t total_phys = pmm_total_frames() * PAGE_SIZE_4K;
    total_phys = (total_phys + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);

    





    uint64_t identity_limit = total_phys;
    if (identity_limit > 512ULL * 1024 * 1024) identity_limit = 512ULL * 1024 * 1024;
    identity_limit = (identity_limit + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);

    





    hhdm_limit = total_phys;

    for (uint64_t off = 0; off < identity_limit; off += PAGE_SIZE_2M)
        if (map_huge_page(&kernel_as, off, off, VMM_WRITE) != 0)
            return -1;

    for (uint64_t off = 0; off < hhdm_limit; off += PAGE_SIZE_2M)
        if (map_huge_page(&kernel_as, PHYSICAL_BASE + off, off, VMM_WRITE | VMM_GLOBAL) != 0)
            return -1;

    if (fb_phys && fb_size) {
        uint64_t fb_start = fb_phys & ~(PAGE_SIZE_2M - 1);
        uint64_t fb_end   = (fb_phys + fb_size + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
        



        uint64_t fb_flags = pat_wc_available()
            ? (VMM_WRITE | VMM_GLOBAL | VMM_WC)
            : (VMM_WRITE | VMM_GLOBAL | PTE_NO_CACHE | PTE_WRITE_THRU);
        for (uint64_t off = fb_start; off < fb_end; off += PAGE_SIZE_2M) {
            






            if (off < hhdm_limit)
                continue;
            if (map_huge_page(&kernel_as, PHYSICAL_BASE + off, off, fb_flags) != 0)
                return -1;
        }
    }

    uint64_t kstart = 0x100000ULL;
    uint64_t ksize  = (uint64_t)kernel_end - KERNEL_VMA - kstart;
    for (uint64_t off = 0; off < ksize + PAGE_SIZE_4K; off += PAGE_SIZE_4K)
        if (vmm_map_page(&kernel_as, KERNEL_VMA + kstart + off, kstart + off, VMM_FLAGS_KERNEL_RW) != 0)
            return -1;

    
    
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 4) | (1ULL << 7);  
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    vmm_switch_address_space(&kernel_as);
    hhdm_ready = 1;

    
    
    fb_remap(PHYSICAL_BASE);
    return 0;
}

/* Calls fn for each present 4 KB user page in [start, end), skipping empty
 * table levels (sparse reservations of gigabytes cost nothing).  fn gets the
 * page's entry and may change or clear it; the TLB entry is flushed after. */
void vmm_walk_user(addr_space_t *as, uint64_t start, uint64_t end,
                   void (*fn)(addr_space_t *as, uint64_t va, pte_t *pte, void *ctx), void *ctx) {
    pte_t *pml4;
    int visited = 0;
    uint64_t va = start & ~0xFFFULL;
    if (end > 0x0000800000000000ULL) end = 0x0000800000000000ULL;
    if (!as) return;
    pml4 = pt_ptr(as->pml4_phys);
    while (va < end) {
        uint64_t i4 = VA_PML4_IDX(va), i3, i2, i1;
        pte_t *pdpt, *pd, *pt;
        if (!(pml4[i4] & PTE_PRESENT)) { va = (va | ((1ULL << 39) - 1)) + 1; continue; }
        pdpt = pt_ptr(PTE_FRAME(pml4[i4]));
        i3 = VA_PDPT_IDX(va);
        if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) { va = (va | ((1ULL << 30) - 1)) + 1; continue; }
        pd = pt_ptr(PTE_FRAME(pdpt[i3]));
        i2 = VA_PD_IDX(va);
        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) { va = (va | ((1ULL << 21) - 1)) + 1; continue; }
        pt = pt_ptr(PTE_FRAME(pd[i2]));
        for (i1 = VA_PT_IDX(va); i1 < 512 && va < end; i1++, va += PAGE_SIZE_4K) {
            if (!(pt[i1] & PTE_PRESENT) && !pte_is_swap(pt[i1])) continue;
            visited = 1;
            fn(as, va, &pt[i1], ctx);
            vmm_invlpg(va);
        }
    }
    if (visited) tlb_shootdown(as);            /* other CPUs running this space */
}

/* Pages present in [start, end) of a user space.  Only reads the tables: no
 * TLB entry is dropped (vmm_walk_user is for walks that change them), so
 * /proc can count a process's pages as often as it is asked. */
uint64_t vmm_count_present(addr_space_t *as, uint64_t start, uint64_t end) {
    pte_t *pml4;
    uint64_t va = start & ~0xFFFULL, n = 0;
    if (end > 0x0000800000000000ULL) end = 0x0000800000000000ULL;
    if (!as) return 0;
    pml4 = pt_ptr(as->pml4_phys);
    while (va < end) {
        uint64_t i4 = VA_PML4_IDX(va), i3, i2, i1;
        pte_t *pdpt, *pd, *pt;
        if (!(pml4[i4] & PTE_PRESENT)) { va = (va | ((1ULL << 39) - 1)) + 1; continue; }
        pdpt = pt_ptr(PTE_FRAME(pml4[i4]));
        i3 = VA_PDPT_IDX(va);
        if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) { va = (va | ((1ULL << 30) - 1)) + 1; continue; }
        pd = pt_ptr(PTE_FRAME(pdpt[i3]));
        i2 = VA_PD_IDX(va);
        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) { va = (va | ((1ULL << 21) - 1)) + 1; continue; }
        pt = pt_ptr(PTE_FRAME(pd[i2]));
        for (i1 = VA_PT_IDX(va); i1 < 512 && va < end; i1++, va += PAGE_SIZE_4K)
            if (pt[i1] & PTE_PRESENT) n++;
    }
    return n;
}

/* user pages in memory: mapped less those in swap.
 *
 * The counter is checked against the machine: a web process on ICDA was once
 * seen at 3.4 million gigabytes resident (a corrupted counter, cause not yet
 * found), and WebKit, which measures its page processes by this figure
 * (/proc/self/statm), ended the page as "using too much memory".  A count
 * past the memory the machine has in use is recounted from the page tables,
 * repaired, and reported on the serial port with what it was. */
uint64_t vmm_resident_pages(addr_space_t *as) {
    uint64_t resident;
    if (!as) return 0;
    resident = as->mapped_pages > as->swapped_pages ? as->mapped_pages - as->swapped_pages : 0;
    /* no process has more pages in memory than the machine has in use: a
     * count past that has drifted (Activity showed a process at 7.9 GB) */
    if (resident > pmm_usable_frames() - pmm_free_frames()) {
        static int reported;
        uint64_t present = vmm_count_present(as, 0, 0x0000800000000000ULL);
        if (present != resident && reported < 20) {
            char buf[200];
            int n = 0;
            const char *s = "[vmm] resident count was ";
            uint64_t vals[3] = { as->mapped_pages, as->swapped_pages, present };
            reported++;
            while (*s) buf[n++] = *s++;
            for (int k = 0; k < 3; k++) {
                char t[24];
                int m = 0;
                uint64_t v = vals[k];
                do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v);
                while (m) buf[n++] = t[--m];
                s = k == 0 ? " pages (swapped " : k == 1 ? "), present " : ": repaired\n";
                while (*s) buf[n++] = *s++;
            }
            buf[n] = 0;
            serial_write(buf);
        }
        as->mapped_pages = present + as->swapped_pages;
        resident = present;
    }
    return resident;
}

/* Bookkeeping for a page entry the walker cleared */
void vmm_note_unmapped(addr_space_t *as) {
    if (as && as->mapped_pages > 0) as->mapped_pages--;
}
