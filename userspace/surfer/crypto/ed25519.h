#ifndef SURFER_ED25519_H
#define SURFER_ED25519_H

#include <stdint.h>
#include <stddef.h>

/* Ed25519 signature check (RFC 8032), after the public-domain TweetNaCl.
 * Returns 1 when sig is a valid signature of msg under pub. */
int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pub[32]);

#endif
