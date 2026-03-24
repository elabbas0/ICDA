#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "process.h"
#include "../memory/vmm.h"
#include "../cpu/isr.h"

//  init 
// Must be called after pmm_init(), vmm_init(), and pf_init().
// Creates the idle process (the current boot context becomes PID 0).
void sched_init(void);

//  process creation 
// Create a kernel thread: entry() runs in ring 0, shared kernel address space.
// Returns the new process_t, or NULL on OOM.
process_t *proc_create_kernel(void (*entry)(void));

//  scheduler 
// Pick the next READY process and switch to it.
// Call from the timer IRQ handler.
void schedule(struct registers *regs);

// accessors 
process_t *sched_current(void);   // currently running process

#endif
