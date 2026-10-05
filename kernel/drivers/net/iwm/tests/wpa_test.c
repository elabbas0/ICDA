/*
 * ICDA port: host unit test for the WPA2 crypto and the supplicant
 * handshake code (wpa_crypto.c, wpa_eapol.c).
 *
 *   make wifi-crypto-test
 *
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../wpa_crypto.h"
#include "../wpa_eapol.h"
#include "crypto/aes.h"

static int failures, checks;

static size_t
unhex(const char *hex, uint8_t *out)
{
	size_t n = 0;

	while (hex[0] && hex[1]) {
		unsigned int v;
		if (hex[0] == ' ') {
			hex++;
			continue;
		}
		sscanf(hex, "%2x", &v);
		out[n++] = (uint8_t)v;
		hex += 2;
	}
	return n;
}

static void
check(const char *name, const uint8_t *got, size_t len, const char *hexexp)
{
	uint8_t exp[256];
	size_t n = unhex(hexexp, exp), i;

	checks++;
	if (n != len || memcmp(got, exp, len) != 0) {
		failures++;
		printf("FAIL %s\n  got  ", name);
		for (i = 0; i < len; i++)
			printf("%02x", got[i]);
		printf("\n  want %s\n", hexexp);
	} else
		printf("ok   %s\n", name);
}

static void
check_true(const char *name, int cond)
{
	checks++;
	if (!cond) {
		failures++;
		printf("FAIL %s\n", name);
	} else
		printf("ok   %s\n", name);
}

/* ---- primitives ------------------------------------------------------- */

static void
test_aes(void)
{
	uint8_t key[16], pt[16], ct[16], rk[176];

	unhex("000102030405060708090a0b0c0d0e0f", key);
	unhex("00112233445566778899aabbccddeeff", pt);
	aes128_expand_key(key, rk);
	aes128_encrypt_block(rk, pt, ct);
	check("AES-128 FIPS-197 C.1", ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");
}

static void
test_hmac(void)
{
	uint8_t key[80], mac[20];
	const uint8_t *addr[1];
	size_t len[1];

	memset(key, 0x0b, 20);
	addr[0] = (const uint8_t *)"Hi There";
	len[0] = 8;
	wpa_hmac_sha1_vec(key, 20, 1, addr, len, mac);
	check("HMAC-SHA1 RFC 2202 #1", mac, 20,
	    "b617318655057264e28bc0b6fb378c8ef146be00");

	addr[0] = (const uint8_t *)"what do ya want for nothing?";
	len[0] = 28;
	wpa_hmac_sha1_vec((const uint8_t *)"Jefe", 4, 1, addr, len, mac);
	check("HMAC-SHA1 RFC 2202 #2", mac, 20,
	    "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

	memset(key, 0xaa, 80);
	addr[0] = (const uint8_t *)
	    "Test Using Larger Than Block-Size Key - Hash Key First";
	len[0] = 54;
	wpa_hmac_sha1_vec(key, 80, 1, addr, len, mac);
	check("HMAC-SHA1 RFC 2202 #6", mac, 20,
	    "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

static void
test_psk(void)
{
	uint8_t pmk[32];

	/* IEEE 802.11i-2004 Annex H.4.1 */
	wpa_passphrase_to_pmk("password", (const uint8_t *)"IEEE", 4, pmk);
	check("PSK H.4.1 #1 password/IEEE", pmk, 32,
	    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
	wpa_passphrase_to_pmk("ThisIsAPassword",
	    (const uint8_t *)"ThisIsASSID", 11, pmk);
	check("PSK H.4.1 #2 ThisIsAPassword/ThisIsASSID", pmk, 32,
	    "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
	wpa_passphrase_to_pmk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	    (const uint8_t *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, pmk);
	check("PSK H.4.1 #3 a*32/Z*32", pmk, 32,
	    "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62");

	check_true("PSK rejects 7-char passphrase",
	    wpa_passphrase_to_pmk("1234567", (const uint8_t *)"x", 1, pmk) != 0);
	check_true("PSK accepts 64 hex digits",
	    wpa_passphrase_to_pmk("000102030405060708090a0b0c0d0e0f"
	    "101112131415161718191a1b1c1d1e1f", (const uint8_t *)"x", 1,
	    pmk) == 0 && pmk[0] == 0x00 && pmk[31] == 0x1f);
}

static void
test_prf(void)
{
	uint8_t key[80], out[64];

	/* IEEE 802.11i-2004 Annex H.3 */
	memset(key, 0x0b, 20);
	wpa_prf_sha1(key, 20, "prefix", (const uint8_t *)"Hi There", 8,
	    out, 64);
	check("PRF H.3 #1", out, 64,
	    "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
	    "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a");
	wpa_prf_sha1((const uint8_t *)"Jefe", 4, "prefix-2",
	    (const uint8_t *)"what do ya want for nothing?", 28, out, 32);
	check("PRF H.3 #2", out, 32,
	    "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c");
	memset(key, 0xaa, 80);
	wpa_prf_sha1(key, 80, "prefix-3", (const uint8_t *)
	    "Test Using Larger Than Block-Size Key - Hash Key First", 54,
	    out, 64);
	check("PRF H.3 #3", out, 64, "0ab6c33ccf70d0d736f4b04c8a7373255511abc5073713163bd0b8c9eeb7e195"
	    "6fa066820a73ddee3f6d3bd407e0682a8b21b58b67358e7a423c3a7b02f154f3");
}

static void
test_ptk(void)
{
	uint8_t pmk[32], aa[6], spa[6], anonce[32], snonce[32], ptk[48];
	int i;

	/* expected values cross-checked with Python hmac/hashlib */
	unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af",
	    pmk);
	unhex("a0a1a1a3a4a5", aa);
	unhex("b0b1b2b3b4b5", spa);
	for (i = 0; i < 32; i++) {
		anonce[i] = (uint8_t)(0xe0 + i);
		snonce[i] = (uint8_t)(0xc0 + i);
	}
	wpa_derive_ptk(pmk, aa, spa, anonce, snonce, ptk, sizeof(ptk));
	check("PTK KCK", ptk, 16, "e79498d36bd81b9b41819b48f3c97f96");
	check("PTK KEK", ptk + 16, 16, "6e620972f3d263238df571d65fa2b8fa");
	check("PTK TK", ptk + 32, 16, "d503e8b5891f81c74422468fc2d4dd39");
}

static void
test_keywrap(void)
{
	uint8_t kek[16], plain[16], cipher[24], back[16];

	/* RFC 3394 section 4.1 */
	unhex("000102030405060708090a0b0c0d0e0f", kek);
	unhex("00112233445566778899aabbccddeeff", plain);
	wpa_aes_wrap(kek, plain, 2, cipher);
	check("AES key wrap RFC 3394 4.1", cipher, 24,
	    "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
	check_true("AES key unwrap RFC 3394 4.1",
	    wpa_aes_unwrap(kek, cipher, 2, back) == 0 &&
	    memcmp(back, plain, 16) == 0);
	cipher[5] ^= 1;
	check_true("AES key unwrap detects tampering",
	    wpa_aes_unwrap(kek, cipher, 2, back) != 0);
}

static void
test_ccmp(void)
{
	uint8_t tk[16], hdr[24], plain[20], out[28], dec[20];
	uint64_t pn = 0xB5039776E70CULL;

	/* IEEE 802.11-2012 M.6.4 (802.11i-2004 H.6.4) CCMP test vector */
	unhex("c97c1f67ce371185514a8a19f2bdd52f", tk);
	unhex("0848 c32c 0fd2e128a57c 5030f1844408 abaea5b8fcba 8033", hdr);
	unhex("f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050", plain);
	wpa_ccmp_encrypt(tk, hdr, 24, pn, plain, 20, out);
	check("CCMP M.6.4 ciphertext", out, 20,
	    "f3d0a2fe9a3dbf2342a643e43246e80c3c04d019");
	check("CCMP M.6.4 MIC", out + 20, 8, "7845ce0b16f97623");
	check_true("CCMP decrypt round trip",
	    wpa_ccmp_decrypt(tk, hdr, 24, pn, out, 28, dec) == 0 &&
	    memcmp(dec, plain, 20) == 0);
	out[3] ^= 0x80;
	check_true("CCMP decrypt detects tampering",
	    wpa_ccmp_decrypt(tk, hdr, 24, pn, out, 28, dec) != 0);
}

/* ---- 4-way and group handshake against a simulated AP ----------------- */

static uint8_t test_snonce_seed;

static void
test_random(void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(test_snonce_seed + 7 * i);
}

static void
wr16(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* Authenticator side: build an EAPOL-Key frame. */
static size_t
ap_frame(uint8_t *f, unsigned int info, uint64_t replay,
    const uint8_t *nonce, const uint8_t *rsc6, const uint8_t *data,
    size_t datalen, unsigned int keylen)
{
	int i;

	memset(f, 0, EK_DATA + datalen);
	f[EK_VERSION] = 2;
	f[EK_TYPE] = 3;
	wr16(f + EK_LEN, (unsigned int)(EK_DATA - 4 + datalen));
	f[EK_DESC] = 2;
	wr16(f + EK_INFO, info | EK_INFO_DESC_V2);
	wr16(f + EK_KEYLEN, keylen);
	for (i = 0; i < 8; i++)
		f[EK_REPLAY + i] = (uint8_t)(replay >> (56 - 8 * i));
	if (nonce)
		memcpy(f + EK_NONCE, nonce, 32);
	if (rsc6)
		memcpy(f + EK_RSC, rsc6, 6);
	wr16(f + EK_PAYLEN, (unsigned int)datalen);
	if (data)
		memcpy(f + EK_DATA, data, datalen);
	return EK_DATA + datalen;
}

static void
ap_sign(uint8_t *f, size_t len, const uint8_t *kck)
{
	memset(f + EK_MIC, 0, 16);
	wpa_eapol_mic_v2(kck, f, len, f + EK_MIC);
}

static int
mic_ok(const uint8_t *f, size_t len, const uint8_t *kck)
{
	uint8_t copy[512], mic[16];

	memcpy(copy, f, len);
	memset(copy + EK_MIC, 0, 16);
	wpa_eapol_mic_v2(kck, copy, len, mic);
	return memcmp(mic, f + EK_MIC, 16) == 0;
}

/* Key Data with a GTK KDE, padded and AES-wrapped with the KEK. */
static size_t
ap_keydata(uint8_t *out, const uint8_t *rsnie, size_t rsnielen,
    const uint8_t *gtk, int kid, const uint8_t *kek)
{
	uint8_t plain[128];
	size_t n = 0;

	if (rsnie) {
		memcpy(plain, rsnie, rsnielen);
		n = rsnielen;
	}
	plain[n++] = 0xdd;
	plain[n++] = 6 + 16;
	plain[n++] = 0x00; plain[n++] = 0x0f; plain[n++] = 0xac;
	plain[n++] = 1;				/* GTK KDE */
	plain[n++] = (uint8_t)(kid & 3);
	plain[n++] = 0;
	memcpy(plain + n, gtk, 16);
	n += 16;
	if (n % 8) {
		plain[n++] = 0xdd;
		while (n % 8)
			plain[n++] = 0;
	}
	wpa_aes_wrap(kek, plain, n / 8, out);
	return n + 8;
}

static void
test_handshake(void)
{
	uint8_t pmk[32], aa[6] = { 2, 0x11, 0x22, 0x33, 0x44, 0x55 };
	uint8_t spa[6] = { 0x34, 0x13, 0xe8, 0x01, 0x02, 0x03 };
	uint8_t ap_ie[64], own_ie[64], anonce[32], snonce[32], ptk[64];
	uint8_t gtk[16], gtk2[16], frame[512], data[256], rsc[6] = { 5 };
	size_t ap_ie_len, own_ie_len, len, dlen;
	struct wpa_sm sm;
	struct wpa_result res;
	int i, rc;

	wpa_passphrase_to_pmk("correct horse battery",
	    (const uint8_t *)"ICDA-test", 9, pmk);
	ap_ie_len = wpa_build_rsn_ie(ap_ie, 0x000c);
	own_ie_len = wpa_build_rsn_ie(own_ie, 0x000c);
	for (i = 0; i < 32; i++)
		anonce[i] = (uint8_t)(0x40 + i);
	for (i = 0; i < 16; i++) {
		gtk[i] = (uint8_t)(0xa0 + i);
		gtk2[i] = (uint8_t)(0x10 + i);
	}

	test_snonce_seed = 0x5a;
	wpa_sm_init(&sm, pmk, aa, spa, ap_ie, ap_ie_len, own_ie, own_ie_len,
	    test_random);

	/* message 1 */
	len = ap_frame(frame, EK_INFO_PAIRWISE | EK_INFO_ACK, 1, anonce, NULL,
	    NULL, 0, 16);
	rc = wpa_sm_rx(&sm, frame, len, &res);
	check_true("4-way msg1 accepted", rc == 0 && res.msg == 1 &&
	    res.tx_len > 0);

	/* the AP checks message 2 */
	memcpy(snonce, res.tx + EK_NONCE, 32);
	wpa_derive_ptk(pmk, aa, spa, anonce, snonce, ptk, sizeof(ptk));
	check_true("msg2 MIC verifies with the AP's PTK",
	    mic_ok(res.tx, res.tx_len, ptk));
	check_true("msg2 key info = pairwise|MIC|v2",
	    (((unsigned)res.tx[EK_INFO] << 8) | res.tx[EK_INFO + 1]) ==
	    (EK_INFO_PAIRWISE | EK_INFO_MIC | EK_INFO_DESC_V2));
	check_true("msg2 carries our RSN IE",
	    res.tx[EK_PAYLEN + 1] == own_ie_len &&
	    memcmp(res.tx + EK_DATA, own_ie, own_ie_len) == 0);
	check_true("msg2 replay counter echoes msg1", res.tx[EK_REPLAY + 7] == 1);

	/* message 3 with the GTK */
	dlen = ap_keydata(data, ap_ie, ap_ie_len, gtk, 1, ptk + 16);
	len = ap_frame(frame, EK_INFO_PAIRWISE | EK_INFO_ACK | EK_INFO_MIC |
	    EK_INFO_INSTALL | EK_INFO_SECURE | EK_INFO_ENCRYPTED, 2, anonce,
	    rsc, data, dlen, 16);
	ap_sign(frame, len, ptk);
	rc = wpa_sm_rx(&sm, frame, len, &res);
	check_true("4-way msg3 accepted", rc == 0 && res.msg == 3 &&
	    res.deauth_reason == 0);
	check_true("msg4 MIC verifies", res.tx_len == EK_DATA &&
	    mic_ok(res.tx, res.tx_len, ptk));
	check_true("PTK installed with the AP's TK", res.install_ptk &&
	    memcmp(res.tk, ptk + 32, 16) == 0);
	check_true("GTK installed (key id 1, RSC 5)", res.install_gtk &&
	    res.gtk_kid == 1 && res.gtk_len == 16 &&
	    memcmp(res.gtk, gtk, 16) == 0 && res.gtk_rsc == 5);
	check_true("port opened", res.port_valid);

	/* a replayed message 3 must be ignored (no key reinstall) */
	rc = wpa_sm_rx(&sm, frame, len, &res);
	check_true("replayed msg3 rejected", rc != 0 && !res.install_ptk);

	/* group key rekey */
	dlen = ap_keydata(data, NULL, 0, gtk2, 2, ptk + 16);
	len = ap_frame(frame, EK_INFO_ACK | EK_INFO_MIC | EK_INFO_SECURE |
	    EK_INFO_ENCRYPTED, 3, NULL, rsc, data, dlen, 16);
	ap_sign(frame, len, ptk);
	rc = wpa_sm_rx(&sm, frame, len, &res);
	check_true("group msg1 accepted", rc == 0 && res.msg == 11);
	check_true("new GTK installed (key id 2)", res.install_gtk &&
	    res.gtk_kid == 2 && memcmp(res.gtk, gtk2, 16) == 0);
	check_true("group msg2 MIC verifies", res.tx_len == EK_DATA &&
	    mic_ok(res.tx, res.tx_len, ptk));

	/* wrong passphrase: message 3 MIC must fail */
	wpa_passphrase_to_pmk("wrong password!", (const uint8_t *)"ICDA-test",
	    9, pmk);
	wpa_sm_init(&sm, pmk, aa, spa, ap_ie, ap_ie_len, own_ie, own_ie_len,
	    test_random);
	len = ap_frame(frame, EK_INFO_PAIRWISE | EK_INFO_ACK, 10, anonce,
	    NULL, NULL, 0, 16);
	(void)wpa_sm_rx(&sm, frame, len, &res);
	dlen = ap_keydata(data, ap_ie, ap_ie_len, gtk, 1, ptk + 16);
	len = ap_frame(frame, EK_INFO_PAIRWISE | EK_INFO_ACK | EK_INFO_MIC |
	    EK_INFO_INSTALL | EK_INFO_SECURE | EK_INFO_ENCRYPTED, 11, anonce,
	    rsc, data, dlen, 16);
	ap_sign(frame, len, ptk);
	rc = wpa_sm_rx(&sm, frame, len, &res);
	check_true("wrong passphrase: msg3 MIC rejected",
	    rc != 0 && !res.install_ptk && res.error != NULL);
}

int
main(void)
{
	test_aes();
	test_hmac();
	test_psk();
	test_prf();
	test_ptk();
	test_keywrap();
	test_ccmp();
	test_handshake();
	printf("\n%d/%d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
