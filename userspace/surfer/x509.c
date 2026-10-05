/* X.509 certificate parsing and web PKI chain verification for Surfer's TLS:
 * DER, RSA PKCS#1 v1.5 / PSS and ECDSA P-256 / P-384 signatures, a root
 * store loaded from a PEM bundle, hostname and validity checks. */
#include "x509.h"
#include "sha256.h"
#include "sha1.h"

#include "crypto/sha512.h"
#include "crypto/ecdsa.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- DER ---------------------------------------------------------------- */

typedef struct {
    uint8_t tag;
    span_t  val;     /* contents */
    span_t  full;    /* tag + length + contents */
} tlv_t;

static int der_read(span_t *cur, tlv_t *out) {
    const uint8_t *p = cur->p;
    size_t left = cur->len, len, hdr = 2;
    if (left < 2) return -1;
    out->tag = p[0];
    len = p[1];
    if (len & 0x80) {
        int n = (int)(len & 0x7F);
        if (n < 1 || n > 4 || left < (size_t)(2 + n)) return -1;
        len = 0;
        for (int i = 0; i < n; i++) len = (len << 8) | p[2 + i];
        hdr = 2 + (size_t)n;
    }
    if (len > left - hdr) return -1;
    out->val.p = p + hdr;
    out->val.len = len;
    out->full.p = p;
    out->full.len = hdr + len;
    cur->p += hdr + len;
    cur->len -= hdr + len;
    return 0;
}

static int der_expect(span_t *cur, uint8_t tag, tlv_t *out) {
    return der_read(cur, out) == 0 && out->tag == tag ? 0 : -1;
}

static int oid_is(const span_t *v, const uint8_t *oid, size_t n) {
    return v->len == n && memcmp(v->p, oid, n) == 0;
}

static const uint8_t OID_RSA[]        = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t OID_RSA_SHA1[]   = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05 };
static const uint8_t OID_RSA_SHA256[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const uint8_t OID_RSA_SHA384[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C };
static const uint8_t OID_RSA_SHA512[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D };
static const uint8_t OID_EC_KEY[]     = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t OID_ECDSA_256[]  = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const uint8_t OID_ECDSA_384[]  = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03 };
static const uint8_t OID_ECDSA_512[]  = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04 };
static const uint8_t OID_P256[]       = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[]       = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_SAN[]        = { 0x55, 0x1D, 0x11 };
static const uint8_t OID_BASIC[]      = { 0x55, 0x1D, 0x13 };

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    int64_t era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t parse_time(const tlv_t *t) {
    const uint8_t *p = t->val.p;
    int y, i = 0;
    int v[6];
    if (t->tag == 0x17 && t->val.len >= 12) {
        y = (p[0] - '0') * 10 + (p[1] - '0');
        y += y < 50 ? 2000 : 1900;
        i = 2;
    } else if (t->tag == 0x18 && t->val.len >= 14) {
        y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        i = 4;
    } else {
        return 0;
    }
    for (int k = 0; k < 5; k++, i += 2) v[k] = (p[i] - '0') * 10 + (p[i + 1] - '0');
    return days_from_civil(y, (unsigned)v[0], (unsigned)v[1]) * 86400 + v[2] * 3600 + v[3] * 60 + v[4];
}

static void sig_alg(const span_t *oid, cert_t *c) {
    c->sig_hash = HASH_NONE;
    if (oid_is(oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))) { c->sig_hash = HASH_SHA256; c->sig_rsa = 1; }
    else if (oid_is(oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384))) { c->sig_hash = HASH_SHA384; c->sig_rsa = 1; }
    else if (oid_is(oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512))) { c->sig_hash = HASH_SHA512; c->sig_rsa = 1; }
    else if (oid_is(oid, OID_RSA_SHA1, sizeof(OID_RSA_SHA1))) { c->sig_hash = HASH_SHA1; c->sig_rsa = 1; }
    else if (oid_is(oid, OID_ECDSA_256, sizeof(OID_ECDSA_256))) { c->sig_hash = HASH_SHA256; c->sig_rsa = 0; }
    else if (oid_is(oid, OID_ECDSA_384, sizeof(OID_ECDSA_384))) { c->sig_hash = HASH_SHA384; c->sig_rsa = 0; }
    else if (oid_is(oid, OID_ECDSA_512, sizeof(OID_ECDSA_512))) { c->sig_hash = HASH_SHA512; c->sig_rsa = 0; }
}

static int parse_spki(span_t spki, cert_t *c) {
    tlv_t alg, oid, param, bits;
    span_t a;
    if (der_expect(&spki, 0x30, &alg) != 0 || der_expect(&spki, 0x03, &bits) != 0 || bits.val.len < 2) return -1;
    a = alg.val;
    if (der_expect(&a, 0x06, &oid) != 0) return -1;
    if (oid_is(&oid.val, OID_RSA, sizeof(OID_RSA))) {
        span_t key = { bits.val.p + 1, bits.val.len - 1 };
        tlv_t seq, n, e;
        span_t s;
        if (der_expect(&key, 0x30, &seq) != 0) return -1;
        s = seq.val;
        if (der_expect(&s, 0x02, &n) != 0 || der_expect(&s, 0x02, &e) != 0) return -1;
        c->key_type = KEY_RSA;
        c->rsa_n = n.val;
        c->rsa_e = e.val;
        while (c->rsa_n.len && c->rsa_n.p[0] == 0) {
            c->rsa_n.p++;
            c->rsa_n.len--;
        }
        return 0;
    }
    if (oid_is(&oid.val, OID_EC_KEY, sizeof(OID_EC_KEY)) && der_expect(&a, 0x06, &param) == 0) {
        if (oid_is(&param.val, OID_P256, sizeof(OID_P256))) c->key_type = KEY_EC_P256;
        else if (oid_is(&param.val, OID_P384, sizeof(OID_P384))) c->key_type = KEY_EC_P384;
        else return -1;
        c->ec_point.p = bits.val.p + 1;
        c->ec_point.len = bits.val.len - 1;
        return 0;
    }
    return -1;
}

static void parse_extensions(span_t exts, cert_t *c) {
    tlv_t seq;
    if (der_expect(&exts, 0x30, &seq) != 0) return;
    exts = seq.val;
    while (exts.len) {
        tlv_t ext, oid, val;
        span_t e;
        if (der_expect(&exts, 0x30, &ext) != 0) return;
        e = ext.val;
        if (der_expect(&e, 0x06, &oid) != 0) continue;
        if (e.len && e.p[0] == 0x01) {        /* critical BOOLEAN */
            tlv_t b;
            der_read(&e, &b);
        }
        if (der_expect(&e, 0x04, &val) != 0) continue;
        if (oid_is(&oid.val, OID_SAN, sizeof(OID_SAN))) {
            span_t v = val.val;
            tlv_t names;
            if (der_expect(&v, 0x30, &names) == 0) c->san = names.val;
        } else if (oid_is(&oid.val, OID_BASIC, sizeof(OID_BASIC))) {
            span_t v = val.val;
            tlv_t bc, flag;
            if (der_expect(&v, 0x30, &bc) == 0) {
                span_t b = bc.val;
                if (der_read(&b, &flag) == 0 && flag.tag == 0x01 && flag.val.len == 1 && flag.val.p[0]) c->is_ca = 1;
            }
        }
    }
}

int x509_parse(const uint8_t *der, size_t len, cert_t *c) {
    span_t all = { der, len }, tbs, cur;
    tlv_t cert, t, alg, sig, tmp;
    memset(c, 0, sizeof(*c));
    if (der_expect(&all, 0x30, &cert) != 0) return -1;
    c->raw = cert.full;
    cur = cert.val;
    if (der_expect(&cur, 0x30, &t) != 0 || der_expect(&cur, 0x30, &alg) != 0 || der_expect(&cur, 0x03, &sig) != 0) {
        return -1;
    }
    if (sig.val.len < 2) return -1;
    c->tbs = t.full;
    c->sig.p = sig.val.p + 1;
    c->sig.len = sig.val.len - 1;
    {
        span_t a = alg.val;
        tlv_t oid;
        if (der_expect(&a, 0x06, &oid) != 0) return -1;
        sig_alg(&oid.val, c);
    }
    tbs = t.val;
    if (tbs.len && tbs.p[0] == 0xA0) der_read(&tbs, &tmp);           /* version */
    if (der_expect(&tbs, 0x02, &tmp) != 0) return -1;                 /* serial */
    if (der_expect(&tbs, 0x30, &tmp) != 0) return -1;                 /* signature alg */
    if (der_expect(&tbs, 0x30, &tmp) != 0) return -1;
    c->issuer = tmp.full;
    if (der_expect(&tbs, 0x30, &tmp) != 0) return -1;                 /* validity */
    {
        span_t v = tmp.val;
        tlv_t nb, na;
        if (der_read(&v, &nb) != 0 || der_read(&v, &na) != 0) return -1;
        c->not_before = parse_time(&nb);
        c->not_after = parse_time(&na);
    }
    if (der_expect(&tbs, 0x30, &tmp) != 0) return -1;
    c->subject = tmp.full;
    if (der_expect(&tbs, 0x30, &tmp) != 0) return -1;
    if (parse_spki(tmp.val, c) != 0) c->key_type = KEY_NONE;
    while (tbs.len) {
        if (der_read(&tbs, &tmp) != 0) break;
        if (tmp.tag == 0xA3) parse_extensions(tmp.val, c);
    }
    return 0;
}

/* ---- hashing and signatures --------------------------------------------- */

void hash_data(int hash, const uint8_t *data, size_t len, uint8_t *out, size_t *out_len) {
    switch (hash) {
    case HASH_SHA1: sha1_hash(data, (uint32_t)len, out); *out_len = 20; break;
    case HASH_SHA256: sha256_hash(data, (uint32_t)len, out); *out_len = 32; break;
    case HASH_SHA384: sha384_hash(data, len, out); *out_len = 48; break;
    case HASH_SHA512: sha512_hash(data, len, out); *out_len = 64; break;
    default: *out_len = 0; break;
    }
}

static const uint8_t DI_SHA1[]   = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14 };
static const uint8_t DI_SHA256[] = { 0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const uint8_t DI_SHA384[] = { 0x30, 0x41, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const uint8_t DI_SHA512[] = { 0x30, 0x51, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

/* em = sig^e mod n, left-padded to the modulus length, using Montgomery
 * multiplication on 64-bit limbs (moduli up to 4096 bits). */
#define RSA_L 64
typedef unsigned __int128 u128_t;

static void rsa_mul(uint64_t *r, const uint64_t *a, const uint64_t *b, const uint64_t *m, uint64_t minv, int n) {
    uint64_t t[RSA_L + 2];
    memset(t, 0, sizeof(uint64_t) * (size_t)(n + 2));
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0, u;
        for (int j = 0; j < n; j++) {
            u128_t s = (u128_t)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        {
            u128_t s = (u128_t)t[n] + carry;
            t[n] = (uint64_t)s;
            t[n + 1] = (uint64_t)(s >> 64);
        }
        u = t[0] * minv;
        carry = 0;
        for (int j = 0; j < n; j++) {
            u128_t s = (u128_t)u * m[j] + t[j] + carry;
            t[j] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        {
            u128_t s = (u128_t)t[n] + carry;
            t[n] = (uint64_t)s;
            t[n + 1] += (uint64_t)(s >> 64);
        }
        for (int j = 0; j <= n; j++) t[j] = t[j + 1];
        t[n + 1] = 0;
    }
    {
        int ge = t[n] != 0;
        if (!ge) {
            ge = 1;
            for (int i = n - 1; i >= 0; i--) {
                if (t[i] != m[i]) {
                    ge = t[i] > m[i];
                    break;
                }
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i < n; i++) {
                u128_t d = (u128_t)t[i] - m[i] - borrow;
                t[i] = (uint64_t)d;
                borrow = (uint64_t)(d >> 64) & 1;
            }
        }
    }
    memcpy(r, t, sizeof(uint64_t) * (size_t)n);
}

static void limbs_be(uint64_t *out, const uint8_t *in, size_t len, int n) {
    memset(out, 0, sizeof(uint64_t) * (size_t)n);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        out[bit / 64] |= (uint64_t)in[i] << (bit % 64);
    }
}

static int rsa_public(const cert_t *k, const uint8_t *sig, size_t sig_len, uint8_t *em, size_t *em_len) {
    uint64_t m[RSA_L], s[RSA_L], acc[RSA_L], base[RSA_L], rr[RSA_L], one[RSA_L];
    size_t klen = k->rsa_n.len;
    int n = (int)((klen + 7) / 8);
    uint64_t minv = 1;
    if (klen > 512 || klen < 128 || sig_len != klen || k->rsa_e.len > 8 || !(k->rsa_n.p[klen - 1] & 1)) return -1;
    limbs_be(m, k->rsa_n.p, klen, n);
    limbs_be(s, sig, sig_len, n);
    for (int i = n - 1; i >= 0; i--) {
        if (s[i] != m[i]) {
            if (s[i] > m[i]) return -1;
            break;
        }
    }
    for (int i = 0; i < 6; i++) minv *= 2 - m[0] * minv;
    minv = (uint64_t)0 - minv;
    /* R^2 mod m by doubling 1 */
    memset(rr, 0, sizeof(rr));
    rr[0] = 1;
    for (int i = 0; i < n * 128; i++) {
        uint64_t carry = 0;
        int ge;
        for (int j = 0; j < n; j++) {
            uint64_t nv = (rr[j] << 1) | carry;
            carry = rr[j] >> 63;
            rr[j] = nv;
        }
        ge = carry != 0;
        if (!ge) {
            ge = 1;
            for (int j = n - 1; j >= 0; j--) {
                if (rr[j] != m[j]) {
                    ge = rr[j] > m[j];
                    break;
                }
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int j = 0; j < n; j++) {
                u128_t d = (u128_t)rr[j] - m[j] - borrow;
                rr[j] = (uint64_t)d;
                borrow = (uint64_t)(d >> 64) & 1;
            }
        }
    }
    memset(one, 0, sizeof(one));
    one[0] = 1;
    rsa_mul(base, s, rr, m, minv, n);          /* base = s R */
    rsa_mul(acc, one, rr, m, minv, n);         /* acc = R (1 in Montgomery form) */
    {
        uint64_t e = 0;
        int top = 63;
        for (size_t i = 0; i < k->rsa_e.len; i++) e = (e << 8) | k->rsa_e.p[i];
        if (!e) return -1;
        while (!((e >> top) & 1)) top--;
        for (int i = top; i >= 0; i--) {
            rsa_mul(acc, acc, acc, m, minv, n);
            if ((e >> i) & 1) rsa_mul(acc, acc, base, m, minv, n);
        }
    }
    rsa_mul(acc, acc, one, m, minv, n);        /* leave Montgomery form */
    for (size_t i = 0; i < klen; i++) {
        size_t bit = (klen - 1 - i) * 8;
        em[i] = (uint8_t)(acc[bit / 64] >> (bit % 64));
    }
    *em_len = klen;
    return 0;
}

static int rsa_pkcs1_verify(const cert_t *k, int hash, const uint8_t *digest, size_t dlen,
                            const uint8_t *sig, size_t sig_len) {
    uint8_t em[512];
    size_t em_len, di_len, pos;
    const uint8_t *di;
    if (rsa_public(k, sig, sig_len, em, &em_len) != 0) return -1;
    switch (hash) {
    case HASH_SHA1: di = DI_SHA1; di_len = sizeof(DI_SHA1); break;
    case HASH_SHA256: di = DI_SHA256; di_len = sizeof(DI_SHA256); break;
    case HASH_SHA384: di = DI_SHA384; di_len = sizeof(DI_SHA384); break;
    case HASH_SHA512: di = DI_SHA512; di_len = sizeof(DI_SHA512); break;
    default: return -1;
    }
    if (em_len < di_len + dlen + 11 || em[0] != 0 || em[1] != 1) return -1;
    pos = em_len - di_len - dlen - 1;
    for (size_t i = 2; i < pos; i++) {
        if (em[i] != 0xFF) return -1;
    }
    if (em[pos] != 0) return -1;
    if (memcmp(em + pos + 1, di, di_len) != 0 || memcmp(em + pos + 1 + di_len, digest, dlen) != 0) return -1;
    return 0;
}

static int rsa_pss_verify(const cert_t *k, int hash, const uint8_t *digest, size_t hlen,
                          const uint8_t *sig, size_t sig_len) {
    uint8_t em_full[512], db[512], h2[64], mprime[8 + 64 + 64];
    size_t em_len, mod_bits, em_bits, db_len, salt;
    uint8_t *em;
    if (rsa_public(k, sig, sig_len, em_full, &em_len) != 0) return -1;
    mod_bits = k->rsa_n.len * 8;
    for (uint8_t top = k->rsa_n.p[0]; !(top & 0x80); top <<= 1) mod_bits--;
    em_bits = mod_bits - 1;
    em = em_full;
    if ((em_bits + 7) / 8 < em_len) {
        if (em[0] != 0) return -1;
        em++;
        em_len--;
    }
    if (em_len < hlen * 2 + 2 || em[em_len - 1] != 0xBC) return -1;
    db_len = em_len - hlen - 1;
    {
        /* MGF1 */
        const uint8_t *h = em + db_len;
        uint32_t counter = 0;
        size_t done = 0;
        while (done < db_len) {
            uint8_t in[64 + 4], out[64];
            size_t ol;
            memcpy(in, h, hlen);
            in[hlen] = (uint8_t)(counter >> 24);
            in[hlen + 1] = (uint8_t)(counter >> 16);
            in[hlen + 2] = (uint8_t)(counter >> 8);
            in[hlen + 3] = (uint8_t)counter;
            hash_data(hash, in, hlen + 4, out, &ol);
            for (size_t i = 0; i < ol && done < db_len; i++, done++) db[done] = em[done] ^ out[i];
            counter++;
        }
    }
    db[0] &= (uint8_t)(0xFF >> (8 * em_len - em_bits));
    {
        size_t i = 0;
        while (i < db_len && db[i] == 0) i++;
        if (i >= db_len || db[i] != 0x01) return -1;
        salt = db_len - i - 1;
        if (salt > 64) return -1;
        memset(mprime, 0, 8);
        memcpy(mprime + 8, digest, hlen);
        memcpy(mprime + 8 + hlen, db + i + 1, salt);
    }
    {
        size_t ol;
        hash_data(hash, mprime, 8 + hlen + salt, h2, &ol);
        return memcmp(h2, em + db_len, hlen) == 0 ? 0 : -1;
    }
}

int x509_verify_sig(const cert_t *signer, int hash, int rsa_pss, const uint8_t *data, size_t len,
                    const uint8_t *sig, size_t sig_len) {
    uint8_t digest[64];
    size_t dlen;
    hash_data(hash, data, len, digest, &dlen);
    if (!dlen) return -1;
    if (signer->key_type == KEY_RSA) {
        return rsa_pss ? rsa_pss_verify(signer, hash, digest, dlen, sig, sig_len)
                       : rsa_pkcs1_verify(signer, hash, digest, dlen, sig, sig_len);
    }
    if (signer->key_type == KEY_EC_P256 || signer->key_type == KEY_EC_P384) {
        return ecdsa_verify(signer->key_type == KEY_EC_P256 ? EC_P256 : EC_P384, signer->ec_point.p,
                            signer->ec_point.len, digest, dlen, sig, sig_len);
    }
    return -1;
}

static int verify_issued(const cert_t *child, const cert_t *issuer) {
    if (child->issuer.len != issuer->subject.len || memcmp(child->issuer.p, issuer->subject.p, child->issuer.len) != 0) {
        return -1;
    }
    if (child->sig_hash == HASH_NONE || child->sig_hash == HASH_SHA1) return -1;
    if (child->sig_rsa != (issuer->key_type == KEY_RSA)) return -1;
    return x509_verify_sig(issuer, child->sig_hash, 0, child->tbs.p, child->tbs.len, child->sig.p, child->sig.len);
}

/* ---- root store --------------------------------------------------------- */

static cert_t *roots;
static int root_count;
static uint8_t *root_der;

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int x509_load_roots(const char *pem_path) {
    FILE *f;
    char *pem;
    long size;
    size_t out = 0;
    int cap = 0;
    if (roots) return root_count;
    f = fopen(pem_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    pem = (char *)malloc((size_t)size + 1);
    root_der = (uint8_t *)malloc((size_t)size);
    if (!pem || !root_der) {
        fclose(f);
        return -1;
    }
    size = (long)fread(pem, 1, (size_t)size, f);
    fclose(f);
    pem[size] = 0;
    for (char *p = pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")) != 0;) {
        char *end = strstr(p, "-----END CERTIFICATE-----");
        size_t start = out;
        uint32_t acc = 0;
        int bits = 0;
        if (!end) break;
        p += 27;
        for (; p < end; p++) {
            int v = b64v((unsigned char)*p);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                root_der[out++] = (uint8_t)(acc >> bits);
            }
        }
        if (root_count == cap) {
            cap = cap ? cap * 2 : 64;
            roots = (cert_t *)realloc(roots, sizeof(cert_t) * (size_t)cap);
        }
        if (x509_parse(root_der + start, out - start, &roots[root_count]) == 0 &&
            roots[root_count].key_type != KEY_NONE) {
            root_count++;
        }
    }
    free(pem);
    return root_count;
}

/* ---- hostname ----------------------------------------------------------- */

static int name_matches(const uint8_t *pat, size_t plen, const char *host) {
    size_t hlen = strlen(host);
    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || dot == host) return 0;
        host = dot;
        hlen = strlen(host);
        pat++;
        plen--;
    }
    if (plen != hlen) return 0;
    for (size_t i = 0; i < plen; i++) {
        int a = pat[i], b = (unsigned char)host[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int host_ok(const cert_t *leaf, const char *host) {
    span_t s = leaf->san;
    while (s.len) {
        tlv_t name;
        if (der_read(&s, &name) != 0) return 0;
        if (name.tag == 0x82 && name_matches(name.val.p, name.val.len, host)) return 1;
    }
    return 0;
}

/* ---- chain -------------------------------------------------------------- */

int x509_verify_chain(const cert_t *certs, int n, const char *host, int64_t now, const char **why) {
    const cert_t *cur = &certs[0];
    int used[16] = { 0 };
    *why = "ok";
    if (n < 1) {
        *why = "no certificate";
        return -1;
    }
    if (root_count <= 0 && x509_load_roots("/etc/ssl/certs.pem") <= 0) {
        *why = "no trusted root certificates installed";
        return -1;
    }
    if (!host_ok(cur, host)) {
        *why = "certificate is for a different site";
        return -1;
    }
    for (int depth = 0; depth < 8; depth++) {
        int next = -1;
        if (now && (now < cur->not_before - 86400 || now > cur->not_after + 86400)) {
            *why = "certificate has expired or is not yet valid";
            return -1;
        }
        for (int r = 0; r < root_count; r++) {
            if (cur->raw.len == roots[r].raw.len && memcmp(cur->raw.p, roots[r].raw.p, cur->raw.len) == 0) return 0;
            if (verify_issued(cur, &roots[r]) == 0) return 0;
        }
        for (int i = 1; i < n && i < 16; i++) {
            if (used[i] || !certs[i].is_ca) continue;
            if (verify_issued(cur, &certs[i]) == 0) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            *why = "certificate is not signed by a trusted authority";
            return -1;
        }
        used[next] = 1;
        cur = &certs[next];
    }
    *why = "certificate chain is too long";
    return -1;
}
