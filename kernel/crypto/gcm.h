#ifndef GCM_H
#define GCM_H

#include <stdint.h>
#include "aes.h"

/* AES-128-GCM. rk is the expanded key from aes128_expand_key.
 * Encrypt: ct_out and tag written. Decrypt: returns 0 if tag matches. */
void aes128_gcm_encrypt(const uint8_t rk[176], const uint8_t nonce[12],
                        const uint8_t *aad, uint32_t aad_len,
                        const uint8_t *pt, uint32_t pt_len,
                        uint8_t *ct_out, uint8_t tag[16]);
int aes128_gcm_decrypt(const uint8_t rk[176], const uint8_t nonce[12],
                       const uint8_t *aad, uint32_t aad_len,
                       const uint8_t *ct, uint32_t ct_len,
                       const uint8_t tag[16], uint8_t *pt_out);

#endif
