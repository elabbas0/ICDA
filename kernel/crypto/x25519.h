#ifndef X25519_H
#define X25519_H

#include <stdint.h>

void x25519_public(uint8_t out[32], const uint8_t priv[32]);
/* Returns -1 if the peer public key yields an all-zero shared secret. */
int x25519_shared(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]);

#endif
