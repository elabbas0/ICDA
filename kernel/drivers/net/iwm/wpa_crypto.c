/*
 * ICDA port: WPA2-PSK cryptographic primitives.  See wpa_crypto.h.
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

#include "wpa_crypto.h"

#include "crypto/sha1.h"
#include "crypto/aes.h"

static void
wcopy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
}

static void
wzero(void *dst, size_t n)
{
	volatile uint8_t *d = dst;

	while (n--)
		*d++ = 0;
}

static int
wcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

/* ---- HMAC-SHA1 -------------------------------------------------------- */

struct hmac_sha1 {
	sha1_ctx_t	inner;
	sha1_ctx_t	outer;
};

static void
hmac_sha1_init(struct hmac_sha1 *h, const uint8_t *key, size_t keylen)
{
	uint8_t k[SHA1_BLOCK_SIZE], pad[SHA1_BLOCK_SIZE];
	size_t i;

	wzero(k, sizeof(k));
	if (keylen > SHA1_BLOCK_SIZE)
		sha1_hash(key, (uint32_t)keylen, k);
	else
		wcopy(k, key, keylen);

	for (i = 0; i < SHA1_BLOCK_SIZE; i++)
		pad[i] = k[i] ^ 0x36;
	sha1_init(&h->inner);
	sha1_update(&h->inner, pad, SHA1_BLOCK_SIZE);
	for (i = 0; i < SHA1_BLOCK_SIZE; i++)
		pad[i] = k[i] ^ 0x5c;
	sha1_init(&h->outer);
	sha1_update(&h->outer, pad, SHA1_BLOCK_SIZE);
	wzero(k, sizeof(k));
	wzero(pad, sizeof(pad));
}

/* Finish a copy of the precomputed state, so `h' can be reused. */
static void
hmac_sha1_final(const struct hmac_sha1 *h, sha1_ctx_t *inner,
    uint8_t mac[SHA1_DIGEST_SIZE])
{
	sha1_ctx_t outer = h->outer;
	uint8_t ih[SHA1_DIGEST_SIZE];

	sha1_final(inner, ih);
	sha1_update(&outer, ih, SHA1_DIGEST_SIZE);
	sha1_final(&outer, mac);
}

void
wpa_hmac_sha1_vec(const uint8_t *key, size_t keylen, size_t n,
    const uint8_t *const *addr, const size_t *len, uint8_t mac[20])
{
	struct hmac_sha1 h;
	sha1_ctx_t inner;
	size_t i;

	hmac_sha1_init(&h, key, keylen);
	inner = h.inner;
	for (i = 0; i < n; i++)
		sha1_update(&inner, addr[i], (uint32_t)len[i]);
	hmac_sha1_final(&h, &inner, mac);
	wzero(&h, sizeof(h));
}

/* ---- PBKDF2 / PSK ----------------------------------------------------- */

void
wpa_pbkdf2_sha1(const uint8_t *pass, size_t passlen, const uint8_t *salt,
    size_t saltlen, unsigned int iterations, uint8_t *out, size_t outlen)
{
	struct hmac_sha1 h;
	uint8_t u[SHA1_DIGEST_SIZE], t[SHA1_DIGEST_SIZE], cnt[4];
	uint32_t block = 1;

	hmac_sha1_init(&h, pass, passlen);
	while (outlen > 0) {
		sha1_ctx_t inner = h.inner;
		size_t i, c;
		unsigned int it;

		cnt[0] = (uint8_t)(block >> 24);
		cnt[1] = (uint8_t)(block >> 16);
		cnt[2] = (uint8_t)(block >> 8);
		cnt[3] = (uint8_t)block;
		sha1_update(&inner, salt, (uint32_t)saltlen);
		sha1_update(&inner, cnt, 4);
		hmac_sha1_final(&h, &inner, u);
		wcopy(t, u, sizeof(t));
		for (it = 1; it < iterations; it++) {
			inner = h.inner;
			sha1_update(&inner, u, SHA1_DIGEST_SIZE);
			hmac_sha1_final(&h, &inner, u);
			for (i = 0; i < SHA1_DIGEST_SIZE; i++)
				t[i] ^= u[i];
		}
		c = outlen < SHA1_DIGEST_SIZE ? outlen : SHA1_DIGEST_SIZE;
		wcopy(out, t, c);
		out += c;
		outlen -= c;
		block++;
	}
	wzero(&h, sizeof(h));
	wzero(u, sizeof(u));
	wzero(t, sizeof(t));
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int
wpa_passphrase_to_pmk(const char *pass, const uint8_t *ssid, size_t ssidlen,
    uint8_t pmk[WPA_PMK_LEN])
{
	size_t len = 0, i;

	while (pass[len])
		len++;
	if (len == 64) {
		for (i = 0; i < 32; i++) {
			int hi = hexval(pass[2 * i]), lo = hexval(pass[2 * i + 1]);
			if (hi < 0 || lo < 0)
				break;
			pmk[i] = (uint8_t)(hi << 4 | lo);
		}
		if (i == 32)
			return 0;
	}
	if (len < 8 || len > 63 || ssidlen == 0 || ssidlen > 32)
		return -1;
	for (i = 0; i < len; i++)
		if ((uint8_t)pass[i] < 32 || (uint8_t)pass[i] > 126)
			return -1;
	wpa_pbkdf2_sha1((const uint8_t *)pass, len, ssid, ssidlen, 4096,
	    pmk, WPA_PMK_LEN);
	return 0;
}

/* ---- PRF and key derivation ------------------------------------------- */

void
wpa_prf_sha1(const uint8_t *key, size_t keylen, const char *label,
    const uint8_t *data, size_t datalen, uint8_t *out, size_t outlen)
{
	struct hmac_sha1 h;
	uint8_t digest[SHA1_DIGEST_SIZE], zero = 0, count;
	size_t labellen = 0;

	while (label[labellen])
		labellen++;
	hmac_sha1_init(&h, key, keylen);
	for (count = 0; outlen > 0; count++) {
		sha1_ctx_t inner = h.inner;
		size_t c;

		sha1_update(&inner, (const uint8_t *)label, (uint32_t)labellen);
		sha1_update(&inner, &zero, 1);
		sha1_update(&inner, data, (uint32_t)datalen);
		sha1_update(&inner, &count, 1);
		hmac_sha1_final(&h, &inner, digest);
		c = outlen < SHA1_DIGEST_SIZE ? outlen : SHA1_DIGEST_SIZE;
		wcopy(out, digest, c);
		out += c;
		outlen -= c;
	}
	wzero(&h, sizeof(h));
	wzero(digest, sizeof(digest));
}

void
wpa_derive_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6],
    const uint8_t spa[6], const uint8_t anonce[WPA_NONCE_LEN],
    const uint8_t snonce[WPA_NONCE_LEN], uint8_t *ptk, size_t ptklen)
{
	uint8_t buf[2 * 6 + 2 * WPA_NONCE_LEN];
	int lt;

	lt = wcmp(aa, spa, 6) < 0;
	wcopy(&buf[0], lt ? aa : spa, 6);
	wcopy(&buf[6], lt ? spa : aa, 6);
	lt = wcmp(anonce, snonce, WPA_NONCE_LEN) < 0;
	wcopy(&buf[12], lt ? anonce : snonce, WPA_NONCE_LEN);
	wcopy(&buf[44], lt ? snonce : anonce, WPA_NONCE_LEN);

	wpa_prf_sha1(pmk, WPA_PMK_LEN, "Pairwise key expansion", buf,
	    sizeof(buf), ptk, ptklen);
	wzero(buf, sizeof(buf));
}

void
wpa_eapol_mic_v2(const uint8_t kck[16], const uint8_t *frame, size_t len,
    uint8_t mic[WPA_MIC_LEN])
{
	uint8_t digest[SHA1_DIGEST_SIZE];
	const uint8_t *addr[1] = { frame };
	size_t l[1] = { len };

	wpa_hmac_sha1_vec(kck, 16, 1, addr, l, digest);
	wcopy(mic, digest, WPA_MIC_LEN);
	wzero(digest, sizeof(digest));
}

/* ---- AES Key Wrap (RFC 3394) ------------------------------------------ */

void
wpa_aes_wrap(const uint8_t kek[16], const uint8_t *plain, size_t n,
    uint8_t *cipher)
{
	uint8_t rk[176], b[16], *a = cipher, *r;
	size_t i;
	int j;

	aes128_expand_key(kek, rk);
	for (i = 0; i < 8; i++)
		a[i] = 0xa6;
	wcopy(cipher + 8, plain, n * 8);
	for (j = 0; j <= 5; j++) {
		r = cipher + 8;
		for (i = 1; i <= n; i++, r += 8) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;
			int k;

			wcopy(b, a, 8);
			wcopy(b + 8, r, 8);
			aes128_encrypt_block(rk, b, b);
			wcopy(a, b, 8);
			for (k = 7; k >= 0; k--, t >>= 8)
				a[k] ^= (uint8_t)t;
			wcopy(r, b + 8, 8);
		}
	}
	wzero(rk, sizeof(rk));
	wzero(b, sizeof(b));
}

int
wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *cipher, size_t n,
    uint8_t *plain)
{
	uint8_t rk[176], b[16], a[8], *r;
	size_t i;
	int j, ok = 1;

	aes128_expand_key(kek, rk);
	wcopy(a, cipher, 8);
	/* `plain' may alias `cipher': memmove semantics, front to back */
	for (i = 0; i < n * 8; i++)
		plain[i] = cipher[8 + i];
	for (j = 5; j >= 0; j--) {
		r = plain + (n - 1) * 8;
		for (i = n; i >= 1; i--, r -= 8) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;
			int k;

			wcopy(b, a, 8);
			for (k = 7; k >= 0; k--, t >>= 8)
				b[k] ^= (uint8_t)t;
			wcopy(b + 8, r, 8);
			aes128_decrypt_block(rk, b, b);
			wcopy(a, b, 8);
			wcopy(r, b + 8, 8);
		}
	}
	for (i = 0; i < 8; i++)
		if (a[i] != 0xa6)
			ok = 0;
	wzero(rk, sizeof(rk));
	wzero(b, sizeof(b));
	return ok ? 0 : -1;
}

/* ---- CCMP ------------------------------------------------------------- */

#define FC0_TYPE_MASK	0x0c
#define FC0_TYPE_MGT	0x00
#define FC0_TYPE_DATA	0x08
#define FC0_SUBTYPE_MASK 0xf0
#define FC0_SUBTYPE_QOS	0x80
#define FC1_DIR_MASK	0x03
#define FC1_DIR_DSTODS	0x03
#define FC1_RETRY	0x08
#define FC1_PWR_MGT	0x10
#define FC1_MORE_DATA	0x20
#define FC1_ORDER	0x80

static int
hdr_has_qos(const uint8_t *hdr)
{
	return (hdr[0] & (FC0_TYPE_MASK | FC0_SUBTYPE_QOS)) ==
	    (FC0_TYPE_DATA | FC0_SUBTYPE_QOS);
}

/*
 * CCM phase 1 (RFC 3610, M = 8, L = 2): compute the CBC-MAC state over
 * B_0 and the AAD, the counter block A (counter still 0) and S_0.
 */
static void
ccmp_phase1(const uint8_t rk[176], const uint8_t *hdr, size_t hdrlen,
    uint64_t pn, size_t lm, uint8_t b[16], uint8_t a[16], uint8_t s0[16])
{
	uint8_t auth[32], nonce[13], *aad;
	uint8_t tid = 0;
	size_t la;
	int i, addr4 = (hdr[1] & FC1_DIR_MASK) == FC1_DIR_DSTODS;

	(void)hdrlen;
	aad = &auth[2];
	*aad = hdr[0];
	if ((hdr[0] & FC0_TYPE_MASK) == FC0_TYPE_DATA)
		*aad &= (uint8_t)(~FC0_SUBTYPE_MASK | FC0_SUBTYPE_QOS);
	aad++;
	*aad = hdr[1] & (uint8_t)~(FC1_RETRY | FC1_PWR_MGT | FC1_MORE_DATA);
	if (hdr_has_qos(hdr))
		*aad &= (uint8_t)~FC1_ORDER;
	aad++;
	wcopy(aad, hdr + 4, 18);	/* addr1, addr2, addr3 */
	aad += 18;
	*aad++ = hdr[22] & 0x0f;	/* sequence control: fragment number */
	*aad++ = 0;
	if (addr4) {
		wcopy(aad, hdr + 24, 6);
		aad += 6;
	}
	if (hdr_has_qos(hdr)) {
		tid = hdr[addr4 ? 30 : 24] & 0x0f;
		*aad++ = tid;
		*aad++ = 0;
	}

	nonce[0] = tid;
	if ((hdr[0] & FC0_TYPE_MASK) == FC0_TYPE_MGT)
		nonce[0] |= 1 << 4;
	wcopy(&nonce[1], hdr + 10, 6);	/* addr2 */
	nonce[7] = (uint8_t)(pn >> 40);
	nonce[8] = (uint8_t)(pn >> 32);
	nonce[9] = (uint8_t)(pn >> 24);
	nonce[10] = (uint8_t)(pn >> 16);
	nonce[11] = (uint8_t)(pn >> 8);
	nonce[12] = (uint8_t)pn;

	la = (size_t)(aad - &auth[2]);
	auth[0] = (uint8_t)(la >> 8);
	auth[1] = (uint8_t)la;
	wzero(aad, 30 - la);

	b[0] = 89;	/* Adata | M' = 3 | L' = 1 */
	wcopy(&b[1], nonce, 13);
	b[14] = (uint8_t)(lm >> 8);
	b[15] = (uint8_t)lm;
	aes128_encrypt_block(rk, b, b);
	for (i = 0; i < 16; i++)
		b[i] ^= auth[i];
	aes128_encrypt_block(rk, b, b);
	for (i = 0; i < 16; i++)
		b[i] ^= auth[16 + i];
	aes128_encrypt_block(rk, b, b);

	a[0] = 1;	/* L' = 1 */
	wcopy(&a[1], nonce, 13);
	a[14] = a[15] = 0;
	aes128_encrypt_block(rk, a, s0);
}

static void
ccmp_ctr(const uint8_t rk[176], uint8_t a[16], uint16_t ctr, uint8_t s[16])
{
	a[14] = (uint8_t)(ctr >> 8);
	a[15] = (uint8_t)ctr;
	aes128_encrypt_block(rk, a, s);
}

void
wpa_ccmp_encrypt(const uint8_t tk[16], const uint8_t *hdr, size_t hdrlen,
    uint64_t pn, const uint8_t *plain, size_t len, uint8_t *out)
{
	uint8_t rk[176], a[16], b[16], s0[16], s[16];
	size_t i;
	int j = 0;
	uint16_t ctr = 1;

	aes128_expand_key(tk, rk);
	ccmp_phase1(rk, hdr, hdrlen, pn, len, b, a, s0);
	ccmp_ctr(rk, a, ctr, s);
	for (i = 0; i < len; i++) {
		b[j] ^= plain[i];
		out[i] = plain[i] ^ s[j];
		if (++j < 16)
			continue;
		aes128_encrypt_block(rk, b, b);
		ccmp_ctr(rk, a, ++ctr, s);
		j = 0;
	}
	if (j != 0)
		aes128_encrypt_block(rk, b, b);
	for (i = 0; i < WPA_CCMP_MICLEN; i++)
		out[len + i] = b[i] ^ s0[i];
	wzero(rk, sizeof(rk));
}

int
wpa_ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, size_t hdrlen,
    uint64_t pn, const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t rk[176], a[16], b[16], s0[16], s[16], diff = 0;
	size_t i, plen;
	int j = 0;
	uint16_t ctr = 1;

	if (len < WPA_CCMP_MICLEN)
		return -1;
	plen = len - WPA_CCMP_MICLEN;
	aes128_expand_key(tk, rk);
	ccmp_phase1(rk, hdr, hdrlen, pn, plen, b, a, s0);
	ccmp_ctr(rk, a, ctr, s);
	for (i = 0; i < plen; i++) {
		out[i] = in[i] ^ s[j];
		b[j] ^= out[i];
		if (++j < 16)
			continue;
		aes128_encrypt_block(rk, b, b);
		ccmp_ctr(rk, a, ++ctr, s);
		j = 0;
	}
	if (j != 0)
		aes128_encrypt_block(rk, b, b);
	for (i = 0; i < WPA_CCMP_MICLEN; i++)
		diff |= (uint8_t)((b[i] ^ s0[i]) ^ in[plen + i]);
	wzero(rk, sizeof(rk));
	return diff == 0 ? 0 : -1;
}
