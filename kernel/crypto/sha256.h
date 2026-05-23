#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);
void sha256_hash(const uint8_t *data, uint32_t len, uint8_t hash[SHA256_DIGEST_SIZE]);

#endif
