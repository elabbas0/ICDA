#include "x25519.h"

/* Curve25519 (RFC 7748) field arithmetic mod 2^255-19 using five 51-bit
 * limbs with unsigned __int128 accumulators (x86-64 gcc provides this even
 * in freestanding mode). */

typedef unsigned __int128 u128;

#define FE_MASK 0x0007FFFFFFFFFFFFULL

typedef uint64_t fe[5];

static uint64_t ld8(const uint8_t *p) {
    uint64_t r = 0;
    for (int i = 7; i >= 0; i--) r = (r << 8) | p[i];
    return r;
}

static void st8(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static void fe_frombytes(fe h, const uint8_t s[32]) {
    h[0] = ld8(s) & FE_MASK;
    h[1] = (ld8(s + 6) >> 3) & FE_MASK;
    h[2] = (ld8(s + 12) >> 6) & FE_MASK;
    h[3] = (ld8(s + 19) >> 1) & FE_MASK;
    h[4] = (ld8(s + 24) >> 12) & FE_MASK; /* & FE_MASK also clears input bit 255 */
}

static void fe_tobytes(uint8_t s[32], fe h) {
    uint64_t q;
    uint64_t c;

    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    c = h[4] >> 51; h[4] &= FE_MASK; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;

    q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;

    h[0] += 19 * q;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    h[4] &= FE_MASK;

    st8(s,      h[0] | (h[1] << 51));
    st8(s + 8,  (h[1] >> 13) | (h[2] << 38));
    st8(s + 16, (h[2] >> 26) | (h[3] << 25));
    st8(s + 24, (h[3] >> 39) | (h[4] << 12));
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

/* h = f - g; adds 2p first so limbs stay positive (results < 2^52). */
static void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0xFFFFFFFFFFFDAULL - g[0];
    h[1] = f[1] + 0xFFFFFFFFFFFFEULL - g[1];
    h[2] = f[2] + 0xFFFFFFFFFFFFEULL - g[2];
    h[3] = f[3] + 0xFFFFFFFFFFFFEULL - g[3];
    h[4] = f[4] + 0xFFFFFFFFFFFFEULL - g[4];
}

static void fe_reduce(u128 t0, u128 t1, u128 t2, u128 t3, u128 t4, fe h) {
    uint64_t c;
    c = (uint64_t)(t0 >> 51); t1 += c; t0 &= FE_MASK;
    c = (uint64_t)(t1 >> 51); t2 += c; t1 &= FE_MASK;
    c = (uint64_t)(t2 >> 51); t3 += c; t2 &= FE_MASK;
    c = (uint64_t)(t3 >> 51); t4 += c; t3 &= FE_MASK;
    c = (uint64_t)(t4 >> 51); t0 += (u128)c * 19; t4 &= FE_MASK;
    c = (uint64_t)(t0 >> 51); t1 += c; t0 &= FE_MASK;
    h[0] = (uint64_t)t0; h[1] = (uint64_t)t1; h[2] = (uint64_t)t2;
    h[3] = (uint64_t)t3; h[4] = (uint64_t)t4;
}

static void fe_mul(fe h, const fe f, const fe g) {
    u128 t0 = (u128)f[0]*g[0] + 19*((u128)f[1]*g[4] + (u128)f[2]*g[3] + (u128)f[3]*g[2] + (u128)f[4]*g[1]);
    u128 t1 = (u128)f[0]*g[1] + (u128)f[1]*g[0] + 19*((u128)f[2]*g[4] + (u128)f[3]*g[3] + (u128)f[4]*g[2]);
    u128 t2 = (u128)f[0]*g[2] + (u128)f[1]*g[1] + (u128)f[2]*g[0] + 19*((u128)f[3]*g[4] + (u128)f[4]*g[3]);
    u128 t3 = (u128)f[0]*g[3] + (u128)f[1]*g[2] + (u128)f[2]*g[1] + (u128)f[3]*g[0] + 19*((u128)f[4]*g[4]);
    u128 t4 = (u128)f[0]*g[4] + (u128)f[1]*g[3] + (u128)f[2]*g[2] + (u128)f[3]*g[1] + (u128)f[4]*g[0];
    fe_reduce(t0, t1, t2, t3, t4, h);
}

static void fe_sqr(fe h, const fe f) {
    uint64_t d0 = f[0] * 2, d1 = f[1] * 2, d2 = f[2] * 2, d3 = f[3] * 2;
    u128 t0 = (u128)f[0]*f[0] + 19*((u128)d1*f[4] + (u128)d2*f[3]);
    u128 t1 = (u128)d0*f[1] + 19*((u128)d2*f[4] + (u128)f[3]*f[3]);
    u128 t2 = (u128)d0*f[2] + (u128)f[1]*f[1] + 19*((u128)d3*f[4]);
    u128 t3 = (u128)d0*f[3] + (u128)d1*f[2] + 19*((u128)f[4]*f[4]);
    u128 t4 = (u128)d0*f[4] + (u128)d1*f[3] + (u128)f[2]*f[2];
    fe_reduce(t0, t1, t2, t3, t4, h);
}

static void fe_mul121665(fe h, const fe f) {
    u128 t0 = (u128)f[0] * 121665;
    u128 t1 = (u128)f[1] * 121665;
    u128 t2 = (u128)f[2] * 121665;
    u128 t3 = (u128)f[3] * 121665;
    u128 t4 = (u128)f[4] * 121665;
    fe_reduce(t0, t1, t2, t3, t4, h);
}

/* h = z^(2^255 - 21) = z^(p-2) = 1/z */
static void fe_inv(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;

    fe_sqr(t0, z);
    fe_sqr(t1, t0);
    fe_sqr(t1, t1);
    fe_mul(t1, z, t1);          /* z^9 */
    fe_mul(t0, t0, t1);         /* z^11 */
    fe_sqr(t2, t0);
    fe_mul(t1, t1, t2);         /* z^31 = 2^5 - 1 */
    fe_sqr(t2, t1);
    for (i = 1; i < 5; i++) fe_sqr(t2, t2);
    fe_mul(t1, t2, t1);         /* 2^10 - 1 */
    fe_sqr(t2, t1);
    for (i = 1; i < 10; i++) fe_sqr(t2, t2);
    fe_mul(t2, t2, t1);         /* 2^20 - 1 */
    fe_sqr(t3, t2);
    for (i = 1; i < 20; i++) fe_sqr(t3, t3);
    fe_mul(t2, t3, t2);         /* 2^40 - 1 */
    fe_sqr(t2, t2);
    for (i = 1; i < 10; i++) fe_sqr(t2, t2);
    fe_mul(t1, t2, t1);         /* 2^50 - 1 */
    fe_sqr(t2, t1);
    for (i = 1; i < 50; i++) fe_sqr(t2, t2);
    fe_mul(t2, t2, t1);         /* 2^100 - 1 */
    fe_sqr(t3, t2);
    for (i = 1; i < 100; i++) fe_sqr(t3, t3);
    fe_mul(t2, t3, t2);         /* 2^200 - 1 */
    fe_sqr(t2, t2);
    for (i = 1; i < 50; i++) fe_sqr(t2, t2);
    fe_mul(t1, t2, t1);         /* 2^250 - 1 */
    fe_sqr(t1, t1);
    fe_sqr(t1, t1);             /* 2^252 - 4 */
    fe_sqr(t1, t1);             /* 2^253 - 8 */
    fe_sqr(t1, t1);             /* 2^254 - 16 */
    fe_sqr(t1, t1);             /* 2^255 - 32 */
    fe_mul(out, t1, t0);        /* 2^255 - 21 */
}

static void fe_cswap(fe f, fe g, unsigned int swap) {
    uint64_t mask = 0ULL - (uint64_t)swap;
    for (int i = 0; i < 5; i++) {
        uint64_t t = mask & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar_in[32], const uint8_t u_in[32]) {
    uint8_t k[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t0, t1;
    unsigned int swap = 0;

    for (int i = 0; i < 32; i++) k[i] = scalar_in[i];
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe_frombytes(x1, u_in);
    x2[0] = 1; x2[1] = x2[2] = x2[3] = x2[4] = 0;
    z2[0] = z2[1] = z2[2] = z2[3] = z2[4] = 0;
    for (int i = 0; i < 5; i++) { x3[i] = x1[i]; }
    z3[0] = 1; z3[1] = z3[2] = z3[3] = z3[4] = 0;

    for (int t = 254; t >= 0; t--) {
        unsigned int kt = (unsigned int)((k[t >> 3] >> (t & 7)) & 1);
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        fe_add(a, x2, z2);
        fe_sqr(aa, a);
        fe_sub(b, x2, z2);
        fe_sqr(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t0, da, cb);
        fe_sqr(x3, t0);
        fe_sub(t1, da, cb);
        fe_sqr(t1, t1);
        fe_mul(z3, x1, t1);
        fe_mul(x2, aa, bb);
        fe_mul121665(t0, e);
        fe_add(t0, aa, t0);
        fe_mul(z2, e, t0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

void x25519_public(uint8_t out[32], const uint8_t priv[32]) {
    uint8_t base[32];
    for (int i = 0; i < 32; i++) base[i] = 0;
    base[0] = 9;
    x25519_scalarmult(out, priv, base);
}

int x25519_shared(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]) {
    x25519_scalarmult(out, priv, peer_pub);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc == 0 ? -1 : 0;
}
