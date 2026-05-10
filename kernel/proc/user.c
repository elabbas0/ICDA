#include "user.h"

#include "sched.h"
#include "../cpu/gdt.h"
#include "../drivers/console/console.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern uint8_t user_demo_start[];
extern uint8_t user_demo_end[];

volatile uint64_t user_return_rsp = 0;
volatile uint64_t user_return_pending = 0;
static uint64_t user_exit_code = 0;

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

    {
        uint64_t *stack = (uint64_t *)PHYS_TO_VIRT(stack_page);
        for (int i = 0; i < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); i++) {
            stack[i] = 0;
        }
    }

    if (vmm_map_page(proc->addr_space, USER_STACK_TOP - PAGE_SIZE_4K, stack_page, VMM_FLAGS_USER_RW) != 0) {
        pmm_free(stack_page);
        return -1;
    }

    return 0;
}

int user_map_blob(process_t *proc, const void *blob, uint64_t size, uint64_t virt_base) {
    uint64_t pages;
    uint64_t copied = 0;

    if (!proc || !proc->addr_space || !blob || size == 0) {
        return -1;
    }

    pages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        uint64_t chunk = size - copied;
        char *dst;

        if (!phys) {
            return -1;
        }
        if (chunk > PAGE_SIZE_4K) {
            chunk = PAGE_SIZE_4K;
        }
        if (vmm_map_page(proc->addr_space, virt_base + i * PAGE_SIZE_4K, phys, VMM_FLAGS_USER_RO) != 0) {
            pmm_free(phys);
            return -1;
        }

        dst = (char *)PHYS_TO_VIRT(phys);
        for (uint64_t j = 0; j < PAGE_SIZE_4K; j++) {
            dst[j] = 0;
        }
        copy_bytes(dst, (const char *)blob + copied, chunk);
        copied += chunk;
    }
    return 0;
}

void user_request_exit_to_kernel(uint64_t code) {
    user_exit_code = code;
    user_return_pending = 1;
}

uint64_t user_last_exit_code(void) {
    return user_exit_code;
}

int user_run_demo(void) {
    thread_t *current_thread = sched_current_thread();
    process_t *saved_proc;
    process_t *user_proc;
    addr_space_t *saved_as;
    uint64_t blob_size;

    if (!current_thread) {
        return -1;
    }

    user_proc = proc_create_empty(PROCESS_USER);
    if (!user_proc) {
        return -1;
    }
    blob_size = (uint64_t)(user_demo_end - user_demo_start);

    if (user_prepare_address_space(user_proc) != 0) {
        return -1;
    }
    if (user_map_blob(user_proc, user_demo_start, blob_size, USER_TEXT_BASE) != 0) {
        return -1;
    }

    saved_proc = current_thread->owner;
    saved_as = saved_proc ? saved_proc->addr_space : vmm_kernel_address_space();
    current_thread->owner = user_proc;
    tss_set_rsp0(current_thread->kernel_stack_top);
    vmm_switch_address_space(user_proc->addr_space);
    pf_set_current_as(user_proc->addr_space);
    user_return_pending = 0;

    user_enter(USER_TEXT_BASE, USER_STACK_TOP - 16);

    vmm_switch_address_space(saved_as);
    pf_set_current_as(saved_as);
    current_thread->owner = saved_proc;
    if (saved_proc) {
        tss_set_rsp0(current_thread->kernel_stack_top);
    }

    return 0;
}
