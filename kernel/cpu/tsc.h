#ifndef TSC_H
#define TSC_H

#include <stdint.h>

/* Time-stamp counter clock.  It needs no interrupts, so it is safe for
 * waits during boot and in kernel threads that run with interrupts off
 * (where the PIT tick counter does not advance). */
void        tsc_init(void);
uint64_t    tsc_read(void);
uint64_t    tsc_hz(void);
uint64_t    tsc_us(void);               /* microseconds since an arbitrary start */
void        udelay(uint64_t us);
const char *tsc_source(void);           /* how the frequency was found */
uint64_t    tsc_centis(void);           /* real 10 ms ticks since tsc_init, 0 if unknown */

#endif
