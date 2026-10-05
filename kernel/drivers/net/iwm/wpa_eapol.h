/*
 * ICDA port: WPA2 (RSN) supplicant side of the EAPOL-Key 4-way and group
 * key handshakes, PSK/CCMP only, descriptor version 2.  Pure code; the
 * caller transmits the reply frame and installs the keys it returns.
 *
 * The message checks follow OpenBSD's ieee80211_pae_input.c and
 * ieee80211_pae_output.c:
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

#ifndef ICDA_WPA_EAPOL_H
#define ICDA_WPA_EAPOL_H

#include <stdint.h>
#include <stddef.h>

#include "wpa_crypto.h"

/* EAPOL-Key frame layout (offsets from the EAPOL version octet) */
#define EK_VERSION	0
#define EK_TYPE		1
#define EK_LEN		2
#define EK_DESC		4
#define EK_INFO		5
#define EK_KEYLEN	7
#define EK_REPLAY	9
#define EK_NONCE	17
#define EK_IV		49
#define EK_RSC		65
#define EK_RESERVED	73
#define EK_MIC		81
#define EK_PAYLEN	97
#define EK_DATA		99

#define EK_INFO_VERSION_MASK	0x0007
#define EK_INFO_DESC_V2		0x0002
#define EK_INFO_PAIRWISE	0x0008
#define EK_INFO_INSTALL		0x0040
#define EK_INFO_ACK		0x0080
#define EK_INFO_MIC		0x0100
#define EK_INFO_SECURE		0x0200
#define EK_INFO_ERROR		0x0400
#define EK_INFO_REQUEST		0x0800
#define EK_INFO_ENCRYPTED	0x1000

#define WPA_IE_MAX		64
#define WPA_EAPOL_MAX		512	/* largest frame we build */

enum wpa_supp_state {
	WPA_SUPP_INITIALIZE,	/* not associated / RSN off */
	WPA_SUPP_PTKSTART,	/* associated, waiting for message 1 */
	WPA_SUPP_PTKNEGOTIATING,/* sent message 2 */
	WPA_SUPP_PTKDONE	/* sent message 4; group rekeys allowed */
};

struct wpa_sm {
	int		state;
	uint8_t		pmk[WPA_PMK_LEN];
	uint8_t		aa[6];		/* authenticator (AP) address */
	uint8_t		spa[6];		/* our address */
	uint8_t		anonce[WPA_NONCE_LEN];
	uint8_t		snonce[WPA_NONCE_LEN];
	uint8_t		ptk[64];	/* KCK | KEK | TK */
	uint8_t		tptk[64];
	uint64_t	replay;
	int		replay_ok;
	int		new_ptk;	/* expecting a fresh pairwise key */
	uint8_t		ap_ie[WPA_IE_MAX];	/* AP's RSN IE (beacon) */
	size_t		ap_ie_len;
	uint8_t		own_ie[WPA_IE_MAX];	/* our assoc request RSN IE */
	size_t		own_ie_len;
	unsigned int	group_keylen;	/* 16 for CCMP */
	unsigned int	pairwise_keylen;
	uint8_t		last_gtk[32];	/* to reject GTK reinstallation */
	unsigned int	last_gtk_len;
	/* fills `n' random bytes; must be set by the caller */
	void		(*random)(void *buf, size_t n);
};

struct wpa_result {
	/* reply EAPOL frame (from the version octet); tx_len = 0: none */
	uint8_t		tx[WPA_EAPOL_MAX];
	size_t		tx_len;
	int		msg;		/* handled: 1, 3 (4-way), 11 (group) */
	int		install_ptk;
	uint8_t		tk[16];
	uint64_t	ptk_rsc;
	int		install_gtk;
	uint8_t		gtk[32];
	unsigned int	gtk_len;
	int		gtk_kid;
	int		gtk_tx;
	uint64_t	gtk_rsc;
	int		port_valid;	/* handshake complete: open data port */
	int		deauth_reason;	/* nonzero: leave the BSS */
	const char	*error;		/* why a frame was ignored */
};

#define WPA_REASON_AUTH_LEAVE		3
#define WPA_REASON_RSN_DIFFERENT_IE	17

void	wpa_sm_init(struct wpa_sm *, const uint8_t pmk[WPA_PMK_LEN],
	    const uint8_t aa[6], const uint8_t spa[6],
	    const uint8_t *ap_ie, size_t ap_ie_len,
	    const uint8_t *own_ie, size_t own_ie_len,
	    void (*random)(void *, size_t));

/*
 * Process one received EAPOL frame (starting at the version octet).
 * Returns 0 if the frame was handled (see `res'), -1 if it was dropped
 * (res->error says why).
 */
int	wpa_sm_rx(struct wpa_sm *, const uint8_t *frame, size_t len,
	    struct wpa_result *res);

/*
 * Build the RSN IE we put in the association request: WPA2, group and
 * pairwise CCMP, AKM PSK, no PMF.  Returns its length.
 */
size_t	wpa_build_rsn_ie(uint8_t *buf, uint16_t rsncaps);

#endif /* ICDA_WPA_EAPOL_H */
