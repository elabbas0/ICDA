/* ECDSA verification on NIST P-256 and P-384: Montgomery field arithmetic on
 * 64-bit limbs (little-endian), Jacobian coordinates, Shamir's trick.  Only
 * public data is processed, so nothing here needs to be constant time. */
#include "ecdsa.h"

#define MAXL 6

typedef unsigned __int128 u128;

typedef struct {
    int      n;              /* limbs */
    uint64_t m[MAXL];        /* modulus */
    uint64_t minv;           /* -m^-1 mod 2^64 */
    uint64_t rr[MAXL];       /* R^2 mod m */
    uint64_t one[MAXL];      /* R mod m */
} mont_t;

typedef struct {
    int      bytes;
    mont_t   p, n;
    uint64_t b[MAXL];        /* Montgomery form */
    uint64_t gx[MAXL], gy[MAXL];
} curve_t;

typedef struct {
    uint64_t x[MAXL], y[MAXL], z[MAXL];
    int inf;
} pt_t;

static int cmp(const uint64_t *a, const uint64_t *b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static int is_zero(const uint64_t *a, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i]) return 0;
    }
    return 1;
}

static uint64_t sub_raw(uint64_t *r, const uint64_t *a, const uint64_t *b, int n) {
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        u128 d = (u128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) & 1;
    }
    return borrow;
}

static uint64_t add_raw(uint64_t *r, const uint64_t *a, const uint64_t *b, int n) {
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        u128 s = (u128)a[i] + b[i] + carry;
        r[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    return carry;
}

static void add_mod(const mont_t *m, uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[MAXL];
    uint64_t carry = add_raw(t, a, b, m->n);
    if (carry || cmp(t, m->m, m->n) >= 0) sub_raw(t, t, m->m, m->n);
    for (int i = 0; i < m->n; i++) r[i] = t[i];
}

static void sub_mod(const mont_t *m, uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[MAXL];
    if (sub_raw(t, a, b, m->n)) add_raw(t, t, m->m, m->n);
    for (int i = 0; i < m->n; i++) r[i] = t[i];
}

/* CIOS Montgomery multiplication: r = a * b / R mod m */
static void mul(const mont_t *m, uint64_t *r, const uint64_t *a, const uint64_t *b) {
    uint64_t t[MAXL + 2] = { 0 };
    int n = m->n;
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0, u;
        for (int j = 0; j < n; j++) {
            u128 s = (u128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        {
            u128 s = (u128)t[n] + carry;
            t[n] = (uint64_t)s;
            t[n + 1] = (uint64_t)(s >> 64);
        }
        u = t[0] * m->minv;
        carry = 0;
        for (int j = 0; j < n; j++) {
            u128 s = (u128)u * m->m[j] + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        {
            u128 s = (u128)t[n] + carry;
            t[n] = (uint64_t)s;
            t[n + 1] += (uint64_t)(s >> 64);
        }
        for (int j = 0; j <= n; j++) t[j] = t[j + 1];
        t[n + 1] = 0;
    }
    if (t[n] || cmp(t, m->m, n) >= 0) sub_raw(t, t, m->m, n);
    for (int i = 0; i < n; i++) r[i] = t[i];
}

static void to_mont(const mont_t *m, uint64_t *r, const uint64_t *a) {
    mul(m, r, a, m->rr);
}

static void from_mont(const mont_t *m, uint64_t *r, const uint64_t *a) {
    uint64_t one[MAXL] = { 1 };
    mul(m, r, a, one);
}

/* r = a^(m-2) (inverse for prime m), all in Montgomery form */
static void inv(const mont_t *m, uint64_t *r, const uint64_t *a) {
    uint64_t e[MAXL], two[MAXL] = { 2 }, acc[MAXL];
    sub_raw(e, m->m, two, m->n);
    for (int i = 0; i < m->n; i++) acc[i] = m->one[i];
    for (int i = m->n * 64 - 1; i >= 0; i--) {
        mul(m, acc, acc, acc);
        if ((e[i / 64] >> (i % 64)) & 1) mul(m, acc, acc, a);
    }
    for (int i = 0; i < m->n; i++) r[i] = acc[i];
}

static void hex_limbs(uint64_t *out, const char *hex, int n) {
    for (int i = 0; i < n; i++) out[i] = 0;
    for (int k = 0; hex[k]; k++) {
        int c = hex[k], v = c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
        for (int i = n - 1; i > 0; i--) out[i] = (out[i] << 4) | (out[i - 1] >> 60);
        out[0] = (out[0] << 4) | (uint64_t)v;
    }
}

static void mont_init(mont_t *m, const char *hex, int n) {
    uint64_t x = 1;
    m->n = n;
    hex_limbs(m->m, hex, n);
    for (int i = 0; i < 6; i++) x *= 2 - m->m[0] * x;     /* Newton: x = m^-1 mod 2^64 */
    m->minv = (uint64_t)0 - x;
    /* R mod m and R^2 mod m by doubling */
    for (int i = 0; i < MAXL; i++) m->rr[i] = 0;
    m->rr[0] = 1;
    for (int i = 0; i < n * 64 * 2; i++) {
        add_mod(m, m->rr, m->rr, m->rr);
        if (i == n * 64 - 1) {
            for (int k = 0; k < n; k++) m->one[k] = m->rr[k];
        }
    }
}

static curve_t P256, P384;
static int curves_ready;

static void curve_setup(curve_t *c, int n, const char *p, const char *ord, const char *b,
                        const char *gx, const char *gy) {
    uint64_t t[MAXL];
    c->bytes = n * 8;
    mont_init(&c->p, p, n);
    mont_init(&c->n, ord, n);
    hex_limbs(t, b, n);
    to_mont(&c->p, c->b, t);
    hex_limbs(t, gx, n);
    to_mont(&c->p, c->gx, t);
    hex_limbs(t, gy, n);
    to_mont(&c->p, c->gy, t);
}

static void curves_init(void) {
    if (curves_ready) return;
    curve_setup(&P256, 4,
                "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
                "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
                "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
                "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    curve_setup(&P384, 6,
                "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
                "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
                "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
                "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
                "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
    curves_ready = 1;
}

/* Jacobian doubling for a = -3 */
static void pt_double(const curve_t *c, pt_t *r, const pt_t *a) {
    const mont_t *m = &c->p;
    uint64_t delta[MAXL], gamma[MAXL], beta[MAXL], alpha[MAXL], t1[MAXL], t2[MAXL];
    if (a->inf || is_zero(a->y, m->n)) {
        r->inf = 1;
        return;
    }
    mul(m, delta, a->z, a->z);
    mul(m, gamma, a->y, a->y);
    mul(m, beta, a->x, gamma);
    sub_mod(m, t1, a->x, delta);
    add_mod(m, t2, a->x, delta);
    mul(m, alpha, t1, t2);
    add_mod(m, t1, alpha, alpha);
    add_mod(m, alpha, t1, alpha);                 /* alpha = 3(x-d)(x+d) */
    add_mod(m, t1, a->y, a->z);
    mul(m, t1, t1, t1);
    sub_mod(m, t1, t1, gamma);
    sub_mod(m, r->z, t1, delta);                  /* z3 = (y+z)^2 - gamma - delta */
    mul(m, t1, alpha, alpha);
    add_mod(m, t2, beta, beta);
    add_mod(m, t2, t2, t2);                       /* 4 beta */
    sub_mod(m, t1, t1, t2);
    sub_mod(m, r->x, t1, t2);                     /* x3 = alpha^2 - 8 beta */
    sub_mod(m, t2, t2, r->x);                     /* 4 beta - x3 */
    mul(m, t1, alpha, t2);
    mul(m, gamma, gamma, gamma);
    add_mod(m, gamma, gamma, gamma);
    add_mod(m, gamma, gamma, gamma);
    add_mod(m, gamma, gamma, gamma);              /* 8 gamma^2 */
    sub_mod(m, r->y, t1, gamma);
    r->inf = 0;
}

static void pt_add(const curve_t *c, pt_t *r, const pt_t *a, const pt_t *b) {
    const mont_t *m = &c->p;
    uint64_t z1z1[MAXL], z2z2[MAXL], u1[MAXL], u2[MAXL], s1[MAXL], s2[MAXL], h[MAXL], rr[MAXL];
    uint64_t hh[MAXL], hhh[MAXL], v[MAXL], t[MAXL];
    if (a->inf) {
        *r = *b;
        return;
    }
    if (b->inf) {
        *r = *a;
        return;
    }
    mul(m, z1z1, a->z, a->z);
    mul(m, z2z2, b->z, b->z);
    mul(m, u1, a->x, z2z2);
    mul(m, u2, b->x, z1z1);
    mul(m, s1, a->y, b->z);
    mul(m, s1, s1, z2z2);
    mul(m, s2, b->y, a->z);
    mul(m, s2, s2, z1z1);
    sub_mod(m, h, u2, u1);
    sub_mod(m, rr, s2, s1);
    if (is_zero(h, m->n)) {
        if (is_zero(rr, m->n)) pt_double(c, r, a);
        else r->inf = 1;
        return;
    }
    mul(m, hh, h, h);
    mul(m, hhh, h, hh);
    mul(m, v, u1, hh);
    mul(m, t, rr, rr);
    sub_mod(m, t, t, hhh);
    sub_mod(m, t, t, v);
    sub_mod(m, t, t, v);                          /* x3 */
    sub_mod(m, v, v, t);
    mul(m, v, rr, v);
    mul(m, s1, s1, hhh);
    sub_mod(m, r->y, v, s1);
    mul(m, r->z, a->z, b->z);
    mul(m, r->z, r->z, h);
    for (int i = 0; i < m->n; i++) r->x[i] = t[i];
    r->inf = 0;
}

static void load_be(uint64_t *out, const uint8_t *in, int bytes, int n) {
    for (int i = 0; i < n; i++) out[i] = 0;
    for (int i = 0; i < bytes; i++) {
        int bit = (bytes - 1 - i) * 8;
        out[bit / 64] |= (uint64_t)in[i] << (bit % 64);
    }
}

/* Reads a DER INTEGER into limbs; returns bytes consumed or -1. */
static int der_int(const uint8_t *p, size_t len, uint64_t *out, int bytes, int n) {
    size_t l, total;
    const uint8_t *v;
    if (len < 2 || p[0] != 0x02) return -1;
    l = p[1];
    if ((l & 0x80) || l == 0 || 2 + l > len) return -1;
    total = 2 + l;
    v = p + 2;
    while (l > 0 && v[0] == 0) {
        v++;
        l--;
    }
    if ((int)l > bytes) return -1;
    load_be(out, v, (int)l, n);
    return (int)total;
}

int ecdsa_verify(int curve, const uint8_t *pub, size_t pub_len, const uint8_t *hash, size_t hash_len,
                 const uint8_t *sig, size_t sig_len) {
    curve_t *c;
    const mont_t *fp, *fn;
    uint64_t r[MAXL], s[MAXL], e[MAXL], t[MAXL], w[MAXL], u1[MAXL], u2[MAXL];
    pt_t q, g, gq, acc;
    int n, bytes;
    curves_init();
    c = curve == EC_P256 ? &P256 : curve == EC_P384 ? &P384 : 0;
    if (!c) return -1;
    fp = &c->p;
    fn = &c->n;
    n = fp->n;
    bytes = c->bytes;
    /* signature */
    {
        size_t seq_len, off;
        int k;
        if (sig_len < 2 || sig[0] != 0x30) return -1;
        seq_len = sig[1];
        off = 2;
        if (seq_len & 0x80) {
            if (seq_len != 0x81 || sig_len < 3) return -1;
            seq_len = sig[2];
            off = 3;
        }
        if (off + seq_len > sig_len) return -1;
        k = der_int(sig + off, sig_len - off, r, bytes, n);
        if (k < 0) return -1;
        off += (size_t)k;
        if (der_int(sig + off, sig_len - off, s, bytes, n) < 0) return -1;
    }
    if (is_zero(r, n) || is_zero(s, n) || cmp(r, fn->m, n) >= 0 || cmp(s, fn->m, n) >= 0) return -1;
    /* public point */
    if (pub_len != (size_t)(1 + 2 * bytes) || pub[0] != 0x04) return -1;
    load_be(t, pub + 1, bytes, n);
    if (cmp(t, fp->m, n) >= 0) return -1;
    to_mont(fp, q.x, t);
    load_be(t, pub + 1 + bytes, bytes, n);
    if (cmp(t, fp->m, n) >= 0) return -1;
    to_mont(fp, q.y, t);
    for (int i = 0; i < n; i++) q.z[i] = fp->one[i];
    q.inf = 0;
    {
        /* on-curve check: y^2 = x^3 - 3x + b */
        uint64_t lhs[MAXL], rhs[MAXL], x3[MAXL];
        mul(fp, lhs, q.y, q.y);
        mul(fp, x3, q.x, q.x);
        mul(fp, x3, x3, q.x);
        sub_mod(fp, rhs, x3, q.x);
        sub_mod(fp, rhs, rhs, q.x);
        sub_mod(fp, rhs, rhs, q.x);
        add_mod(fp, rhs, rhs, c->b);
        if (cmp(lhs, rhs, n) != 0) return -1;
    }
    /* e = leftmost bits of the hash, reduced mod n */
    if (hash_len > (size_t)bytes) hash_len = (size_t)bytes;
    load_be(e, hash, (int)hash_len, n);
    if (cmp(e, fn->m, n) >= 0) sub_raw(e, e, fn->m, n);
    /* w = s^-1, u1 = e w, u2 = r w (mod n) */
    to_mont(fn, t, s);
    inv(fn, w, t);
    to_mont(fn, t, e);
    mul(fn, u1, t, w);
    from_mont(fn, u1, u1);
    to_mont(fn, t, r);
    mul(fn, u2, t, w);
    from_mont(fn, u2, u2);
    /* acc = u1 G + u2 Q */
    for (int i = 0; i < n; i++) {
        g.x[i] = c->gx[i];
        g.y[i] = c->gy[i];
        g.z[i] = fp->one[i];
    }
    g.inf = 0;
    pt_add(c, &gq, &g, &q);
    acc.inf = 1;
    for (int i = n * 64 - 1; i >= 0; i--) {
        int b1 = (int)((u1[i / 64] >> (i % 64)) & 1), b2 = (int)((u2[i / 64] >> (i % 64)) & 1);
        pt_double(c, &acc, &acc);
        if (b1 && b2) pt_add(c, &acc, &acc, &gq);
        else if (b1) pt_add(c, &acc, &acc, &g);
        else if (b2) pt_add(c, &acc, &acc, &q);
    }
    if (acc.inf) return -1;
    /* affine x = X / Z^2, compare with r mod n */
    {
        uint64_t zi[MAXL], x[MAXL];
        inv(fp, zi, acc.z);
        mul(fp, zi, zi, zi);
        mul(fp, x, acc.x, zi);
        from_mont(fp, x, x);
        if (cmp(x, fn->m, n) >= 0) sub_raw(x, x, fn->m, n);
        return cmp(x, r, n) == 0 ? 0 : -1;
    }
}
