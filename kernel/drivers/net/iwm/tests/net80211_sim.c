/*
 * ICDA port: host simulation of the station side of the Wi-Fi stack.
 * Drives the real net80211.c, wpa_eapol.c, wpa_crypto.c and the mbuf
 * code of iwm_compat.c against a simulated WPA2-PSK access point:
 * beacon -> scan -> auth -> assoc -> 4-way handshake -> encrypted data.
 * The iwm driver itself is replaced by a few callbacks (the hardware
 * cannot be emulated).
 *
 *   make wifi-sim-test
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

/* Built with -include tests/host/rename.h: printf etc. are the kernel's. */
#include "../net80211.h"
#include "../wpa_crypto.h"
#include "../wpa_eapol.h"

extern int sim_quiet;
void exit(int);

static int checks, failures;

static void
check(const char *name, int cond)
{
	checks++;
	if (!cond)
		failures++;
	sim_quiet = 0;
	printf("%s %s\n", cond ? "ok  " : "FAIL", name);
}

static struct ieee80211com ic_store;
static struct ieee80211com *ic = &ic_store;
static int (*net80211_newstate)(struct ieee80211com *, enum ieee80211_state,
    int);

static const uint8_t sta[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t ap[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01 };
static const uint8_t gw[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x99 };

/* driver stand-ins */
static uint8_t hw_tk[16];
static int hw_tk_set, link_events, link_is_up;
static uint8_t rx_frame[2048];
static int rx_len, rx_count;

static int
sim_newstate(struct ieee80211com *c, enum ieee80211_state nstate, int arg)
{
	/* iwm: SCAN is handled by the driver without calling net80211 */
	if (nstate == IEEE80211_S_SCAN) {
		c->ic_state = IEEE80211_S_SCAN;
		return 0;
	}
	return net80211_newstate(c, nstate, arg);
}

static int
sim_set_key(struct ieee80211com *c, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	/* like iwm_set_key(): pairwise CCMP in hardware, the rest in software */
	if ((k->k_flags & IEEE80211_KEY_GROUP) ||
	    k->k_cipher != IEEE80211_CIPHER_CCMP)
		return ieee80211_set_key(c, ni, k);
	memcpy(hw_tk, k->k_key, 16);
	hw_tk_set = 1;
	return 0;
}

static void
sim_input(struct ifnet *ifp, struct mbuf *m)
{
	(void)ifp;
	memcpy(rx_frame, m->m_data, (size_t)m->m_len);
	rx_len = m->m_len;
	rx_count++;
	m_freem(m);
}

static void
sim_link(struct ifnet *ifp)
{
	link_events++;
	link_is_up = LINK_STATE_IS_UP(ifp->if_link_state);
}

/* ---- frames from the AP ------------------------------------------------- */

static void
deliver(const uint8_t *frame, size_t len, int rxflags)
{
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct ieee80211_rxinfo rxi;
	struct ieee80211_node *ni;
	struct mbuf *m = m_get_lead(64, (u_int)len);

	memcpy(m->m_data, frame, len);
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = 60;
	rxi.rxi_chan = 6;
	rxi.rxi_flags = (u_int32_t)rxflags;
	ni = ieee80211_find_rxnode(ic, mtod(m, struct ieee80211_frame *));
	ieee80211_inputm(&ic->ic_if, m, ni, &rxi, &ml);
	ieee80211_release_node(ic, ni);
	if_input(&ic->ic_if, &ml);
}

static uint16_t ap_seq;

static size_t
mgmt_hdr(uint8_t *f, int subtype, const uint8_t *da)
{
	memset(f, 0, 24);
	f[0] = (uint8_t)(IEEE80211_FC0_TYPE_MGT | subtype);
	memcpy(f + 4, da, 6);
	memcpy(f + 10, ap, 6);
	memcpy(f + 16, ap, 6);
	f[22] = (uint8_t)(ap_seq << 4);
	f[23] = (uint8_t)(ap_seq >> 4);
	ap_seq++;
	return 24;
}

static uint8_t ap_rsnie[64];
static size_t ap_rsnie_len;

static void
send_beacon(void)
{
	uint8_t f[256];
	size_t n = mgmt_hdr(f, IEEE80211_FC0_SUBTYPE_BEACON, etherbroadcastaddr);
	static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c,
	    0x12, 0x18, 0x24 };
	static const uint8_t xrates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6c };

	memset(f + n, 0, 8); n += 8;		/* timestamp */
	f[n++] = 100; f[n++] = 0;		/* beacon interval */
	f[n++] = 0x11; f[n++] = 0x04;		/* ESS | privacy | short slot */
	f[n++] = 0; f[n++] = 7;
	memcpy(f + n, "ICDA-AP", 7); n += 7;
	memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
	f[n++] = 3; f[n++] = 1; f[n++] = 6;	/* DS params: channel 6 */
	f[n++] = 5; f[n++] = 4; f[n++] = 0; f[n++] = 1; f[n++] = 0; f[n++] = 0;
	memcpy(f + n, xrates, sizeof(xrates)); n += sizeof(xrates);
	memcpy(f + n, ap_rsnie, ap_rsnie_len); n += ap_rsnie_len;
	deliver(f, n, 0);
}

/* Data frame from the AP (FromDS) carrying an LLC/SNAP payload. */
static size_t
data_hdr(uint8_t *f, const uint8_t *da, const uint8_t *sa, int protect)
{
	memset(f, 0, 24);
	f[0] = IEEE80211_FC0_TYPE_DATA;
	f[1] = IEEE80211_FC1_DIR_FROMDS | (protect ? IEEE80211_FC1_PROTECTED : 0);
	memcpy(f + 4, da, 6);
	memcpy(f + 10, ap, 6);
	memcpy(f + 16, sa, 6);
	f[22] = (uint8_t)(ap_seq << 4);
	f[23] = (uint8_t)(ap_seq >> 4);
	ap_seq++;
	return 24;
}

static size_t
llc(uint8_t *p, uint16_t type)
{
	p[0] = 0xaa; p[1] = 0xaa; p[2] = 0x03;
	p[3] = p[4] = p[5] = 0;
	p[6] = (uint8_t)(type >> 8);
	p[7] = (uint8_t)type;
	return 8;
}

static void
send_eapol(const uint8_t *eapol, size_t len)
{
	uint8_t f[600];
	size_t n = data_hdr(f, sta, ap, 0);

	n += llc(f + n, 0x888e);
	memcpy(f + n, eapol, len);
	deliver(f, n + len, 0);
}

/* CCMP-protected data frame; `hw' simulates hardware decryption. */
static void
send_ccmp(const uint8_t *da, const uint8_t *tk, int kid, uint64_t pn,
    const uint8_t *payload, size_t plen, int hw)
{
	uint8_t f[1600], body[1500];
	size_t n = data_hdr(f, da, gw, 1), blen;
	uint8_t *iv = f + n;

	iv[0] = (uint8_t)pn; iv[1] = (uint8_t)(pn >> 8); iv[2] = 0;
	iv[3] = (uint8_t)(0x20 | kid << 6);
	iv[4] = (uint8_t)(pn >> 16); iv[5] = (uint8_t)(pn >> 24);
	iv[6] = (uint8_t)(pn >> 32); iv[7] = (uint8_t)(pn >> 40);
	n += 8;
	blen = llc(body, 0x0800);
	memcpy(body + blen, payload, plen);
	blen += plen;
	if (hw) {
		/* firmware decrypted it; the IV is left in place */
		memcpy(f + n, body, blen);
		deliver(f, n + blen, IEEE80211_RXI_HWDEC);
	} else {
		wpa_ccmp_encrypt(tk, f, 24, pn, body, blen, f + n);
		deliver(f, n + blen + 8, 0);
	}
}

/* ---- frames to the AP ------------------------------------------------- */

static struct mbuf *
next_tx(void)
{
	return mq_dequeue(&ic->ic_mgtq);
}

static void
ap_frame_eapol(uint8_t *f, unsigned int info, uint64_t replay,
    const uint8_t *nonce, const uint8_t *data, size_t dlen, size_t *len)
{
	int i;

	memset(f, 0, EK_DATA + dlen);
	f[0] = 2; f[1] = 3;
	f[2] = (uint8_t)((EK_DATA - 4 + dlen) >> 8);
	f[3] = (uint8_t)(EK_DATA - 4 + dlen);
	f[EK_DESC] = 2;
	info |= EK_INFO_DESC_V2;
	f[EK_INFO] = (uint8_t)(info >> 8); f[EK_INFO + 1] = (uint8_t)info;
	f[EK_KEYLEN + 1] = 16;
	for (i = 0; i < 8; i++)
		f[EK_REPLAY + i] = (uint8_t)(replay >> (56 - 8 * i));
	if (nonce)
		memcpy(f + EK_NONCE, nonce, 32);
	f[EK_PAYLEN] = (uint8_t)(dlen >> 8); f[EK_PAYLEN + 1] = (uint8_t)dlen;
	if (data)
		memcpy(f + EK_DATA, data, dlen);
	*len = EK_DATA + dlen;
}

int
main(void)
{
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_frame *wh;
	struct mbuf *m;
	uint8_t pmk[32], anonce[32], snonce[32], ptk[64], gtk[16];
	uint8_t eapol[600], kd[128], wrapped[136];
	size_t len, n;
	int i;

	/* ---- attach, like iwm_icda_attach() ---- */
	sim_quiet = 1;
	memcpy(ic->ic_myaddr, sta, 6);
	for (i = 1; i <= 13; i++) {
		ic->ic_channels[i].ic_freq =
		    (u_int16_t)ieee80211_ieee2mhz((u_int)i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_CCK |
		    IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	ic->ic_caps = IEEE80211_C_RSN | IEEE80211_C_SHSLOT |
	    IEEE80211_C_SHPREAMBLE | IEEE80211_C_SCANALL;
	ic->ic_ibss_chan = &ic->ic_channels[1];
	ic->ic_max_rssi = 100;
	ic->ic_set_key = sim_set_key;
	ic->ic_opmode = IEEE80211_M_STA;
	ifp->if_input_icda = sim_input;
	ifp->if_link_icda = sim_link;
	ieee80211_ifattach(ifp);
	net80211_newstate = ic->ic_newstate;
	ic->ic_newstate = sim_newstate;
	check("attach: ic_bss allocated", ic->ic_bss != NULL);

	/* ---- mbuf sanity ---- */
	m = m_get_lead(16, 100);
	memset(m->m_data, 0x5a, 100);
	{
		struct mbuf *c = m_copym(m, 10, M_COPYALL, M_DONTWAIT);
		check("m_copym shares the buffer", c->m_buf == m->m_buf &&
		    c->m_buf->refs == 2 && c->m_len == 90);
		m_freem(c);
		check("m_freem drops a reference", m->m_buf->refs == 1);
	}
	M_PREPEND(m, 40, M_DONTWAIT);	/* needs a new buffer */
	check("M_PREPEND without lead room copies", m != NULL &&
	    m->m_len == 140 && (uint8_t)m->m_data[40] == 0x5a &&
	    (uint8_t)m->m_data[139] == 0x5a);
	m_freem(m);

	/* ---- scan ---- */
	wpa_passphrase_to_pmk("correct horse battery",
	    (const uint8_t *)"ICDA-AP", 7, pmk);
	ap_rsnie_len = wpa_build_rsn_ie(ap_rsnie, 0x000c);
	ieee80211_icda_configure(ic, "ICDA-AP", 7, pmk);
	ieee80211_begin_scan(ifp);
	check("begin_scan -> SCAN", ic->ic_state == IEEE80211_S_SCAN);
	send_beacon();
	check("beacon creates a scan entry", ic->ic_nnodes == 1 &&
	    ieee80211_find_node(ic, ap) != NULL);
	{
		struct ieee80211_node *ni = ieee80211_find_node(ic, ap);
		check("scan entry: ssid, channel 6, WPA2-PSK/CCMP",
		    ni->ni_esslen == 7 && memcmp(ni->ni_essid, "ICDA-AP", 7) == 0 &&
		    ieee80211_chan2ieee(ic, ni->ni_chan) == 6 &&
		    (ni->ni_rsnprotos & IEEE80211_PROTO_RSN) &&
		    (ni->ni_rsnakms & IEEE80211_AKM_PSK) &&
		    ni->ni_rsngroupcipher == IEEE80211_CIPHER_CCMP &&
		    ni->ni_rates.rs_nrates == 12);
	}

	/* ---- join, auth ---- */
	ieee80211_end_scan(ifp);
	check("end_scan joins -> AUTH", ic->ic_state == IEEE80211_S_AUTH &&
	    IEEE80211_ADDR_EQ(ic->ic_bss->ni_bssid, ap));
	m = next_tx();
	wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
	check("auth request sent to the AP", wh != NULL &&
	    (wh->i_fc[0] & 0xfc) == (IEEE80211_FC0_TYPE_MGT |
	    IEEE80211_FC0_SUBTYPE_AUTH) && IEEE80211_ADDR_EQ(wh->i_addr1, ap) &&
	    ((uint8_t *)(wh + 1))[2] == 1);
	if (m) {
		ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
		m_freem(m);
	}
	{
		uint8_t f[64];
		n = mgmt_hdr(f, IEEE80211_FC0_SUBTYPE_AUTH, sta);
		f[n++] = 0; f[n++] = 0; f[n++] = 2; f[n++] = 0;
		f[n++] = 0; f[n++] = 0;
		deliver(f, n, 0);
	}
	check("auth response -> ASSOC", ic->ic_state == IEEE80211_S_ASSOC);

	/* ---- assoc ---- */
	m = next_tx();
	{
		const uint8_t *body, *end, *rsn = NULL;
		wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
		check("association request sent", wh != NULL &&
		    (wh->i_fc[0] & 0xf0) == IEEE80211_FC0_SUBTYPE_ASSOC_REQ);
		if (wh) {
			body = (const uint8_t *)(wh + 1) + 4;
			end = (const uint8_t *)wh + m->m_len;
			for (; body + 2 <= end; body += 2 + body[1])
				if (body[0] == IEEE80211_ELEMID_RSN)
					rsn = body;
			check("assoc request carries our RSN IE (PSK/CCMP)",
			    rsn != NULL && rsn[1] == 20 && rsn[19] == 2);
			ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
			m_freem(m);
		}
	}
	{
		uint8_t f[64];
		static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96,
		    0x0c, 0x12, 0x18, 0x24 };
		n = mgmt_hdr(f, IEEE80211_FC0_SUBTYPE_ASSOC_RESP, sta);
		f[n++] = 0x11; f[n++] = 0x04;	/* capinfo */
		f[n++] = 0; f[n++] = 0;		/* status */
		f[n++] = 1; f[n++] = 0xc0;	/* AID 1 */
		memcpy(f + n, rates, sizeof(rates)); n += sizeof(rates);
		deliver(f, n, 0);
	}
	check("association response -> RUN, link still down",
	    ic->ic_state == IEEE80211_S_RUN && !link_is_up &&
	    ic->ic_bss->ni_associd == 0xc001);

	/* data before the handshake must not pass */
	ifp->if_ipackets = 0;
	rx_count = 0;
	{
		uint8_t f[128];
		n = data_hdr(f, sta, gw, 0);
		n += llc(f + n, 0x0800);
		memset(f + n, 0x45, 40);
		deliver(f, n + 40, 0);
	}
	check("unprotected data dropped before the port opens", rx_count == 0);

	/* ---- 4-way handshake ---- */
	for (i = 0; i < 32; i++)
		anonce[i] = (uint8_t)(0x80 + i);
	ap_frame_eapol(eapol, EK_INFO_PAIRWISE | EK_INFO_ACK, 1, anonce, NULL,
	    0, &len);
	send_eapol(eapol, len);
	m = next_tx();
	wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
	check("message 2 sent as a ToDS EAPOL data frame", wh != NULL &&
	    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_TODS &&
	    !(wh->i_fc[1] & IEEE80211_FC1_PROTECTED) &&
	    ((uint8_t *)(wh + 1))[6] == 0x88 && ((uint8_t *)(wh + 1))[7] == 0x8e);
	if (wh) {
		uint8_t *e = (uint8_t *)(wh + 1) + 8;
		size_t elen = (size_t)m->m_len - 24 - 8;
		uint8_t copy[300], mic[16];

		memcpy(snonce, e + EK_NONCE, 32);
		wpa_derive_ptk(pmk, ap, sta, anonce, snonce, ptk, sizeof(ptk));
		memcpy(copy, e, elen);
		memset(copy + EK_MIC, 0, 16);
		wpa_eapol_mic_v2(ptk, copy, elen, mic);
		check("message 2 MIC verifies at the AP",
		    memcmp(mic, e + EK_MIC, 16) == 0);
		ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
		m_freem(m);
	}

	/* message 3: RSN IE + GTK KDE, wrapped with the KEK */
	for (i = 0; i < 16; i++)
		gtk[i] = (uint8_t)(0xc0 + i);
	n = 0;
	memcpy(kd, ap_rsnie, ap_rsnie_len); n += ap_rsnie_len;
	kd[n++] = 0xdd; kd[n++] = 22; kd[n++] = 0; kd[n++] = 0x0f;
	kd[n++] = 0xac; kd[n++] = 1; kd[n++] = 1; kd[n++] = 0;
	memcpy(kd + n, gtk, 16); n += 16;
	if (n % 8) {
		kd[n++] = 0xdd;
		while (n % 8)
			kd[n++] = 0;
	}
	wpa_aes_wrap(ptk + 16, kd, n / 8, wrapped);
	ap_frame_eapol(eapol, EK_INFO_PAIRWISE | EK_INFO_ACK | EK_INFO_MIC |
	    EK_INFO_INSTALL | EK_INFO_SECURE | EK_INFO_ENCRYPTED, 2, anonce,
	    wrapped, n + 8, &len);
	wpa_eapol_mic_v2(ptk, eapol, len, eapol + EK_MIC);
	send_eapol(eapol, len);
	m = next_tx();
	wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
	check("message 4 sent unencrypted (before the PTK is used)",
	    wh != NULL && !(wh->i_fc[1] & IEEE80211_FC1_PROTECTED));
	if (m) {
		ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
		m_freem(m);
	}
	check("PTK installed in 'hardware'", hw_tk_set &&
	    memcmp(hw_tk, ptk + 32, 16) == 0);
	check("GTK installed for software CCMP",
	    (ic->ic_nw_keys[1].k_flags & IEEE80211_KEY_SWCRYPTO) &&
	    memcmp(ic->ic_nw_keys[1].k_key, gtk, 16) == 0);
	check("port open, link up", ic->ic_bss->ni_port_valid && link_is_up &&
	    (ic->ic_bss->ni_flags & IEEE80211_NODE_TXRXPROT) ==
	    IEEE80211_NODE_TXRXPROT);

	/* ---- encrypted data ---- */
	{
		uint8_t payload[60];
		for (i = 0; i < 60; i++)
			payload[i] = (uint8_t)i;
		rx_count = 0;
		send_ccmp(etherbroadcastaddr, gtk, 1, 7, payload, 60, 0);
		check("broadcast under the GTK decrypted in software",
		    rx_count == 1 && rx_len == 14 + 60 &&
		    memcmp(rx_frame, etherbroadcastaddr, 6) == 0 &&
		    memcmp(rx_frame + 6, gw, 6) == 0 &&
		    rx_frame[12] == 0x08 && rx_frame[13] == 0x00 &&
		    memcmp(rx_frame + 14, payload, 60) == 0);
		send_ccmp(etherbroadcastaddr, gtk, 1, 7, payload, 60, 0);
		check("replayed broadcast dropped", rx_count == 1);
		send_ccmp(sta, NULL, 0, 3, payload, 60, 1);
		check("hardware-decrypted unicast: IV stripped",
		    rx_count == 2 && rx_len == 14 + 60 &&
		    memcmp(rx_frame, sta, 6) == 0 &&
		    memcmp(rx_frame + 14, payload, 60) == 0);
		send_ccmp(sta, NULL, 0, 3, payload, 60, 1);
		check("hardware-decrypted replay dropped", rx_count == 2);
	}

	/* ---- transmit ---- */
	{
		struct ieee80211_node *ni;
		uint8_t eth[14 + 40];

		memcpy(eth, gw, 6);
		memcpy(eth + 6, sta, 6);
		eth[12] = 0x08; eth[13] = 0x00;
		memset(eth + 14, 0x77, 40);
		m = m_get_lead(128, sizeof(eth));
		memcpy(m->m_data, eth, sizeof(eth));
		m = ieee80211_encap(ifp, m, &ni);
		wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
		check("encap: ToDS + Protected, A1=BSSID A2=us A3=dest",
		    wh != NULL && wh->i_fc[1] ==
		    (IEEE80211_FC1_DIR_TODS | IEEE80211_FC1_PROTECTED) &&
		    IEEE80211_ADDR_EQ(wh->i_addr1, ap) &&
		    IEEE80211_ADDR_EQ(wh->i_addr2, sta) &&
		    IEEE80211_ADDR_EQ(wh->i_addr3, gw) &&
		    m->m_len == 24 + 8 + 40 &&
		    ((uint8_t *)(wh + 1))[6] == 0x08);
		if (m) {
			ieee80211_release_node(ic, ni);
			m_freem(m);
		}
	}

	/* ---- group rekey ---- */
	{
		uint8_t gtk2[16];
		for (i = 0; i < 16; i++)
			gtk2[i] = (uint8_t)(0x30 + i);
		n = 0;
		kd[n++] = 0xdd; kd[n++] = 22; kd[n++] = 0; kd[n++] = 0x0f;
		kd[n++] = 0xac; kd[n++] = 1; kd[n++] = 2; kd[n++] = 0;
		memcpy(kd + n, gtk2, 16); n += 16;
		wpa_aes_wrap(ptk + 16, kd, n / 8, wrapped);
		ap_frame_eapol(eapol, EK_INFO_ACK | EK_INFO_MIC |
		    EK_INFO_SECURE | EK_INFO_ENCRYPTED, 3, NULL, wrapped,
		    n + 8, &len);
		wpa_eapol_mic_v2(ptk, eapol, len, eapol + EK_MIC);
		/* rekeys arrive encrypted with the PTK: hardware decrypts */
		{
			uint8_t f[600], body[600];
			size_t fn = data_hdr(f, sta, ap, 1), bl;
			uint8_t *iv = f + fn;
			memset(iv, 0, 8);
			iv[0] = 9; iv[3] = 0x20;
			fn += 8;
			bl = llc(body, 0x888e);
			memcpy(body + bl, eapol, len);
			memcpy(f + fn, body, bl + len);
			deliver(f, fn + bl + len, IEEE80211_RXI_HWDEC);
		}
		m = next_tx();
		wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
		check("group message 2 sent (protected now)", wh != NULL &&
		    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED));
		if (m) {
			ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
			m_freem(m);
		}
		check("new GTK (key 2) installed",
		    memcmp(ic->ic_nw_keys[2].k_key, gtk2, 16) == 0);
	}

	/* ---- deauth by the AP: re-authenticate ---- */
	{
		uint8_t f[64];
		n = mgmt_hdr(f, IEEE80211_FC0_SUBTYPE_DEAUTH, sta);
		f[n++] = 7; f[n++] = 0;
		deliver(f, n, 0);
		m = next_tx();
		wh = m ? mtod(m, struct ieee80211_frame *) : NULL;
		check("deauth -> AUTH, link down, new auth request",
		    ic->ic_state == IEEE80211_S_AUTH && !link_is_up &&
		    wh != NULL && (wh->i_fc[0] & 0xf0) ==
		    IEEE80211_FC0_SUBTYPE_AUTH);
		if (m) {
			ieee80211_release_node(ic, m->m_pkthdr.ph_cookie);
			m_freem(m);
		}
	}

	printf("\n%d/%d simulation checks passed\n", checks - failures, checks);
	exit(failures ? 1 : 0);
	return 0;
}
