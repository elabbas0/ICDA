#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

#define SHA1_BLOCK_SIZE 64
#define SHA1_DIGEST_SIZE 20

typedef struct {
    uint32_t state[5];
    uint64_t bitlen;
    uint8_t data[SHA1_BLOCK_SIZE];
    uint32_t datalen;
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t hash[SHA1_DIGEST_SIZE]);
void sha1_hash(const uint8_t *data, uint32_t len, uint8_t hash[SHA1_DIGEST_SIZE]);

#endif
