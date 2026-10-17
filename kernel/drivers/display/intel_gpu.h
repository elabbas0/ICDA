#ifndef INTEL_GPU_H
#define INTEL_GPU_H

#include <stdint.h>

/* Intel integrated graphics, 9th generation (Skylake, Kaby Lake, Coffee
 * Lake, Comet Lake: HD/UHD 5xx/6xx).  Stage 1: the GPU is found, woken and
 * runs a command of ours on its render engine. */
int intel_gpu_init(int disabled);

/* /dev/gpu: what was found and how the self-test went, as text */
uint64_t intel_gpu_node_read(char *buf, uint64_t cap);

#endif
