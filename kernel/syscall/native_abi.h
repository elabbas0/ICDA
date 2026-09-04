#ifndef NATIVE_ABI_H
#define NATIVE_ABI_H

/*
 * Frozen native syscall ABI, version 1.
 *
 * The native numbers SYS_CONSOLE_WRITE..SYS_PROC_STATS (0..69) are a
 * stable contract with userspace (userspace/icda_sys.h mirrors them).
 * Do NOT renumber, remove, or append without bumping
 * ICDA_NATIVE_ABI_VERSION and updating docs/ABI-v1.md — the Linux
 * personality and every .app binary depend on these values.
 *
 * Enforcement:
 *  - compile time: _Static_asserts in kernel/syscall/syscall.c,
 *  - CI/dev time: scripts/check-abi.sh diffs the two headers.
 */
#define ICDA_NATIVE_ABI_VERSION 1

/* Count of native calls: numbers 0..69 inclusive. */
#define ICDA_NATIVE_SYS_MAX 70

#endif
