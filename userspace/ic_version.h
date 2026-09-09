/*
 * ic_version.h - ICDA library version information (self-contained).
 *
 * Version scheme: MAJOR.MINOR.PATCH (semantic-style)
 *   MAJOR  — ABI-breaking overhaul (kernel/userspace contract changes)
 *   MINOR  — big feature update, append-only, no breakage
 *   PATCH  — hotfix / bugfix only
 *
 * This header is public API.  Any file may #include "ic_version.h"
 * without pulling in the rest of libicda.
 */
#ifndef USERSPACE_IC_VERSION_H
#define USERSPACE_IC_VERSION_H

#include <stdint.h>

/* ---- current release --------------------------------------------------- */

#define IC_VERSION_MAJOR  1
#define IC_VERSION_MINOR  3
#define IC_VERSION_PATCH  0

/* Pack into a single uint32_t:  (major << 24) | (minor << 8) | patch       *
 * Range: major 0-255, minor 0-255, patch 0-255.                            *
 * Note: minor << 8 leaves bits 16–23 zero (reserved for future use).       */
#define IC_VERSION_U32 \
    (((uint32_t)IC_VERSION_MAJOR << 24) | \
     ((uint32_t)IC_VERSION_MINOR <<  8) | \
     ((uint32_t)IC_VERSION_PATCH))

#define IC_VERSION_STRING "1.3.0"

/* ---- human-readable banner (for -v flags / logging) --------------------- */

#define IC_VERSION_BANNER "ICDA " IC_VERSION_STRING " (libicda)"

#endif /* USERSPACE_IC_VERSION_H */
