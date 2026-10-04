#ifndef UACCESS_H
#define UACCESS_H





















#include <stdint.h>
#include <stddef.h>

#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "../memory/pf.h"
#include "../proc/sched.h"


#define USER_HALF_END 0x0000800000000000ULL


#define UACCESS_MAX_LEN 0x40000000ULL

#define UACCESS_MAX_STR 0x100000ULL






static inline int user_page_ready(addr_space_t *as, uint64_t page_va,
                                  int for_write) {
    uint64_t phys;

    page_va &= ~0xFFFULL;
    if (!as) {
        return 0;
    }
    phys = vmm_virt_to_phys(as, page_va);
    if (phys) {
        if (!for_write) {
            return 1;
        }
        return vmm_page_writable(as, page_va) || vmm_cow_break(as, page_va) == 0;
    }
    if (page_va < USER_STACK_LIMIT || page_va >= USER_STACK_TOP) {
        return 0;
    }
    phys = pmm_alloc();
    if (!phys) {
        return 0;
    }
    if (vmm_map_page(as, page_va, phys, VMM_FLAGS_USER_RW) != 0) {
        pmm_free(phys);
        return 0;
    }
    {
        uint64_t *z = (uint64_t *)PHYS_TO_VIRT(phys);
        uint64_t i;
        for (i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++) {
            z[i] = 0;
        }
    }
    return 1;
}




static inline int user_range_prepare(addr_space_t *as, uint64_t addr,
                                     uint64_t len, int for_write) {
    uint64_t end;
    uint64_t page;
    uint64_t last;

    if (len == 0) {
        return 1;
    }
    if (!as) {
        return 0;
    }
    if (len > UACCESS_MAX_LEN) {
        return 0;
    }
    end = addr + len;
    if (end < addr) {
        return 0;
    }
    if (end > USER_HALF_END) {
        return 0;
    }
    page = addr & ~0xFFFULL;
    last = (end - 1) & ~0xFFFULL;
    for (;;) {
        if (!user_page_ready(as, page, for_write)) {
            return 0;
        }
        if (page == last) {
            break;
        }
        page += PAGE_SIZE_4K;
    }
    return 1;
}


static inline int user_range_prepare_cur_r(const void *uaddr, uint64_t len) {
    process_t *proc = sched_current_process();
    if (!proc || proc->kind != PROCESS_USER || !proc->addr_space) {
        return 1;
    }
    return user_range_prepare(proc->addr_space, (uint64_t)(uintptr_t)uaddr,
                              len, 0);
}

static inline int user_range_prepare_cur_w(void *uaddr, uint64_t len) {
    process_t *proc = sched_current_process();
    if (!proc || proc->kind != PROCESS_USER || !proc->addr_space) {
        return 1;
    }
    return user_range_prepare(proc->addr_space, (uint64_t)(uintptr_t)uaddr,
                              len, 1);
}


static inline int user_range_prepare_cur(const void *uaddr, uint64_t len) {
    return user_range_prepare_cur_r(uaddr, len);
}





static inline int copy_to_user(void *udst, const void *ksrc, uint64_t len) {
    process_t *proc = sched_current_process();
    const uint8_t *s = (const uint8_t *)ksrc;
    uint8_t *d = (uint8_t *)udst;
    uint64_t i;

    if (len == 0) {
        return 0;
    }
    if (!udst || !ksrc) {
        return -1;
    }
    if (!proc || proc->kind != PROCESS_USER || !proc->addr_space) {
        for (i = 0; i < len; i++) {
            d[i] = s[i];
        }
        return 0;
    }
    if (!user_range_prepare(proc->addr_space, (uint64_t)(uintptr_t)udst,
                            len, 1)) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}


static inline int copy_from_user(void *kdst, const void *usrc, uint64_t len) {
    process_t *proc = sched_current_process();
    uint8_t *d = (uint8_t *)kdst;
    const uint8_t *s = (const uint8_t *)usrc;
    uint64_t i;

    if (len == 0) {
        return 0;
    }
    if (!kdst || !usrc) {
        return -1;
    }
    if (!proc || proc->kind != PROCESS_USER || !proc->addr_space) {
        for (i = 0; i < len; i++) {
            d[i] = s[i];
        }
        return 0;
    }
    if (!user_range_prepare(proc->addr_space, (uint64_t)(uintptr_t)usrc,
                            len, 0)) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}




static inline uint64_t strnlen_user(const char *usrc, uint64_t max) {
    process_t *proc = sched_current_process();
    uint64_t addr;
    uint64_t n = 0;

    if (!usrc) {
        return (uint64_t)-1;
    }
    if (max > UACCESS_MAX_STR) {
        max = UACCESS_MAX_STR;
    }
    if (!proc || proc->kind != PROCESS_USER || !proc->addr_space) {
        while (n < max && usrc[n]) {
            n++;
        }
        return (n < max) ? n : (uint64_t)-1;
    }
    addr = (uint64_t)(uintptr_t)usrc;
    if (addr >= USER_HALF_END) {
        return (uint64_t)-1;
    }
    if (max > USER_HALF_END - addr) {
        max = USER_HALF_END - addr;
    }
    while (n < max) {
        uint64_t page = (addr + n) & ~0xFFFULL;
        uint64_t page_end = page + PAGE_SIZE_4K;
        if (!user_page_ready(proc->addr_space, page, 0)) {
            return (uint64_t)-1;
        }
        while (n < max && addr + n < page_end) {
            if (((const char *)(uintptr_t)(addr + n))[0] == '\0') {
                return n;
            }
            n++;
        }
    }
    return (uint64_t)-1;
}

#endif
