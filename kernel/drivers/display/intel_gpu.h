#ifndef INTEL_GPU_H
#define INTEL_GPU_H

#include <stdint.h>

/* Intel integrated graphics, 9th generation (Skylake, Kaby Lake, Coffee
 * Lake, Comet Lake: HD/UHD 5xx/6xx).  Stage 1: the GPU is found, woken and
 * runs a command of ours on its render engine. */
int intel_gpu_init(int disabled);

/* /dev/gpu: what was found and how the self-test went, as text */
uint64_t intel_gpu_node_read(char *buf, uint64_t cap);



/* the hardware, for the Linux driver interface (i915.c) */
#include "../pci/pci.h"
uint32_t igpu_rd(uint32_t reg);
void igpu_wr(uint32_t reg, uint32_t v);
const pci_device_t *igpu_pci(void);
void igpu_report(const char *s);                /* a line in /dev/gpu and on the serial port */
void igpu_flush(const void *p, uint64_t bytes); /* CPU caches out to memory */
void igpu_context_regs(uint32_t *regs);         /* a render context's register list (page 1) */
int igpu_reset_render(void);
void igpu_setup_ppat(void);
int64_t igpu_ggtt_window(uint64_t pages);
void igpu_ggtt_set(uint64_t entry, uint64_t phys);
void igpu_ggtt_flush(void);

/* i915.c */
int i915_init(void);
#endif
