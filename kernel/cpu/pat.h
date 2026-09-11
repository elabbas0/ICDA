#ifndef PAT_H
#define PAT_H

#include <stdint.h>

/* Program PAT MSR entry for write-combining (WC) framebuffer access.
 * CPUID-gated: silently returns 0 (no change) if PAT is not supported.
 * Single-CPU (no SMP concerns).
 * Returns 1 if PAT WC was enabled, 0 if skipped. */
int pat_init_wc(void);

/* Returns non-zero after pat_init_wc() successfully programmed PAT slot 4
 * to WC.  Used by VMM and devnodes to decide between WC (fast) and
 * UC (PCD+PWT, safe fallback) for framebuffer/device mappings. */
int pat_wc_available(void);

#endif
