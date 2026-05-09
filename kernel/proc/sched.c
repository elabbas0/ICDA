#include "sched.h"
#include "process.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../memory/pf.h"
#include "../cpu/gdt.h"
#include "../cpu/isr.h"
#include "../drivers/console/console.h"

extern void switch_context(process_t *prev, process_t *next);

//  run queue 
// Circular singly-linked list. current always points to the running process.
// The list is: current → next → ... → current.
static process_t *current = NULL;
static uint64_t   next_pid = 0;

//  scheduler tick counter 
// We don't switch on every tick — that would be too fast at 18 Hz.
// Switch every SCHED_TICKS ticks (~every 110 ms at 18 Hz).
#define SCHED_TICKS 2
static uint64_t tick_count = 0;

//  process_t allocation 
// We allocate one physical page to hold the process_t struct itself.
// This is simple and avoids needing a heap (kmalloc) yet.
static process_t *alloc_pcb(void) {
    uint64_t phys = pmm_alloc();
    if (!phys) return NULL;

    // zero the page through the HHDM
    process_t *p = (process_t *)PHYS_TO_VIRT(phys);
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < (int)PAGE_SIZE_4K; i++) b[i] = 0;
    return p;
}

//  kernel stack allocation 
// Allocates KERNEL_STACK_PAGES contiguous physical pages and maps them into the kernel address space via the HHDM. Returns the virtual address of the
// TOP (high address) of the stack, or 0 on failure.
static uint64_t alloc_kernel_stack(void) {
    uint64_t phys = pmm_alloc_contiguous(KERNEL_STACK_PAGES);
    if (!phys) return 0;
    // stack top = HHDM virtual address of the page + its size
    return (uint64_t)PHYS_TO_VIRT(phys) + KERNEL_STACK_SIZE;
}

//  enqueue 
static void enqueue(process_t *p) {
    if (!current) {
        // first process — point to itself
        p->next = p;
        current = p;
    } else {
        // insert after current (just before current in the circle)
        // walk to the node whose next == current, insert there
        process_t *tail = current;
        while (tail->next != current) tail = tail->next;
        tail->next = p;
        p->next    = current;
    }
}

//  sched_init 
// Turns the running boot context into PID 0 (the idle process).
// The boot stack becomes its kernel stack; we just need to fill in the PCB.
void sched_init(void) {
    process_t *idle = alloc_pcb();
    if (!idle) {
        console_write("sched_init: OOM\n", CONSOLE_STYLE_ERROR);
        return;
    }

    idle->pid              = next_pid++;
    idle->state            = PROC_RUNNING;
    idle->addr_space       = vmm_kernel_address_space();
    // kernel_rsp will be set by the first switch_context call — no need to init
    // kernel_stack_top: we don't know the boot stack top precisely, but the
    // idle process never goes to ring 3, so TSS.RSP0 doesn't matter for it.
    idle->kernel_stack_top = 0;
    idle->next             = idle;

    current = idle;

    // tell the page fault handler about the initial address space
    pf_set_current_as(idle->addr_space);
}

//  kernel thread trampoline 
// Every kernel thread lands here on its first schedule-in.
// We re-enable interrupts (the IRQ handler cleared IF; since we never iretq'd
// back into the new thread, IF stayed 0) then call the real entry function.
static void kthread_trampoline(void) {
    __asm__ volatile("sti");
    sched_current()->entry();
    // if entry() ever returns, loop forever (should never happen)
    while (1) __asm__ volatile("hlt");
}

//  proc_create_kernel 
process_t *proc_create_kernel(void (*entry)(void)) {
    process_t *p = alloc_pcb();
    if (!p) return NULL;

    uint64_t stack_top = alloc_kernel_stack();
    if (!stack_top) { pmm_free(VIRT_TO_PHYS((uint64_t)p)); return NULL; }

    p->pid              = next_pid++;
    p->state            = PROC_READY;
    p->addr_space       = vmm_kernel_address_space();
    p->kernel_stack_top = stack_top;
    p->entry            = entry;   // saved for the trampoline

    //  fake the kernel stack frame 
    // switch_context pops: r15, r14, r13, r12, rbp, rbx, then does ret.
    // The ret address is kthread_trampoline, which enables interrupts then
    // calls entry(). Without the trampoline the new thread would start with
    // IF=0 (interrupts disabled) because it never goes through iretq.
    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)kthread_trampoline;  // ret → trampoline
    *(--sp) = 0;   // rbx
    *(--sp) = 0;   // rbp
    *(--sp) = 0;   // r12
    *(--sp) = 0;   // r13
    *(--sp) = 0;   // r14
    *(--sp) = 0;   // r15

    p->kernel_rsp = (uint64_t)sp;

    enqueue(p);
    return p;
}

//  schedule 
// called from the timer IRQ,piks the next READY process and switches to it.
void schedule(struct registers *regs) {
    (void)regs;

    if (!current) return;

    // throttle: only switch every SCHED_TICKS timer ticks
    if (++tick_count % SCHED_TICKS != 0) return;

    // find next READY process in the circular list
    process_t *next = current->next;
    int laps = 0;
    while (next->state != PROC_READY && next->state != PROC_RUNNING) {
        next = next->next;
        if (++laps > 1024) return;   // no runnable process found
    }

    if (next == current) return;     // nothing else to run

    // perform the switch 
    process_t *prev = current;

    prev->state = PROC_READY;
    next->state = PROC_RUNNING;
    current     = next;

    // update TSS.RSP0 so the CPU uses the new process's kernel stack
    // if it receives an interrupt while in ring 3
    if (next->kernel_stack_top)
        tss_set_rsp0(next->kernel_stack_top);

    // switch address space if needed (noop if both use kernel AS)
    if (next->addr_space != prev->addr_space) {
        vmm_switch_address_space(next->addr_space);
        pf_set_current_as(next->addr_space);
    }

    switch_context(prev, next);
    // execution resumes here when prev is switched back in
}

// accessors 
process_t *sched_current(void) { return current; }
