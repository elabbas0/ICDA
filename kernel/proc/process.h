#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "../memory/vmm.h"

//  kernel stack 
#define KERNEL_STACK_PAGES  2                            // 8 KiB per process
#define KERNEL_STACK_SIZE   (KERNEL_STACK_PAGES * PAGE_SIZE_4K)

//  states 
typedef enum {
    PROC_READY   = 0,   // in the run queue, waiting for CPU
    PROC_RUNNING = 1,   // currently on the CPU
    PROC_BLOCKED = 2,   // waiting for an event (future use)
    PROC_ZOMBIE  = 3,   // exited, waiting to be reaped (future use)
} proc_state_t;

//  process control block 
//
// kernel_rsp is the only field that switch_context.asm touches directly;
// its offset must stay at 0 for the asm to work without knowing the full struct.
//
typedef struct process {
    //  context (must be first, offset 0) 
    uint64_t    kernel_rsp;         // saved kernel stack pointer

    //  identity 
    uint64_t    pid;                // unique process ID
    proc_state_t state;

    //  memory 
    addr_space_t *addr_space;       // virtual address space (owns its PML4)
    uint64_t    kernel_stack_top;   // top (high addr) of this process's kernel stack
                                    // stored here so the scheduler can set TSS.RSP0

    //  scheduler queue 
    void        (*entry)(void);     // kernel thread entry point (for trampoline)
    struct process *next;           // circular singly-linked run queue
} process_t;

#endif
