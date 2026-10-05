#ifndef SURFER_ECDSA_H
#define SURFER_ECDSA_H

#include <stdint.h>
#include <stddef.h>

#define EC_P256 1
#define EC_P384 2

/* Verifies an ECDSA signature.  pub is the uncompressed SEC1 point
 * (0x04 || X || Y); sig is the DER SEQUENCE { r INTEGER, s INTEGER };
 * hash is the message digest (truncated to the curve size if longer).
 * Returns 0 when the signature is valid. */
int ecdsa_verify(int curve, const uint8_t *pub, size_t pub_len, const uint8_t *hash, size_t hash_len,
                 const uint8_t *sig, size_t sig_len);

#endif
