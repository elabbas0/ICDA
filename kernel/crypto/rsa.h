#ifndef RSA_H
#define RSA_H

#include <stdint.h>
#include "bn.h"

typedef struct {
    bn_t n;
    bn_t e;
} rsa_pubkey_t;

int rsa_pubkey_from_cert_der(const uint8_t *der, int der_len, rsa_pubkey_t *key);
int rsa_pubkey_from_der(const uint8_t *der, int der_len, rsa_pubkey_t *key);
int rsa_encrypt(const rsa_pubkey_t *key, const uint8_t *in, int in_len, uint8_t *out, int *out_len);
int rsa_pkcs1_v15_encode(const uint8_t *in, int in_len, uint8_t *out, int out_len);

#endif
