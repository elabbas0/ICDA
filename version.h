#ifndef ICDA_VERSION_H
#define ICDA_VERSION_H

#include <stdint.h>

/* ---- current release --------------------------------------------------- */

#define ICDA_VERSION_MAJOR  1
#define ICDA_VERSION_MINOR  3
#define ICDA_VERSION_PATCH  3

/* Pack into a single uint32_t:  (major << 24) | (minor << 8) | patch       *
 * Range: major 0-255, minor 0-255, patch 0-255.                            */
#define ICDA_VERSION_U32 \
    (((uint32_t)ICDA_VERSION_MAJOR << 24) | \
     ((uint32_t)ICDA_VERSION_MINOR <<  8) | \
     ((uint32_t)ICDA_VERSION_PATCH))

#define ICDA_VERSION_STRING "1.3.3"

/* ---- human-readable banner (for -v flags / logging) --------------------- */

#define ICDA_VERSION_BANNER "ICDA " ICDA_VERSION_STRING

#endif /* ICDA_VERSION_H */
