#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "process.h"
#include "../cpu/isr.h"

void sched_init(void);
process_t *proc_create_empty(process_kind_t kind);
process_t *proc_create_kernel(void (*entry)(void));
thread_t *proc_create_user_thread(process_t *proc, uint64_t user_rip, uint64_t user_rsp, void (*entry)(void));
void schedule(struct registers *regs);
void sched_yield(void);

thread_t *sched_current_thread(void);
process_t *sched_current_process(void);
process_t *sched_first_process(void);
process_t *sched_find_process(uint64_t pid);
const char *sched_process_state_name(process_state_t state);

#endif
