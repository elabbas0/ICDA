#ifndef DEVOPS_H
#define DEVOPS_H

/*
 * Device-ops boundary (P0 OS-ification, step 1).
 *
 * Syscall handlers must not call drivers directly. They go through the
 * per-surface function tables below, populated once at boot by
 * dev_populate() (kernel/dev/devnodes.c). Drivers stay in-kernel for
 * now — this establishes the indirection so they can move to userspace
 * servers later without touching the syscall layer again.
 *
 * The three surfaces mirror the /dev nodes (all VFS_NODE_FILE, listed
 * by `ls /dev` for discoverability; dispatch uses this registry, never
 * the VFS tree, so persistence/replay cannot affect it):
 *   /dev/console  keyboard/screen text surface
 *   /dev/input    keyboard input surface (mouse stays direct for now)
 *   /dev/fb0      framebuffer + GPU surface
 *
 * All functions run in syscall context. A NULL table or NULL entry
 * means dev_populate() failed — callers must return -1, never invent
 * behavior (populate failure is log-only, never a boot halt).
 */

#include <stdint.h>

typedef struct dev_calls {
    /* /dev/console */
    uint64_t (*con_write)(const char *text);
    void     (*con_clear)(void);
    void     (*con_backspace)(void);
    void     (*con_set_cursor)(int x, int y);
    void     (*con_get_cursor)(int *x, int *y);
    int      (*con_columns)(void);
    int      (*con_rows)(void);
    /* /dev/input */
    int      (*in_read_char)(void);
    /* /dev/fb0 (info/out are syscall_fb_info_t / syscall_gpu_info_t) */
    uint64_t (*fb_claim_map)(void *info);
    int      (*fb_claimed)(void);
    int      (*gpu_query)(void *out);
    int      (*gpu_present)(void);
    int      (*gpu_set_cursor)(int x, int y, const uint32_t *image,
                               int w, int h);
} dev_calls_t;

/* Register a table under an absolute path ("/dev/console"). The table
 * must point at static storage. Returns 0, or -1 when full. */
int devops_register(const char *path, const dev_calls_t *calls);

/* Look up a table by path. Returns NULL when absent. */
const dev_calls_t *devops_lookup(const char *path);

/* Typed accessors for the three known nodes (NULL when missing). */
static inline const dev_calls_t *dev_console(void) {
    const dev_calls_t *d = devops_lookup("/dev/console");
    return (d && d->con_write) ? d : 0;
}

static inline const dev_calls_t *dev_input(void) {
    const dev_calls_t *d = devops_lookup("/dev/input");
    return (d && d->in_read_char) ? d : 0;
}

static inline const dev_calls_t *dev_fb(void) {
    const dev_calls_t *d = devops_lookup("/dev/fb0");
    return (d && d->fb_claim_map) ? d : 0;
}

/* Create /dev + nodes and register the three tables. Idempotent:
 * safe to call when a persistfs replay already recreated /dev.
 * Returns 0 normally; nonzero is log-only for the caller. */
int dev_populate(void);

#endif
