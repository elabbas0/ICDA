/*
 * ICDA port: WPA2 supplicant EAPOL-Key handshakes.  See wpa_eapol.h.
 *
 * Message processing follows OpenBSD's ieee80211_pae_input.c
 * (ieee80211_recv_4way_msg1, _msg3, ieee80211_recv_rsn_group_msg1) and
 * ieee80211_pae_output.c (ieee80211_send_4way_msg2, _msg4,
 * ieee80211_send_group_msg2):
 *
 * Copyright (c) 2007,2008 Damien Bergamini <damien.bergamini@free.fr>
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

#include "wpa_eapol.h"

#define EAPOL_VERSION		1
#define EAPOL_KEY		3
#define EAPOL_KEY_DESC_RSN	2
#define ELEMID_RSN		48
#define ELEMID_VENDOR		221
#define KDE_GTK			1

static const uint8_t ieee80211_oui[3] = { 0x00, 0x0f, 0xac };

static void
ecopy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
}

static void
ezero(void *dst, size_t n)
{
	volatile uint8_t *d = dst;

	while (n--)
		*d++ = 0;
}

static int
eequal(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	uint8_t acc = 0;

	while (n--)
		acc |= *x++ ^ *y++;
	return acc == 0;
}

static uint16_t
rd16be(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void
wr16be(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint64_t
rd64be(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

static uint64_t
rd48le(const uint8_t *p)
{
	return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
	    (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40;
}

size_t
wpa_build_rsn_ie(uint8_t *buf, uint16_t rsncaps)
{
	uint8_t *p = buf;

	*p++ = ELEMID_RSN;
	*p++ = 0;		/* length, below */
	*p++ = 1; *p++ = 0;	/* version 1 */
	ecopy(p, ieee80211_oui, 3); p += 3; *p++ = 4;	/* group: CCMP */
	*p++ = 1; *p++ = 0;	/* 1 pairwise cipher */
	ecopy(p, ieee80211_oui, 3); p += 3; *p++ = 4;	/* CCMP */
	*p++ = 1; *p++ = 0;	/* 1 AKM */
	ecopy(p, ieee80211_oui, 3); p += 3; *p++ = 2;	/* PSK */
	*p++ = (uint8_t)rsncaps;
	*p++ = (uint8_t)(rsncaps >> 8);
	buf[1] = (uint8_t)(p - buf - 2);
	return (size_t)(p - buf);
}

void
wpa_sm_init(struct wpa_sm *sm, const uint8_t pmk[WPA_PMK_LEN],
    const uint8_t aa[6], const uint8_t spa[6], const uint8_t *ap_ie,
    size_t ap_ie_len, const uint8_t *own_ie, size_t own_ie_len,
    void (*random)(void *, size_t))
{
	ezero(sm, sizeof(*sm));
	sm->state = WPA_SUPP_PTKSTART;
	ecopy(sm->pmk, pmk, WPA_PMK_LEN);
	ecopy(sm->aa, aa, 6);
	ecopy(sm->spa, spa, 6);
	if (ap_ie_len > WPA_IE_MAX)
		ap_ie_len = WPA_IE_MAX;
	ecopy(sm->ap_ie, ap_ie, ap_ie_len);
	sm->ap_ie_len = ap_ie_len;
	if (own_ie_len > WPA_IE_MAX)
		own_ie_len = WPA_IE_MAX;
	ecopy(sm->own_ie, own_ie, own_ie_len);
	sm->own_ie_len = own_ie_len;
	sm->group_keylen = 16;
	sm->pairwise_keylen = 16;
	sm->random = random;
}

/* Start an EAPOL-Key reply in res->tx; returns a pointer to the frame. */
static uint8_t *
reply_start(struct wpa_result *res, uint16_t info, const uint8_t *replay,
    size_t datalen)
{
	uint8_t *f = res->tx;

	ezero(f, EK_DATA + datalen);
	f[EK_VERSION] = EAPOL_VERSION;
	f[EK_TYPE] = EAPOL_KEY;
	wr16be(f + EK_LEN, (uint16_t)(EK_DATA - 4 + datalen));
	f[EK_DESC] = EAPOL_KEY_DESC_RSN;
	wr16be(f + EK_INFO, (uint16_t)(info | EK_INFO_DESC_V2));
	ecopy(f + EK_REPLAY, replay, 8);
	wr16be(f + EK_PAYLEN, (uint16_t)datalen);
	res->tx_len = EK_DATA + datalen;
	return f;
}

static void
reply_sign(struct wpa_result *res, const uint8_t kck[16])
{
	wpa_eapol_mic_v2(kck, res->tx, res->tx_len, res->tx + EK_MIC);
}

/* MIC check on a copy, so the caller's buffer stays untouched. */
static int
check_mic(const uint8_t *frame, size_t len, const uint8_t kck[16])
{
	uint8_t copy[1024], mic[WPA_MIC_LEN];
	int ok;

	if (len > sizeof(copy))
		return 0;
	ecopy(copy, frame, len);
	ezero(copy + EK_MIC, WPA_MIC_LEN);
	wpa_eapol_mic_v2(kck, copy, len, mic);
	ok = eequal(mic, frame + EK_MIC, WPA_MIC_LEN);
	ezero(copy, len);
	return ok;
}

/*
 * Decrypt the Key Data field (AES Key Wrap with the KEK) into `out'.
 * Returns the plaintext length, or -1.
 */
static int
decrypt_keydata(const uint8_t *data, size_t len, const uint8_t kek[16],
    uint8_t *out, size_t cap)
{
	if (len < 16 + 8 || (len & 7) != 0 || len - 8 > cap)
		return -1;
	if (wpa_aes_unwrap(kek, data, (len - 8) / 8, out) != 0)
		return -1;
	return (int)(len - 8);
}

struct keydata {
	const uint8_t	*rsnie1;
	const uint8_t	*rsnie2;
	const uint8_t	*gtk;
};

static void
parse_keydata(const uint8_t *frm, size_t len, struct keydata *kd)
{
	const uint8_t *efrm = frm + len;

	kd->rsnie1 = kd->rsnie2 = kd->gtk = NULL;
	while (frm + 2 <= efrm) {
		if (frm + 2 + frm[1] > efrm)
			break;
		switch (frm[0]) {
		case ELEMID_RSN:
			if (kd->rsnie1 == NULL)
				kd->rsnie1 = frm;
			else if (kd->rsnie2 == NULL)
				kd->rsnie2 = frm;
			break;
		case ELEMID_VENDOR:
			if (frm[1] >= 4 && eequal(&frm[2], ieee80211_oui, 3) &&
			    frm[5] == KDE_GTK)
				kd->gtk = frm;
			break;
		}
		if (frm[0] == ELEMID_VENDOR && frm[1] == 0)
			break;		/* AES key wrap padding (0xdd 0x00..) */
		frm += 2 + frm[1];
	}
}

static int
install_gtk(struct wpa_sm *sm, const uint8_t *gtk, const uint8_t *rsc,
    struct wpa_result *res)
{
	unsigned int keylen = sm->group_keylen;

	if (gtk[1] != 6 + keylen)
		return -1;
	res->gtk_kid = gtk[6] & 3;
	res->gtk_tx = (gtk[6] >> 2) & 1;
	res->gtk_len = keylen;
	ecopy(res->gtk, &gtk[8], keylen);
	res->gtk_rsc = rd48le(rsc);
	/* Do not reinstall a key we already have (KRACK). */
	if (sm->last_gtk_len == keylen && eequal(sm->last_gtk, &gtk[8], keylen))
		return 0;
	ecopy(sm->last_gtk, &gtk[8], keylen);
	sm->last_gtk_len = keylen;
	res->install_gtk = 1;
	return 0;
}

/* 4-way handshake message 1 (ANonce from the authenticator). */
static int
rx_msg1(struct wpa_sm *sm, const uint8_t *f, size_t len,
    struct wpa_result *res)
{
	(void)len;
	if (sm->state == WPA_SUPP_INITIALIZE) {
		res->error = "msg1: not expecting a handshake";
		return -1;
	}
	if (sm->replay_ok && rd64be(f + EK_REPLAY) <= sm->replay) {
		res->error = "msg1: replayed";
		return -1;
	}
	ecopy(sm->anonce, f + EK_NONCE, WPA_NONCE_LEN);
	sm->random(sm->snonce, WPA_NONCE_LEN);
	wpa_derive_ptk(sm->pmk, sm->aa, sm->spa, sm->anonce, sm->snonce,
	    sm->tptk, sizeof(sm->tptk));
	sm->new_ptk = 1;

	/* message 2: SNonce, our RSN IE, MIC with the TPTK's KCK */
	uint8_t *r = reply_start(res, EK_INFO_PAIRWISE | EK_INFO_MIC,
	    f + EK_REPLAY, sm->own_ie_len);
	ecopy(r + EK_NONCE, sm->snonce, WPA_NONCE_LEN);
	ecopy(r + EK_DATA, sm->own_ie, sm->own_ie_len);
	reply_sign(res, sm->tptk);
	sm->state = WPA_SUPP_PTKNEGOTIATING;
	res->msg = 1;
	return 0;
}

/* 4-way handshake message 3 (install the PTK, receive the GTK). */
static int
rx_msg3(struct wpa_sm *sm, const uint8_t *f, size_t len,
    struct wpa_result *res)
{
	uint8_t plain[256];
	const uint8_t *data;
	struct keydata kd;
	uint16_t info = rd16be(f + EK_INFO);
	size_t paylen = rd16be(f + EK_PAYLEN);
	int n;

	if (sm->state != WPA_SUPP_PTKNEGOTIATING &&
	    sm->state != WPA_SUPP_PTKDONE) {
		res->error = "msg3: unexpected";
		return -1;
	}
	if (sm->replay_ok && rd64be(f + EK_REPLAY) <= sm->replay) {
		res->error = "msg3: replayed";
		return -1;
	}
	if (!eequal(f + EK_NONCE, sm->anonce, WPA_NONCE_LEN)) {
		res->error = "msg3: ANonce differs from msg1";
		return -1;
	}
	wpa_derive_ptk(sm->pmk, sm->aa, sm->spa, sm->anonce, sm->snonce,
	    sm->tptk, sizeof(sm->tptk));
	if (!check_mic(f, len, sm->tptk)) {
		res->error = "msg3: bad MIC (wrong password?)";
		return -1;
	}
	ecopy(sm->ptk, sm->tptk, sizeof(sm->ptk));

	data = f + EK_DATA;
	if (info & EK_INFO_ENCRYPTED) {
		n = decrypt_keydata(data, paylen, sm->ptk + 16, plain,
		    sizeof(plain));
		if (n < 0) {
			res->error = "msg3: key data unwrap failed";
			return -1;
		}
		data = plain;
		paylen = (size_t)n;
	}
	parse_keydata(data, paylen, &kd);
	if (kd.rsnie1 == NULL) {
		res->error = "msg3: no RSN IE";
		return -1;
	}
	if (kd.gtk != NULL && !(info & EK_INFO_ENCRYPTED)) {
		res->error = "msg3: GTK not encrypted";
		return -1;
	}
	if (!(info & EK_INFO_INSTALL)) {
		res->error = "msg3: Install bit clear";
		return -1;
	}
	/* The AP must repeat the RSN IE from its beacon, bit for bit. */
	if ((size_t)kd.rsnie1[1] + 2 != sm->ap_ie_len ||
	    !eequal(kd.rsnie1, sm->ap_ie, sm->ap_ie_len)) {
		res->deauth_reason = WPA_REASON_RSN_DIFFERENT_IE;
		res->error = "msg3: RSN IE differs from beacon";
		return 0;
	}
	sm->replay = rd64be(f + EK_REPLAY);
	sm->replay_ok = 1;

	/* message 4 */
	reply_start(res, EK_INFO_PAIRWISE | EK_INFO_MIC | EK_INFO_SECURE,
	    f + EK_REPLAY, 0);
	reply_sign(res, sm->ptk);
	sm->state = WPA_SUPP_PTKDONE;
	res->msg = 3;

	if (sm->new_ptk) {
		if (rd16be(f + EK_KEYLEN) != sm->pairwise_keylen) {
			res->deauth_reason = WPA_REASON_AUTH_LEAVE;
			res->error = "msg3: bad pairwise key length";
			return 0;
		}
		res->install_ptk = 1;
		ecopy(res->tk, sm->ptk + 32, 16);
		res->ptk_rsc = (kd.gtk == NULL) ? rd48le(f + EK_RSC) : 0;
		sm->new_ptk = 0;
	}
	if (kd.gtk != NULL && install_gtk(sm, kd.gtk, f + EK_RSC, res) != 0) {
		res->deauth_reason = WPA_REASON_AUTH_LEAVE;
		res->error = "msg3: bad GTK length";
		return 0;
	}
	if (info & EK_INFO_SECURE)
		res->port_valid = 1;
	ezero(plain, sizeof(plain));
	return 0;
}

/* Group key handshake message 1 (GTK rekey). */
static int
rx_group_msg1(struct wpa_sm *sm, const uint8_t *f, size_t len,
    struct wpa_result *res)
{
	uint8_t plain[256];
	struct keydata kd;
	uint16_t info = rd16be(f + EK_INFO);
	int n;

	if (sm->state != WPA_SUPP_PTKDONE) {
		res->error = "group msg1: 4-way handshake not done";
		return -1;
	}
	if (rd64be(f + EK_REPLAY) <= sm->replay) {
		res->error = "group msg1: replayed";
		return -1;
	}
	if (!check_mic(f, len, sm->ptk)) {
		res->error = "group msg1: bad MIC";
		return -1;
	}
	if (!(info & EK_INFO_ENCRYPTED)) {
		res->error = "group msg1: key data not encrypted";
		return -1;
	}
	n = decrypt_keydata(f + EK_DATA, rd16be(f + EK_PAYLEN), sm->ptk + 16,
	    plain, sizeof(plain));
	if (n < 0) {
		res->error = "group msg1: key data unwrap failed";
		return -1;
	}
	parse_keydata(plain, (size_t)n, &kd);
	if (kd.gtk == NULL) {
		res->error = "group msg1: no GTK KDE";
		return -1;
	}
	if (install_gtk(sm, kd.gtk, f + EK_RSC, res) != 0) {
		res->error = "group msg1: bad GTK length";
		return -1;
	}
	if (info & EK_INFO_SECURE)
		res->port_valid = 1;
	sm->replay = rd64be(f + EK_REPLAY);

	reply_start(res, EK_INFO_MIC | EK_INFO_SECURE, f + EK_REPLAY, 0);
	reply_sign(res, sm->ptk);
	res->msg = 11;
	ezero(plain, sizeof(plain));
	return 0;
}

int
wpa_sm_rx(struct wpa_sm *sm, const uint8_t *f, size_t len,
    struct wpa_result *res)
{
	uint16_t info, bodylen, paylen;

	ezero(res, sizeof(*res));
	if (len < EK_DATA) {
		res->error = "short EAPOL frame";
		return -1;
	}
	if (f[EK_TYPE] != EAPOL_KEY) {
		res->error = "not an EAPOL-Key frame";
		return -1;
	}
	if (f[EK_DESC] != EAPOL_KEY_DESC_RSN) {
		res->error = "not an RSN key descriptor (WPA1?)";
		return -1;
	}
	bodylen = rd16be(f + EK_LEN);
	if ((size_t)bodylen + 4 > len || (size_t)bodylen + 4 < EK_DATA) {
		res->error = "bad EAPOL body length";
		return -1;
	}
	len = (size_t)bodylen + 4;	/* ignore trailing padding */
	paylen = rd16be(f + EK_PAYLEN);
	if ((size_t)paylen > len - EK_DATA) {
		res->error = "bad key data length";
		return -1;
	}
	info = rd16be(f + EK_INFO);
	if ((info & EK_INFO_VERSION_MASK) != EK_INFO_DESC_V2) {
		res->error = "unsupported key descriptor version";
		return -1;
	}
	if (info & EK_INFO_REQUEST) {
		res->error = "EAPOL-Key request from AP";
		return -1;
	}
	if (info & EK_INFO_PAIRWISE) {
		if ((info & EK_INFO_MIC) && (info & EK_INFO_ACK))
			return rx_msg3(sm, f, len, res);
		if (!(info & EK_INFO_MIC) && (info & EK_INFO_ACK))
			return rx_msg1(sm, f, len, res);
		res->error = "unexpected pairwise message";
		return -1;
	}
	if ((info & EK_INFO_MIC) && (info & EK_INFO_ACK))
		return rx_group_msg1(sm, f, len, res);
	res->error = "unexpected group message";
	return -1;
}
