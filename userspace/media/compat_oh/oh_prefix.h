/* Force-included into every OpenH264 translation unit: ICDA's libc headers
 * with C linkage, and single-threaded stand-ins for the POSIX pieces the
 * decoder references (it never starts a thread with iThreadCount 0). */
#ifndef ICDA_OH_PREFIX_H
#define ICDA_OH_PREFIX_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __cplusplus
}
#endif
#include "oh_posix.h"
#endif
