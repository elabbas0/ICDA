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
void sched_wake_thread(thread_t *thread);

thread_t *sched_current_thread(void);
process_t *sched_current_process(void);
process_t *sched_first_process(void);
process_t *sched_find_process(uint64_t pid);
const char *sched_process_state_name(process_state_t state);
uint64_t sched_ticks(void);
void sched_sleep(uint64_t ticks);
void sched_wait_input(void);
void sched_wait_input_timeout(uint64_t ticks);
void sched_wake_input_waiters(void);
int sched_suspend_process(uint64_t pid);
int sched_resume_process(uint64_t pid);
int sched_kill_process(uint64_t pid, uint64_t exit_code);

/* IRQ-context safe: marks the current user process (and every other user
 * process) exited so user_run_path() returns and the boot loop can start
 * the app for the newly selected virtual terminal. */
void sched_force_exit_current_with_children(uint64_t exit_code);
void sched_force_exit_all_user_processes(uint64_t exit_code);

#endif
