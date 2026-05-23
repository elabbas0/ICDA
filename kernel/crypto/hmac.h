#ifndef HMAC_H
#define HMAC_H

#include <stdint.h>
#include "sha256.h"

static inline void hmac_sha256(const uint8_t *key, uint32_t key_len,
                                const uint8_t *data, uint32_t data_len,
                                uint8_t out[SHA256_DIGEST_SIZE]) {
    sha256_ctx_t ctx;
    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    uint8_t k_opad[SHA256_BLOCK_SIZE];
    uint8_t tk[SHA256_DIGEST_SIZE];
    uint8_t k[SHA256_BLOCK_SIZE];
    uint32_t i;

    if (key_len > SHA256_BLOCK_SIZE) {
        sha256_hash(key, key_len, tk);
        for (i = 0; i < SHA256_DIGEST_SIZE; i++) k[i] = tk[i];
        for (; i < SHA256_BLOCK_SIZE; i++) k[i] = 0;
    } else {
        for (i = 0; i < key_len; i++) k[i] = key[i];
        for (; i < SHA256_BLOCK_SIZE; i++) k[i] = 0;
    }

    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, out);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, out, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, out);
}

#endif
