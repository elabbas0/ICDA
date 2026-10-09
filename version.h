#ifndef ICDA_VERSION_H
#define ICDA_VERSION_H

#include <stdint.h>



#define ICDA_VERSION_MAJOR  1
#define ICDA_VERSION_MINOR  8
#define ICDA_VERSION_PATCH  8



#define ICDA_VERSION_U32 \
    (((uint32_t)ICDA_VERSION_MAJOR << 24) | \
     ((uint32_t)ICDA_VERSION_MINOR <<  8) | \
     ((uint32_t)ICDA_VERSION_PATCH))

#define ICDA_VERSION_STRING "1.8.8"



#define ICDA_VERSION_BANNER "ICDA " ICDA_VERSION_STRING

#endif 
