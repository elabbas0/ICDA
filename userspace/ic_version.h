/*
 * ic_version.h - ICDA library version information.
 *
 * Single source of truth: version.h (root).  This header aliases the
 * IC_VERSION_* names so existing userspace code keeps compiling.
 */
#ifndef USERSPACE_IC_VERSION_H
#define USERSPACE_IC_VERSION_H

#include "version.h"   /* ICDA_VERSION_* definitions (root, -I. covers it) */

/* Alias IC_VERSION_* -> ICDA_VERSION_* (keep existing names/signatures). */
#define IC_VERSION_MAJOR   ICDA_VERSION_MAJOR
#define IC_VERSION_MINOR   ICDA_VERSION_MINOR
#define IC_VERSION_PATCH   ICDA_VERSION_PATCH
#define IC_VERSION_U32     ICDA_VERSION_U32
#define IC_VERSION_STRING  ICDA_VERSION_STRING
#define IC_VERSION_BANNER  ICDA_VERSION_BANNER

#endif /* USERSPACE_IC_VERSION_H */
