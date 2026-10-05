#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "gdt.h"

#define SMP_MAX_CPUS 16
#define SMP_TIMER_VECTOR 48

struct thread;
struct addr_space_s;

/* Per-CPU block.  The first three fields are read by the syscall entry stub
 * through GS (swapgs), so their offsets are fixed. */
typedef struct cpu {
    uint64_t         self;        /* +0  */
    uint64_t         kstack_top;  /* +8  kernel stack for the syscall instruction */
    uint64_t         user_rsp;    /* +16 scratch for the user stack pointer */
    uint32_t         index;
    uint32_t         apic_id;
    struct thread   *current;
    struct thread   *idle;
    void            *active_as;
    volatile int     online;
    int              bkl_held;
    uint64_t         ticks;
    struct tss      *tss;
    struct gdt_entry gdt[7];
    struct gdt_ptr   gp;
    struct tss       tss_store;
} cpu_t;

extern cpu_t smp_cpus[SMP_MAX_CPUS];

cpu_t   *this_cpu(void);
uint32_t smp_cpu_count(void);
void     smp_init(void *multiboot_info);

/* Big kernel lock: held by whichever CPU is executing kernel code.  Kernel
 * entry takes it if this CPU does not hold it yet (returns 1 if it did). */
int      bkl_enter(void);
void     bkl_exit(void);

#endif
