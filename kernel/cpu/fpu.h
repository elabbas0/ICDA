#ifndef FPU_H
#define FPU_H

#include <stdint.h>

/* x87/SSE state for user threads.
 *
 * The kernel itself is built with -mno-sse and never touches the
 * FPU/XMM registers, so the only owner of that state is whichever user
 * thread last ran.  Saving it eagerly at every context switch
 * (fxsave prev / fxrstor next) is therefore sufficient and keeps the
 * trap path untouched.  Userspace is built with SSE2 enabled (the
 * x86-64 baseline), which the GUI stack needs for float animation
 * math and wide pixel loops. */

#define FPU_STATE_SIZE 512

/* Enable x87 + SSE (CR0.EM=0, CR0.MP=1, CR4.OSFXSR, CR4.OSXMMEXCPT),
 * reset the unit to its default control words and capture that state
 * as the template every new thread starts from.  Must run before the
 * first thread is created. */
void fpu_init(void);

/* Copy the clean default state into a new thread's save area
 * (512 bytes, 16-byte aligned). */
void fpu_state_init(uint8_t *area);

/* Save the live FPU/SSE registers into prev and load next. */
void fpu_switch(uint8_t *prev, const uint8_t *next);

#endif
