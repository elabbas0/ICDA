#ifndef I915_H
#define I915_H

#include <stdint.h>

/* Linux's i915 driver interface on ICDA's Intel GPU driver: what Mesa's
 * Intel driver (iris) and libdrm ask of /dev/dri/renderD128.  The Linux
 * personality (kernel/linux/lx_drm.c) owns the files; this owns the GPU. */

typedef struct i915_client i915_client_t;
typedef struct i915_obj i915_obj_t;

int i915_available(void);

i915_client_t *i915_open(void);
void i915_close(i915_client_t *c);

/* a DRM or i915 ioctl, number nr (the low byte of the request), its
 * argument copied into karg; returns 0 or minus a Linux errno */
int64_t i915_ioctl(i915_client_t *c, uint32_t nr, void *karg);

/* the frame behind a GEM mmap offset of this client (mmap of the DRM file) */
uint64_t i915_client_frame(i915_client_t *c, uint64_t off);

/* buffers shared between processes (PRIME, dma-buf files) */
i915_obj_t *i915_obj_from_handle(i915_client_t *c, uint32_t handle);   /* a new reference */
int i915_handle_for_obj(i915_client_t *c, i915_obj_t *o, uint32_t *handle);
void i915_obj_unref(i915_obj_t *o);
uint64_t i915_obj_frame(i915_obj_t *o, uint64_t off);
uint64_t i915_obj_size(i915_obj_t *o);
int i915_obj_wait(i915_obj_t *o, int64_t timeout_ns);

/* set by the Linux layer: a signal is waiting (waits end with EINTR); the monotonic clock */
extern int (*i915_interrupted)(void);
extern uint64_t (*i915_now_ns)(void);

#endif
