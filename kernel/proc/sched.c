#include "sched.h"
#include "process.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../memory/pf.h"
#include "../cpu/gdt.h"
#include "../fs/vfs.h"
#include "../drivers/console/console.h"

extern void switch_context(thread_t *prev, thread_t *next);

static thread_t *current_thread = NULL;
static uint64_t next_pid = 0;
static uint64_t next_tid = 0;

#define SCHED_TICKS 2
static uint64_t tick_count = 0;

static void *alloc_object_page(void) {
    uint64_t phys = pmm_alloc();
    uint8_t *obj;

    if (!phys) {
        return NULL;
    }

    obj = (uint8_t *)PHYS_TO_VIRT(phys);
    for (int i = 0; i < (int)PAGE_SIZE_4K; i++) {
        obj[i] = 0;
    }

    return obj;
}

static process_t *alloc_process(void) {
    return (process_t *)alloc_object_page();
}

static thread_t *alloc_thread(void) {
    return (thread_t *)alloc_object_page();
}

static uint64_t alloc_kernel_stack(void) {
    uint64_t phys = pmm_alloc_contiguous(KERNEL_STACK_PAGES);
    if (!phys) {
        return 0;
    }
    return (uint64_t)PHYS_TO_VIRT(phys) + KERNEL_STACK_SIZE;
}

static void enqueue(thread_t *thread) {
    if (!current_thread) {
        thread->next = thread;
        current_thread = thread;
        return;
    }

    thread_t *tail = current_thread;
    while (tail->next != current_thread) {
        tail = tail->next;
    }

    tail->next = thread;
    thread->next = current_thread;
}

void sched_init(void) {
    process_t *idle_proc = alloc_process();
    thread_t *idle_thread = alloc_thread();

    if (!idle_proc || !idle_thread) {
        console_write("sched_init: OOM\n", CONSOLE_STYLE_ERROR);
        return;
    }

    idle_proc->pid = next_pid++;
    idle_proc->kind = PROCESS_KERNEL;
    idle_proc->addr_space = vmm_kernel_address_space();
    idle_proc->main_thread = idle_thread;
    idle_proc->cwd = vfs_root();

    idle_thread->tid = next_tid++;
    idle_thread->state = THREAD_RUNNING;
    idle_thread->owner = idle_proc;
    idle_thread->kernel_stack_top = 0;
    idle_thread->next = idle_thread;

    current_thread = idle_thread;
    pf_set_current_as(idle_proc->addr_space);
}

static void kthread_trampoline(void) {
    __asm__ volatile("sti");
    sched_current_thread()->entry();
    while (1) {
        __asm__ volatile("hlt");
    }
}

process_t *proc_create_kernel(void (*entry)(void)) {
    process_t *proc = alloc_process();
    thread_t *thread = alloc_thread();
    uint64_t stack_top;
    uint64_t *sp;

    if (!proc || !thread) {
        return NULL;
    }

    stack_top = alloc_kernel_stack();
    if (!stack_top) {
        if (thread) {
            pmm_free(VIRT_TO_PHYS((uint64_t)thread));
        }
        if (proc) {
            pmm_free(VIRT_TO_PHYS((uint64_t)proc));
        }
        return NULL;
    }

    proc->pid = next_pid++;
    proc->kind = PROCESS_KERNEL;
    proc->addr_space = vmm_kernel_address_space();
    proc->parent = sched_current_process();
    proc->main_thread = thread;
    proc->cwd = proc->parent ? proc->parent->cwd : vfs_root();

    thread->tid = next_tid++;
    thread->state = THREAD_READY;
    thread->owner = proc;
    thread->kernel_stack_top = stack_top;
    thread->entry = entry;

    sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)kthread_trampoline;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    thread->kernel_rsp = (uint64_t)sp;

    enqueue(thread);
    return proc;
}

void schedule(struct registers *regs) {
    thread_t *next;
    thread_t *prev;
    int laps = 0;

    (void)regs;

    if (!current_thread) {
        return;
    }

    if (++tick_count % SCHED_TICKS != 0) {
        return;
    }

    next = current_thread->next;
    while (next->state != THREAD_READY && next->state != THREAD_RUNNING) {
        next = next->next;
        if (++laps > 1024) {
            return;
        }
    }

    if (next == current_thread) {
        return;
    }

    prev = current_thread;
    prev->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    current_thread = next;

    if (next->kernel_stack_top) {
        tss_set_rsp0(next->kernel_stack_top);
    }

    if (next->owner->addr_space != prev->owner->addr_space) {
        vmm_switch_address_space(next->owner->addr_space);
        pf_set_current_as(next->owner->addr_space);
    }

    switch_context(prev, next);
}

thread_t *sched_current_thread(void) {
    return current_thread;
}

process_t *sched_current_process(void) {
    return current_thread ? current_thread->owner : NULL;
}
