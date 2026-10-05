/*
 * ICDA port: WPA2-PSK cryptographic primitives (802.11i / 802.11-2016).
 * Pure functions with no kernel dependencies, so the host unit test
 * (tests/wpa_test.c) can check them against the standard test vectors.
 *
 * The CCMP code is derived from OpenBSD's ieee80211_crypto_ccmp.c:
 *
 * Copyright (c) 2008 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2026 ICDA contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ICDA_WPA_CRYPTO_H
#define ICDA_WPA_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define WPA_PMK_LEN	32
#define WPA_NONCE_LEN	32
#define WPA_MIC_LEN	16
#define WPA_CCMP_HDRLEN	8
#define WPA_CCMP_MICLEN	8

/* HMAC-SHA1 over a list of buffers. */
void	wpa_hmac_sha1_vec(const uint8_t *key, size_t keylen, size_t n,
	    const uint8_t *const *addr, const size_t *len, uint8_t mac[20]);

/* PBKDF2-HMAC-SHA1 (RFC 2898). */
void	wpa_pbkdf2_sha1(const uint8_t *pass, size_t passlen,
	    const uint8_t *salt, size_t saltlen, unsigned int iterations,
	    uint8_t *out, size_t outlen);

/*
 * PSK from a passphrase (8..63 printable chars; 4096 iterations, salt =
 * SSID) or from 64 hex digits.  Returns 0, or -1 if the input is invalid.
 */
int	wpa_passphrase_to_pmk(const char *pass, const uint8_t *ssid,
	    size_t ssidlen, uint8_t pmk[WPA_PMK_LEN]);

/*
 * PRF-n of 802.11i 8.5.1.1: HMAC-SHA1(K, A || 0 || B || i) for i = 0, 1, ..
 * `label' is A without the terminating zero octet.
 */
void	wpa_prf_sha1(const uint8_t *key, size_t keylen, const char *label,
	    const uint8_t *data, size_t datalen, uint8_t *out, size_t outlen);

/* PTK = PRF-X(PMK, "Pairwise key expansion", Min/Max(AA,SPA) || nonces) */
void	wpa_derive_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6],
	    const uint8_t spa[6], const uint8_t anonce[WPA_NONCE_LEN],
	    const uint8_t snonce[WPA_NONCE_LEN], uint8_t *ptk, size_t ptklen);

/* EAPOL-Key MIC, descriptor version 2 (HMAC-SHA1-128). */
void	wpa_eapol_mic_v2(const uint8_t kck[16], const uint8_t *frame,
	    size_t len, uint8_t mic[WPA_MIC_LEN]);

/* AES-128 Key Wrap (RFC 3394); n = number of 64-bit plaintext blocks. */
void	wpa_aes_wrap(const uint8_t kek[16], const uint8_t *plain, size_t n,
	    uint8_t *cipher /* (n + 1) * 8 bytes */);
/* Returns 0 on success, -1 if the integrity check fails. */
int	wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *cipher, size_t n,
	    uint8_t *plain /* n * 8 bytes */);

/*
 * CCMP (CCM with M=8, L=2) over an 802.11 MPDU.  `hdr' is the 802.11
 * header (hdrlen bytes, Protected bit as it will appear on the air),
 * `pn' the 48-bit packet number.
 *   encrypt: plain[len] -> out[len + 8] (ciphertext || MIC)
 *   decrypt: in[len] (ciphertext || MIC) -> out[len - 8]; 0 if MIC ok
 */
void	wpa_ccmp_encrypt(const uint8_t tk[16], const uint8_t *hdr,
	    size_t hdrlen, uint64_t pn, const uint8_t *plain, size_t len,
	    uint8_t *out);
int	wpa_ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr,
	    size_t hdrlen, uint64_t pn, const uint8_t *in, size_t len,
	    uint8_t *out);

#endif /* ICDA_WPA_CRYPTO_H */
