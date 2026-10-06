/* Single-precision functions as wrappers over the double versions (enough
 * for nanosvg and the media decoders; not tuned like musl's own sinf etc). */
#include <math.h>

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float roundf(float x) { return (float)round(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float sqrtf(float x) { return (float)sqrt(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float fabsf(float x) { return x < 0 ? -x : x; }
