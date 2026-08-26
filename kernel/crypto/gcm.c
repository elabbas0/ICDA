#include "gcm.h"

/* GF(2^128) multiplication per NIST SP 800-38D (bit-serial, MSB first). */
static void gcm_gf_mul(uint8_t x[16], const uint8_t y[16]) {
    uint8_t z[16];
    uint8_t v[16];
    for (int i = 0; i < 16; i++) { z[i] = 0; v[i] = y[i]; }

    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1) {
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        }
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    for (int i = 0; i < 16; i++) x[i] = z[i];
}

static void gcm_ghash(const uint8_t h[16], const uint8_t *aad, uint32_t aad_len,
                      const uint8_t *ct, uint32_t ct_len, uint8_t out[16]) {
    uint8_t x[16];
    uint8_t block[16];
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
            gcm_gf_mul(x, h);
            off += n;
        }
    }

    for (int i = 0; i < 8; i++) block[i] = (uint8_t)((uint64_t)aad_len * 8 >> (56 - i * 8));
    for (int i = 0; i < 8; i++) block[8 + i] = (uint8_t)((uint64_t)ct_len * 8 >> (56 - i * 8));
    for (int i = 0; i < 16; i++) x[i] ^= block[i];
    gcm_gf_mul(x, h);

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
