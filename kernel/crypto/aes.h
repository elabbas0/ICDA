#ifndef AES_H
#define AES_H

#include <stdint.h>

#define AES_BLOCK_SIZE 16
#define AES128_KEY_SIZE 16

void aes128_expand_key(const uint8_t key[AES128_KEY_SIZE], uint8_t rk[176]);
void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);
void aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);
void aes128_cbc_encrypt(const uint8_t rk[176], const uint8_t iv[AES_BLOCK_SIZE], const uint8_t *in, uint32_t len, uint8_t *out);
void aes128_cbc_decrypt(const uint8_t rk[176], const uint8_t iv[AES_BLOCK_SIZE], const uint8_t *in, uint32_t len, uint8_t *out);

#endif
