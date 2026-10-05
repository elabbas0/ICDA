#include "gcm.h"


/* GHASH multiplication with 4-bit tables (Shoup's method): 16 table
 * entries per key replace 128 shift-and-xor rounds per block. */
typedef struct {
    uint64_t hl[16], hh[16];
} gcm_table_t;

static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void gcm_init_table(gcm_table_t *t, const uint8_t h[16]) {
    uint64_t vh = be64(h), vl = be64(h + 8);
    t->hl[0] = t->hh[0] = 0;
    t->hl[8] = vl;
    t->hh[8] = vh;
    for (int i = 4; i > 0; i >>= 1) {
        uint64_t r = (vl & 1) ? 0xE1000000ULL << 32 : 0;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ r;
        t->hl[i] = vl;
        t->hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2) {
        for (int j = 1; j < i; j++) {
            t->hh[i + j] = t->hh[i] ^ t->hh[j];
            t->hl[i + j] = t->hl[i] ^ t->hl[j];
        }
    }
}

static const uint64_t gcm_last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

static void gcm_gf_mul(uint8_t x[16], const gcm_table_t *t) {
    uint8_t lo = x[15] & 0x0F, hi, rem;
    uint64_t zh = t->hh[lo], zl = t->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0x0F;
        hi = (x[i] >> 4) & 0x0F;
        if (i != 15) {
            rem = (uint8_t)(zl & 0x0F);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ (gcm_last4[rem] << 48);
            zh ^= t->hh[lo];
            zl ^= t->hl[lo];
        }
        rem = (uint8_t)(zl & 0x0F);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4) ^ (gcm_last4[rem] << 48);
        zh ^= t->hh[hi];
        zl ^= t->hl[hi];
    }
    for (int i = 0; i < 8; i++) {
        x[i] = (uint8_t)(zh >> (56 - i * 8));
        x[8 + i] = (uint8_t)(zl >> (56 - i * 8));
    }
}

static void gcm_ghash(const uint8_t h[16], const uint8_t *aad, uint32_t aad_len,
                      const uint8_t *ct, uint32_t ct_len, uint8_t out[16]) {
    uint8_t x[16];
    uint8_t block[16];
    gcm_table_t t;
    gcm_init_table(&t, h);
    for (int i = 0; i < 16; i++) x[i] = 0;

    const uint8_t *streams[2] = { aad, ct };
    uint32_t lens[2] = { aad_len, ct_len };
    for (int s = 0; s < 2; s++) {
        const uint8_t *d = streams[s];
        uint32_t len = lens[s];
        uint32_t off = 0;
        while (off < len) {
            uint32_t n = len - off;
            if (n > 16) n = 16;
            for (int i = 0; i < 16; i++) block[i] = 0;
            for (uint32_t i = 0; i < n; i++) block[i] = d[off + i];
            for (int i = 0; i < 16; i++) x[i] ^= block[i];
            gcm_gf_mul(x, &t);
            off += n;
        }
    }

    for (int i = 0; i < 8; i++) block[i] = (uint8_t)((uint64_t)aad_len * 8 >> (56 - i * 8));
    for (int i = 0; i < 8; i++) block[8 + i] = (uint8_t)((uint64_t)ct_len * 8 >> (56 - i * 8));
    for (int i = 0; i < 16; i++) x[i] ^= block[i];
    gcm_gf_mul(x, &t);

    for (int i = 0; i < 16; i++) out[i] = x[i];
}

static void gcm_ctr(const uint8_t rk[176], uint8_t ctr[16],
                    const uint8_t *in, uint32_t len, uint8_t *out) {
    uint8_t ks[16];
    uint32_t off = 0;
    while (off < len) {
        aes128_encrypt_block(rk, ctr, ks);
        uint32_t n = len - off;
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
        off += n;
        for (int i = 15; i >= 12; i--) {
            if (++ctr[i]) break;
        }
    }
}

static void gcm_j0(const uint8_t nonce[12], uint8_t j0[16]) {
    for (int i = 0; i < 12; i++) j0[i] = nonce[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

void aes128_gcm_encrypt(const uint8_t rk[176], const uint8_t nonce[12],
                        const uint8_t *aad, uint32_t aad_len,
                        const uint8_t *pt, uint32_t pt_len,
                        uint8_t *ct_out, uint8_t tag[16]) {
    uint8_t h[16];
    uint8_t j0[16];
    uint8_t s[16];
    uint8_t ej0[16];
    uint8_t zero[16];
    for (int i = 0; i < 16; i++) zero[i] = 0;

    aes128_encrypt_block(rk, zero, h);
    gcm_j0(nonce, j0);

    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = j0[i];
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i]) break;
    }
    gcm_ctr(rk, ctr, pt, pt_len, ct_out);

    gcm_ghash(h, aad, aad_len, ct_out, pt_len, s);
    aes128_encrypt_block(rk, j0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ej0[i];
}

int aes128_gcm_decrypt(const uint8_t rk[176], const uint8_t nonce[12],
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *ct, uint32_t ct_len,
                       const uint8_t tag[16], uint8_t *pt_out) {
    uint8_t h[16];
    uint8_t j0[16];
    uint8_t s[16];
    uint8_t ej0[16];
    uint8_t zero[16];
    for (int i = 0; i < 16; i++) zero[i] = 0;

    aes128_encrypt_block(rk, zero, h);
    gcm_j0(nonce, j0);

    gcm_ghash(h, aad, aad_len, ct, ct_len, s);
    aes128_encrypt_block(rk, j0, ej0);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(s[i] ^ ej0[i] ^ tag[i]);
    if (diff != 0) return -1;

    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = j0[i];
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i]) break;
    }
    gcm_ctr(rk, ctr, ct, ct_len, pt_out);
    return 0;
}
