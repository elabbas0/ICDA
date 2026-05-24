#ifndef BN_H
#define BN_H

#include <stdint.h>

#define BN_MAX_WORDS 256
#define BN_WORD_BITS 32

typedef struct {
    uint32_t d[BN_MAX_WORDS];
    int len;
} bn_t;

void bn_zero(bn_t *a);
int bn_from_bytes(bn_t *a, const uint8_t *bytes, int len);
void bn_from_uint32(bn_t *a, uint32_t v);
int bn_is_zero(const bn_t *a);
int bn_cmp(const bn_t *a, const bn_t *b);
void bn_mul(bn_t *r, const bn_t *a, const bn_t *b);
void bn_mod(bn_t *r, const bn_t *a, const bn_t *m);
void bn_mod_exp(bn_t *r, const bn_t *a, const bn_t *e, const bn_t *m);
void bn_add(bn_t *r, const bn_t *a, const bn_t *b);
void bn_sub(bn_t *r, const bn_t *a, const bn_t *b);
void bn_sub_mod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m);
void bn_to_bytes(const bn_t *a, uint8_t *bytes, int *len);

#endif
