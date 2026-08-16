#include "sched.h"
#include "process.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../memory/pf.h"
#include "../cpu/gdt.h"
#include "../fs/vfs.h"
#include "../drivers/console/console.h"

extern void switch_context(thread_t *prev, thread_t *next);
extern void user_thread_start(void);
static void kthread_trampoline(void);

thread_t *current_thread_ptr = NULL;
static thread_t *idle_thread_ptr = NULL;
static process_t *process_list = NULL;
static uint64_t next_pid = 0;
static uint64_t next_tid = 0;

#define SCHED_TICKS 1
static uint64_t tick_count = 0;
static uint64_t uptime_ticks = 0;

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

static void register_process(process_t *proc) {
    if (!proc) {
        return;
    }
    proc->next_all = process_list;
    process_list = proc;
}

static uint64_t alloc_kernel_stack(void) {
    uint64_t phys = pmm_alloc_contiguous(KERNEL_STACK_PAGES);
    if (!phys) {
        return 0;
    }
    return (uint64_t)PHYS_TO_VIRT(phys) + KERNEL_STACK_SIZE;
}

static uint64_t thread_entry_stack_top(const thread_t *thread) {
    if (!thread) {
        return 0;
    }
    if (thread->owner && thread->owner->kind == PROCESS_USER && thread->user_entry_stack_top) {
        return thread->user_entry_stack_top;
    }
    return thread->kernel_stack_top;
}

static int sched_can_control(process_t *actor, process_t *target) {
    if (!actor || !target) {
        return 0;
    }
    if (target->kind != PROCESS_USER) {
        return 0;
    }
    if (target->state == PROCESS_REAPED) {
        return 0;
    }
    return actor->session_id == target->session_id;
}

static void sched_wake_parent_if_waiting(process_t *proc) {
    if (proc && proc->parent && proc->parent->main_thread &&
        proc->parent->main_thread->state == THREAD_BLOCKED) {
        proc->parent->main_thread->state = THREAD_READY;
        proc->parent->main_thread->block_reason = THREAD_BLOCK_NONE;
        proc->parent->main_thread->wake_tick = 0;
        if (proc->parent->state == PROCESS_BLOCKED) {
            proc->parent->state = PROCESS_READY;
        }
    }
}

static void wake_blocked_threads(void) {
    process_t *proc = process_list;

    while (proc) {
        thread_t *thread = proc->main_thread;
        if (thread &&
            thread->state == THREAD_BLOCKED &&
            (thread->block_reason == THREAD_BLOCK_SLEEP ||
             (thread->block_reason == THREAD_BLOCK_INPUT && thread->wake_tick != 0)) &&
            thread->wake_tick <= uptime_ticks) {
            thread->state = THREAD_READY;
            thread->block_reason = THREAD_BLOCK_NONE;
            thread->wake_tick = 0;
            if (proc->state == PROCESS_BLOCKED) {
                proc->state = PROCESS_READY;
            }
        }
        proc = proc->next_all;
    }
}

void sched_wake_input_waiters(void) {
    process_t *proc = process_list;

    while (proc) {
        thread_t *thread = proc->main_thread;
        if (thread &&
            thread->state == THREAD_BLOCKED &&
            thread->block_reason == THREAD_BLOCK_INPUT) {
            thread->state = THREAD_READY;
            thread->block_reason = THREAD_BLOCK_NONE;
            thread->wake_tick = 0;
            if (proc->state == PROCESS_BLOCKED) {
                proc->state = PROCESS_READY;
            }
        }
        proc = proc->next_all;
    }
}

static void enqueue(thread_t *thread) {
    if (!current_thread_ptr) {
        thread->next = thread;
        current_thread_ptr = thread;
        return;
    }

    thread_t *tail = current_thread_ptr;
    while (tail->next != current_thread_ptr) {
        tail = tail->next;
    }

    tail->next = thread;
    thread->next = current_thread_ptr;
}

static void idle_thread_main(void) {
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

static void prepare_kernel_thread_stack(thread_t *thread, uint64_t stack_top, void (*entry)(void)) {
    uint64_t *sp = (uint64_t *)stack_top;

    thread->kernel_stack_top = stack_top;
    thread->entry = entry;

    *(--sp) = (uint64_t)kthread_trampoline;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    thread->kernel_rsp = (uint64_t)sp;
}

process_t *proc_create_empty(process_kind_t kind) {
    process_t *proc = alloc_process();

    if (!proc) {
        return NULL;
    }

    proc->pid = next_pid++;
    proc->kind = kind;
    proc->state = PROCESS_NEW;
    proc->exit_code = 0;
    proc->addr_space = vmm_kernel_address_space();
    proc->parent = sched_current_process();
    proc->session_id = proc->parent ? proc->parent->session_id : proc->pid;
    proc->process_group_id = proc->pid;
    proc->cwd = proc->parent ? proc->parent->cwd : vfs_root();
    proc->main_thread = NULL;
    register_process(proc);
    return proc;
}

void sched_init(void) {
    process_t *bootstrap_proc = alloc_process();
    thread_t *bootstrap_thread = alloc_thread();
    process_t *idle_proc = alloc_process();
    thread_t *idle_thread = alloc_thread();
    uint64_t bootstrap_stack_top;
    uint64_t idle_stack_top;

    if (!bootstrap_proc || !bootstrap_thread || !idle_proc || !idle_thread) {
        console_write("sched_init: OOM\n", CONSOLE_STYLE_ERROR);
        return;
    }

    bootstrap_stack_top = alloc_kernel_stack();
    idle_stack_top = alloc_kernel_stack();
    if (!bootstrap_stack_top || !idle_stack_top) {
        console_write("sched_init: no kernel stack for scheduler bootstrap\n", CONSOLE_STYLE_ERROR);
        return;
    }

    bootstrap_proc->pid = next_pid++;
    bootstrap_proc->kind = PROCESS_KERNEL;
    bootstrap_proc->state = PROCESS_RUNNING;
    bootstrap_proc->exit_code = 0;
    bootstrap_proc->session_id = bootstrap_proc->pid;
    bootstrap_proc->process_group_id = bootstrap_proc->pid;
    bootstrap_proc->addr_space = vmm_kernel_address_space();
    bootstrap_proc->main_thread = bootstrap_thread;
    bootstrap_proc->cwd = vfs_root();
    register_process(bootstrap_proc);

    bootstrap_thread->tid = next_tid++;
    bootstrap_thread->state = THREAD_RUNNING;
    bootstrap_thread->owner = bootstrap_proc;
    bootstrap_thread->kernel_stack_top = bootstrap_stack_top;

    idle_proc->pid = next_pid++;
    idle_proc->kind = PROCESS_KERNEL;
    idle_proc->state = PROCESS_READY;
    idle_proc->exit_code = 0;
    idle_proc->session_id = bootstrap_proc->session_id;
    idle_proc->process_group_id = idle_proc->pid;
    idle_proc->addr_space = vmm_kernel_address_space();
    idle_proc->main_thread = idle_thread;
    idle_proc->cwd = vfs_root();
    register_process(idle_proc);

    idle_thread->tid = next_tid++;
    idle_thread->state = THREAD_READY;
    idle_thread->owner = idle_proc;
    prepare_kernel_thread_stack(idle_thread, idle_stack_top, idle_thread_main);
    idle_thread_ptr = idle_thread;

    bootstrap_thread->next = idle_thread;
    idle_thread->next = bootstrap_thread;

    current_thread_ptr = bootstrap_thread;
    tss_set_rsp0(bootstrap_stack_top);
    pf_set_current_as(bootstrap_proc->addr_space);
}

static void kthread_trampoline(void) {
    thread_t *thread = sched_current_thread();
    thread->entry();
    while (1) {
        __asm__ volatile("hlt");
    }
}

process_t *proc_create_kernel(void (*entry)(void)) {
    process_t *proc = proc_create_empty(PROCESS_KERNEL);
    thread_t *thread = alloc_thread();
    uint64_t stack_top;

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

    proc->main_thread = thread;
    proc->state = PROCESS_READY;

    thread->tid = next_tid++;
    thread->state = THREAD_READY;
    thread->owner = proc;
    prepare_kernel_thread_stack(thread, stack_top, entry);

    enqueue(thread);
    return proc;
}

thread_t *proc_create_user_thread(process_t *proc, uint64_t user_rip, uint64_t user_rsp, void (*entry)(void)) {
    thread_t *thread = alloc_thread();
    uint64_t stack_top;
    uint64_t entry_stack_top;

    if (!proc || !entry || !user_rip || !user_rsp || !thread) {
        return NULL;
    }

    stack_top = alloc_kernel_stack();
    if (!stack_top) {
        if (thread) {
            pmm_free(VIRT_TO_PHYS((uint64_t)thread));
        }
        return NULL;
    }
    entry_stack_top = alloc_kernel_stack();
    if (!entry_stack_top) {
        pmm_free_range(VIRT_TO_PHYS(stack_top - KERNEL_STACK_SIZE), KERNEL_STACK_PAGES);
        pmm_free(VIRT_TO_PHYS((uint64_t)thread));
        return NULL;
    }

    proc->main_thread = thread;
    proc->state = PROCESS_READY;

    thread->tid = next_tid++;
    thread->state = THREAD_READY;
    thread->owner = proc;
    thread->user_entry_stack_top = entry_stack_top;
    thread->user_rip = user_rip;
    thread->user_rsp = user_rsp;
    prepare_kernel_thread_stack(thread, stack_top, entry);
    enqueue(thread);
    return thread;
}

static void schedule_inner(int force) {
    thread_t *next;
    thread_t *prev;
    thread_t *candidate_idle = 0;
    int laps = 0;

    if (!current_thread_ptr) {
        return;
    }

    if (!force && ++tick_count % SCHED_TICKS != 0) {
        return;
    }

    next = current_thread_ptr->next;
    while (1) {
        if ((next->state == THREAD_READY || next->state == THREAD_RUNNING) && next != idle_thread_ptr) {
            break;
        }
        if ((next->state == THREAD_READY || next->state == THREAD_RUNNING) && next == idle_thread_ptr) {
            candidate_idle = next;
        }
        next = next->next;
        if (++laps > 1024) {
            next = candidate_idle;
            break;
        }
    }

    if (!next) {
        return;
    }

    if (next == current_thread_ptr) {
        return;
    }

    prev = current_thread_ptr;
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;
    current_thread_ptr = next;

    if (thread_entry_stack_top(next)) {
        tss_set_rsp0(thread_entry_stack_top(next));
    }

    if (next->owner->addr_space != prev->owner->addr_space) {
        vmm_switch_address_space(next->owner->addr_space);
        pf_set_current_as(next->owner->addr_space);
    }

    switch_context(prev, next);
}

void schedule(struct registers *regs) {
    (void)regs;
    uptime_ticks++;
    wake_blocked_threads();
    schedule_inner(0);
}

void sched_yield(void) {
    schedule_inner(1);
}

void sched_wake_thread(thread_t *thread) {
    process_t *proc;

    if (!thread) {
        return;
    }

    proc = thread->owner;
    if (thread->state == THREAD_BLOCKED) {
        thread->state = THREAD_READY;
        thread->block_reason = THREAD_BLOCK_NONE;
        thread->wake_tick = 0;
    }

    if (proc && proc->state != PROCESS_EXITED && proc->state != PROCESS_REAPED && proc->state != PROCESS_STOPPED) {
        if (proc->state == PROCESS_BLOCKED || proc->state == PROCESS_NEW) {
            proc->state = PROCESS_READY;
        }
    }
}

uint64_t sched_ticks(void) {
    return uptime_ticks;
}

void sched_sleep(uint64_t ticks) {
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();

    if (!thread) {
        return;
    }

    if (ticks == 0) {
        sched_yield();
        return;
    }

    thread->wake_tick = uptime_ticks + ticks;
    thread->block_reason = THREAD_BLOCK_SLEEP;
    thread->state = THREAD_BLOCKED;
    if (proc && proc->state != PROCESS_EXITED && proc->state != PROCESS_REAPED) {
        proc->state = PROCESS_BLOCKED;
    }
    schedule_inner(1);
}

void sched_wait_input(void) {
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();

    if (!thread) {
        return;
    }

    thread->wake_tick = 0;
    thread->block_reason = THREAD_BLOCK_INPUT;
    thread->state = THREAD_BLOCKED;
    if (proc && proc->state != PROCESS_EXITED && proc->state != PROCESS_REAPED && proc->state != PROCESS_STOPPED) {
        proc->state = PROCESS_BLOCKED;
    }
    schedule_inner(1);
}

void sched_wait_input_timeout(uint64_t ticks) {
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();

    if (!thread) {
        return;
    }

    if (ticks == 0) {
        sched_wait_input();
        return;
    }

    thread->wake_tick = uptime_ticks + ticks;
    thread->block_reason = THREAD_BLOCK_INPUT;
    thread->state = THREAD_BLOCKED;
    if (proc && proc->state != PROCESS_EXITED && proc->state != PROCESS_REAPED && proc->state != PROCESS_STOPPED) {
        proc->state = PROCESS_BLOCKED;
    }
    schedule_inner(1);
}

thread_t *sched_current_thread(void) {
    return current_thread_ptr;
}

process_t *sched_current_process(void) {
    return current_thread_ptr ? current_thread_ptr->owner : NULL;
}

process_t *sched_first_process(void) {
    return process_list;
}

process_t *sched_find_process(uint64_t pid) {
    process_t *proc = process_list;
    while (proc) {
        if (proc->pid == pid) {
            return proc;
        }
        proc = proc->next_all;
    }
    return NULL;
}

const char *sched_process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_NEW:
            return "new";
        case PROCESS_READY:
            return "ready";
        case PROCESS_RUNNING:
            return "running";
        case PROCESS_BLOCKED:
            return "blocked";
        case PROCESS_STOPPED:
            return "stopped";
        case PROCESS_EXITED:
            return "exited";
        case PROCESS_REAPED:
            return "reaped";
        default:
            return "unknown";
    }
}

int sched_suspend_process(uint64_t pid) {
    process_t *actor = sched_current_process();
    process_t *target = sched_find_process(pid);
    thread_t *thread;

    if (!sched_can_control(actor, target) || target == actor) {
        return -1;
    }
    if (target->state == PROCESS_EXITED || target->state == PROCESS_REAPED || target->state == PROCESS_STOPPED) {
        return -1;
    }

    thread = target->main_thread;
    if (!thread) {
        return -1;
    }

    target->state = PROCESS_STOPPED;
    if (thread->state != THREAD_ZOMBIE) {
        thread->state = THREAD_STOPPED;
    }
    return 0;
}

int sched_resume_process(uint64_t pid) {
    process_t *actor = sched_current_process();
    process_t *target = sched_find_process(pid);
    thread_t *thread;

    if (!sched_can_control(actor, target)) {
        return -1;
    }
    if (!target || target->state != PROCESS_STOPPED) {
        return -1;
    }

    thread = target->main_thread;
    if (!thread || thread->state != THREAD_STOPPED) {
        return -1;
    }

    if (thread->block_reason == THREAD_BLOCK_SLEEP && thread->wake_tick > uptime_ticks) {
        thread->state = THREAD_BLOCKED;
        target->state = PROCESS_BLOCKED;
    } else if (thread->block_reason == THREAD_BLOCK_WAIT_CHILD ||
               thread->block_reason == THREAD_BLOCK_INPUT) {
        thread->state = THREAD_BLOCKED;
        target->state = PROCESS_BLOCKED;
    } else {
        thread->block_reason = THREAD_BLOCK_NONE;
        thread->wake_tick = 0;
        thread->state = THREAD_READY;
        target->state = PROCESS_READY;
    }
    return 0;
}

int sched_kill_process(uint64_t pid, uint64_t exit_code) {
    process_t *actor = sched_current_process();
    process_t *target = sched_find_process(pid);
    thread_t *thread;

    if (!sched_can_control(actor, target)) {
        return -1;
    }
    if (!target || target->state == PROCESS_EXITED || target->state == PROCESS_REAPED) {
        return -1;
    }

    thread = target->main_thread;
    if (!thread) {
        return -1;
    }
    if (thread == current_thread_ptr) {
        return -1;
    }

    target->state = PROCESS_EXITED;
    target->exit_code = exit_code;
    thread->block_reason = THREAD_BLOCK_NONE;
    thread->wake_tick = 0;

    thread->state = THREAD_ZOMBIE;
    sched_wake_parent_if_waiting(target);
    return 0;
}

void sched_force_exit_current_with_children(uint64_t exit_code) {
    thread_t *thread = current_thread_ptr;
    process_t *proc = thread ? thread->owner : NULL;

    if (!thread || !proc) {
        return;
    }

    /* The timer IRQ that triggers a VT switch can fire while the
     * foreground app is blocked (e.g. the WM waiting for input), with
     * the idle thread current.  So force-exit every user process -
     * the foreground app and all its descendants - rather than only
     * the current thread's owner. */
    sched_force_exit_all_user_processes(exit_code);
}

void sched_force_exit_all_user_processes(uint64_t exit_code) {
    process_t *p = process_list;

    while (p) {
        thread_t *t = p->main_thread;
        if (p->kind == PROCESS_USER &&
            p->state != PROCESS_EXITED && p->state != PROCESS_REAPED && t) {
            p->state = PROCESS_EXITED;
            p->exit_code = exit_code;
            t->block_reason = THREAD_BLOCK_NONE;
            t->wake_tick = 0;
            t->state = THREAD_ZOMBIE;
            sched_wake_parent_if_waiting(p);
        }
        p = p->next_all;
    }
}
