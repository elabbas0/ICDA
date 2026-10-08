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
void sched_tick(void);
void sched_ap_tick(void);
void sched_idle_loop(void);
thread_t *sched_create_idle(uint32_t cpu_index);
void sched_wake_thread(thread_t *thread);
thread_t *proc_create_sibling_thread(process_t *proc, uint64_t user_rip, uint64_t user_rsp, void (*entry)(void));
void sched_stop_threads(process_t *proc, thread_t *keep);
int sched_futex_wait(uint64_t uaddr, uint64_t timeout_ticks);
int sched_futex_wake(uint64_t uaddr, int n);
int sched_futex_requeue(uint64_t uaddr, uint64_t uaddr2, int n);

thread_t *sched_current_thread(void);
process_t *sched_current_process(void);
process_t *sched_first_process(void);
process_t *sched_find_process(uint64_t pid);
uint64_t   sched_idle_ticks(void);
/* no timer preemption of the running thread until enabled again (nests) */
void       sched_preempt_disable(void);
void       sched_preempt_enable(void);
/* sleep until sched_event_wake or one tick; wake every such sleeper */
void       sched_event_wait(void);
void       sched_event_wake(void);
void       sched_wait_on(const void *key);
void       sched_event_wake_key(const void *key);
const char *sched_process_state_name(process_state_t state);
uint64_t sched_ticks(void);
void sched_sleep(uint64_t ticks);
void sched_wait_input(void);
void sched_wait_input_timeout(uint64_t ticks);
void sched_wake_input_waiters(void);
int sched_suspend_process(uint64_t pid);
int sched_resume_process(uint64_t pid);
int sched_kill_process(uint64_t pid, uint64_t exit_code);
void sched_reap_orphans(void);




void sched_force_exit_current_with_children(uint64_t exit_code);
void sched_force_exit_all_user_processes(uint64_t exit_code);

#endif
