#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "process.h"
#include "../cpu/isr.h"

void sched_init(void);
process_t *proc_create_kernel(void (*entry)(void));
void schedule(struct registers *regs);

thread_t *sched_current_thread(void);
process_t *sched_current_process(void);

#endif
