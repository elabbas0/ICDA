#ifndef UACCESS_H
#define UACCESS_H

/*
 * Syscall user-pointer gate (P0 boundary hardening).
 *
 * Every pointer arriving from userspace (Ring 3) MUST pass through these
 * helpers before the kernel dereferences it.  Design notes:
 *
 *  - Single-threaded kernel: once a range is probed + faulted-in below,
 *    nothing else runs before the caller uses it, so probe-then-use is
 *    safe.  If the kernel ever gains preemptive threads sharing an
 *    address space, all callers must switch to copy-through-kbuf.
 *  - Demand-stack safe (critic B1): pages inside
 *    [USER_STACK_LIMIT, USER_STACK_TOP) are faulted in on the spot
 *    instead of rejected, mirroring what pf.c does for kernel-mode
 *    stack writes.  Kernel-mode READS of unmapped stack would panic,
 *    so faulting them in here (instead of merely accepting) is what
 *    makes copy_from_user/strnlen_user safe.
 *  - Trusted kernel callers (syscall_kernel_write and friends, running
 *    as PROCESS_KERNEL) bypass validation entirely.
 */

#include <stdint.h>
#include <stddef.h>

#include "../memory/vmm.h"
#include "../memory/pmm.h"
#include "../memory/pf.h"
#include "../proc/sched.h"

/* Top of the user half (vmm.h layout: user is [0, 0x0000800000000000)). */
#define USER_HALF_END 0x0000800000000000ULL
/* Upper bound for any single validated range (1 GiB). Rejects nonsense
 * lengths before the page walk. */
#define UACCESS_MAX_LEN 0x40000000ULL
/* Upper bound for bounded string scans (1 MiB). */
#define UACCESS_MAX_STR 0x100000ULL

/* Ensure one user page is safe for kernel access: present (and, when
 * for_write, PTE-writable — with CR0.WP=1 a supervisor store to a
 * read-only user page faults instead of succeeding), or demand-mapped
 * RW when it lies in the user stack growth region. Returns 1 if the
 * page may be touched, 0 to fail closed. */
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
        return vmm_page_writable(as, page_va);
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

/* Validate + fault-in [addr, addr+len) for the given address space.
 * for_write additionally requires every page PTE-writable (see above).
 * len == 0 is valid. Returns 1 (safe) or 0 (fail closed). */
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

/* Same, for the calling process. Trusted (non-user) callers bypass. */
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

/* Back-compat alias: unqualified uses are reads. */
static inline int user_range_prepare_cur(const void *uaddr, uint64_t len) {
    return user_range_prepare_cur_r(uaddr, len);
}

/* Copy kernel -> user. Returns 0 on success, -1 with nothing touched
 * beyond already-validated pages on failure (callers must fail the
 * syscall; partial copies on huge ranges are possible, so validate
 * small bounded lengths). */
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

/* Copy user -> kernel. Returns 0 on success, -1 on failure. */
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

/* Bounded strlen on a user string. Every touched page is ensured first,
 * so the scan itself cannot fault. Returns length (excluding NUL), or
 * (uint64_t)-1 when the string runs past `max` or outside user memory. */
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
