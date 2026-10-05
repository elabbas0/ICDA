#ifndef SURFER_SMATH_H
#define SURFER_SMATH_H

/* The few math functions Surfer and its bundled libraries need; ICDA's libc
 * has no libm. */

static inline double sm_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline double sm_floor(double x) {
    long long i = (long long)x;
    return (double)(x < (double)i ? i - 1 : i);
}

static inline double sm_ceil(double x) {
    long long i = (long long)x;
    return (double)(x > (double)i ? i + 1 : i);
}

static inline double sm_fabs(double x) {
    return x < 0 ? -x : x;
}

static inline double sm_fmod(double x, double y) {
    if (y == 0) return 0;
    return x - (double)(long long)(x / y) * y;
}

static inline double sm_exp(double x) {
    /* range reduction by powers of two plus a short series */
    int k = (int)(x / 0.6931471805599453);
    double r = x - k * 0.6931471805599453, term = 1, sum = 1;
    for (int i = 1; i < 18; i++) {
        term *= r / i;
        sum += term;
    }
    while (k > 0) { sum *= 2; k--; }
    while (k < 0) { sum /= 2; k++; }
    return sum;
}

static inline double sm_log(double x) {
    /* x = m * 2^e, then atanh series for log(m) */
    int e = 0;
    double y, y2, sum = 0, term;
    if (x <= 0) return -1e300;
    while (x > 2) { x /= 2; e++; }
    while (x < 1) { x *= 2; e--; }
    y = (x - 1) / (x + 1);
    y2 = y * y;
    term = y;
    for (int i = 1; i < 40; i += 2) {
        sum += term / i;
        term *= y2;
    }
    return 2 * sum + e * 0.6931471805599453;
}

static inline double sm_pow(double x, double y) {
    if (x == 0) return 0;
    if (y == (double)(long long)y && y >= 0 && y < 64) {
        double r = 1;
        for (long long i = 0; i < (long long)y; i++) r *= x;
        return r;
    }
    return sm_exp(y * sm_log(x));
}

static inline double sm_cos(double x) {
    double t, sum, x2;
    const double tau = 6.283185307179586;
    x = sm_fmod(x, tau);
    if (x < 0) x += tau;
    x2 = x * x;
    t = 1;
    sum = 1;
    for (int i = 1; i < 20; i++) {
        t *= -x2 / ((2 * i - 1) * (2 * i));
        sum += t;
    }
    return sum;
}

static inline double sm_acos(double x) {
    /* acos(x) = pi/2 - asin(x), asin by Newton on sin */
    double a = x, s;
    if (x <= -1) return 3.141592653589793;
    if (x >= 1) return 0;
    for (int i = 0; i < 30; i++) {
        s = sm_cos(1.5707963267948966 - a);           /* sin(a) */
        a -= (s - x) / (sm_cos(a) + 1e-12);
    }
    return 1.5707963267948966 - a;
}

#endif
