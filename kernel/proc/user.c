#include "user.h"

#include "sched.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

int user_prepare_address_space(process_t *proc) {
    uint64_t stack_page;

    if (!proc) {
        return -1;
    }

    if (!proc->addr_space || proc->addr_space == vmm_kernel_address_space()) {
        proc->addr_space = vmm_create_address_space();
        if (!proc->addr_space) {
            return -1;
        }
    }

    stack_page = pmm_alloc();
    if (!stack_page) {
        return -1;
    }

    if (vmm_map_page(proc->addr_space, USER_STACK_TOP - PAGE_SIZE_4K, stack_page, VMM_FLAGS_USER_RW) != 0) {
        pmm_free(stack_page);
        return -1;
    }

    {
        uint64_t *stack = (uint64_t *)(uintptr_t)(USER_STACK_TOP - PAGE_SIZE_4K);
        for (int i = 0; i < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); i++) {
            stack[i] = 0;
        }
    }

    return 0;
}

int user_map_blob(process_t *proc, const void *blob, uint64_t size, uint64_t virt_base) {
    process_t *current_proc;
    addr_space_t *restore_as;
    uint64_t pages;
    uint64_t copied = 0;

    if (!proc || !proc->addr_space || !blob || size == 0) {
        return -1;
    }

    current_proc = sched_current_process();
    restore_as = current_proc ? current_proc->addr_space : vmm_kernel_address_space();
    vmm_switch_address_space(proc->addr_space);
    pf_set_current_as(proc->addr_space);

    pages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        uint64_t chunk = size - copied;
        char *dst;

        if (!phys) {
            vmm_switch_address_space(restore_as);
            pf_set_current_as(restore_as);
            return -1;
        }
        if (chunk > PAGE_SIZE_4K) {
            chunk = PAGE_SIZE_4K;
        }
        if (vmm_map_page(proc->addr_space, virt_base + i * PAGE_SIZE_4K, phys, VMM_FLAGS_USER_RO) != 0) {
            pmm_free(phys);
            vmm_switch_address_space(restore_as);
            pf_set_current_as(restore_as);
            return -1;
        }

        dst = (char *)(uintptr_t)(virt_base + i * PAGE_SIZE_4K);
        copy_bytes(dst, (const char *)blob + copied, chunk);
        copied += chunk;
    }

    vmm_switch_address_space(restore_as);
    pf_set_current_as(restore_as);
    return 0;
}
