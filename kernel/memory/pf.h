#ifndef PF_H
#define PF_H

#include <stdint.h>
#include "vmm.h"

//  page fault error code bits (pushed by cpu as err_code) 
#define PF_PRESENT   (1ULL << 0)  // 0 = not-present fault, 1 = protection fault
#define PF_WRITE     (1ULL << 1)  // 0 = read,  1 = write
#define PF_USER      (1ULL << 2)  // 0 = kernel, 1 = user mode
#define PF_RESERVED  (1ULL << 3)  // reserved PTE bit was set
#define PF_IFETCH    (1ULL << 4)  // fault during instruction fetch

//  user stack layout 
// Each user process gets a stack that grows downward from USER_STACK_TOP.
// The VMM will map new pages on demand inside [USER_STACK_LIMIT, USER_STACK_TOP).
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL  // first byte ABOVE user stack
#define USER_STACK_LIMIT  0x00007FFFFFF00000ULL  // max stack size ~8 MiB

//  scheduler hook 
// The scheduler must call this whenever it switches to a new process so the
// page fault handler knows which addr_space_t to map new pages into.
// Before the scheduler exists, the kernel address space is used.
void pf_set_current_as(addr_space_t *as);

//  init 
// Registers the page fault handler with the ISR system (vec 14).
// Call after pmm_init() and vmm_init().
void pf_init(void);

#endif
