#ifndef ICDA_MATH_H
#define ICDA_MATH_H

/* ICDA libm: double/float functions from musl (userspace/libc/math). */

typedef double double_t;
typedef float  float_t;

#define NAN       (__builtin_nanf(""))
#define INFINITY  (__builtin_inff())
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define FP_ILOGBNAN (-1 - 0x7fffffff)
#define FP_ILOGB0   FP_ILOGBNAN

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling 2

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define isunordered(a, b)   __builtin_isunordered(a, b)
#define isless(a, b)        __builtin_isless(a, b)
#define islessequal(a, b)   __builtin_islessequal(a, b)
#define isgreater(a, b)     __builtin_isgreater(a, b)
#define isgreaterequal(a, b) __builtin_isgreaterequal(a, b)
#define islessgreater(a, b) __builtin_islessgreater(a, b)

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

double acos(double);  double acosh(double); double asin(double);  double asinh(double);
double atan(double);  double atan2(double, double); double atanh(double); double cbrt(double);
double ceil(double);  double copysign(double, double); double cos(double); double cosh(double);
double exp(double);   double exp2(double);  double expm1(double); double fabs(double);
double floor(double); double fmax(double, double); double fmin(double, double);
double fmod(double, double); double frexp(double, int *); double hypot(double, double);
double ldexp(double, int); double log(double); double log10(double); double log1p(double);
double log2(double);  long lrint(double); double modf(double, double *); double nearbyint(double);
double pow(double, double); double rint(double); double round(double); double scalbn(double, int);
double sin(double);   double sinh(double);  double sqrt(double);  double tan(double);
double tanh(double);  double trunc(double);

float  fabsf(float);  float sqrtf(float);   float floorf(float);  float ceilf(float);
float  sinf(float);   float cosf(float);    float tanf(float);    float acosf(float);
float  asinf(float);  float atanf(float);   float atan2f(float, float); float fmodf(float, float);
float  roundf(float); float powf(float, float); float expf(float); float logf(float);

/* long double variants musl's internals declare; ICDA does not provide them */
long double fabsl(long double);
long double sqrtl(long double);

#endif
