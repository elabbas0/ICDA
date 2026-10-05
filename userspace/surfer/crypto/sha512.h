#ifndef SURFER_SHA512_H
#define SURFER_SHA512_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t h[8];
    uint8_t  buf[128];
    uint64_t total;
    size_t   used;
    int      out_len;   /* 64 for SHA-512, 48 for SHA-384 */
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *c);
void sha384_init(sha512_ctx_t *c);
void sha512_update(sha512_ctx_t *c, const uint8_t *data, size_t len);
void sha512_final(sha512_ctx_t *c, uint8_t *out);
void sha384_hash(const uint8_t *data, size_t len, uint8_t out[48]);
void sha512_hash(const uint8_t *data, size_t len, uint8_t out[64]);

#endif
