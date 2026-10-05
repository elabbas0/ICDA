/*
 * ICDA port: compact, station-only net80211 for iwm(4).  See net80211.h.
 *
 * This file is derived from OpenBSD's net80211 (ieee80211.c,
 * ieee80211_node.c, ieee80211_proto.c, ieee80211_input.c,
 * ieee80211_output.c, ieee80211_crypto.c, ieee80211_pae_*.c), reduced to
 * what a WPA2-PSK client needs.  Those files carry these notices:
 *
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * Copyright (c) 2007-2009 Damien Bergamini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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

#include "net80211.h"
#include "wpa_crypto.h"
#include "wpa_eapol.h"


/* Lead room for every frame we build: 802.11 + LLC + Ethernet headers. */
#define FRAME_LEAD	96

const char * const ieee80211_state_name[IEEE80211_S_MAX] = {
	"INIT", "SCAN", "AUTH", "ASSOC", "RUN"
};

const char * const ieee80211_phymode_name[] = {
	"auto", "11a", "11b", "11g", "11n", "11ac", "11ax"
};

const struct ieee80211_rateset ieee80211_std_rateset_11a =
	{ 8, { 12, 18, 24, 36, 48, 72, 96, 108 } };
const struct ieee80211_rateset ieee80211_std_rateset_11b =
	{ 4, { 2, 4, 11, 22 } };
const struct ieee80211_rateset ieee80211_std_rateset_11g =
	{ 12, { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 } };

/* EDCA parameters for a non-AP STA (802.11-2012 Table 8-105) */
static const struct ieee80211_edca_ac_params
    edca_table[IEEE80211_MODE_MAX][EDCA_NUM_AC] = {
	[IEEE80211_MODE_11B] = {
		[EDCA_AC_BK] = { 5, 10, 7,   0, 0 },
		[EDCA_AC_BE] = { 5, 10, 3,   0, 0 },
		[EDCA_AC_VI] = { 4,  5, 2, 188, 0 },
		[EDCA_AC_VO] = { 3,  4, 2, 102, 0 }
	},
	[IEEE80211_MODE_11A] = {
		[EDCA_AC_BK] = { 4, 10, 7,   0, 0 },
		[EDCA_AC_BE] = { 4, 10, 3,   0, 0 },
		[EDCA_AC_VI] = { 3,  4, 2,  94, 0 },
		[EDCA_AC_VO] = { 2,  3, 2,  47, 0 }
	},
	[IEEE80211_MODE_11G] = {
		[EDCA_AC_BK] = { 4, 10, 7,   0, 0 },
		[EDCA_AC_BE] = { 4, 10, 3,   0, 0 },
		[EDCA_AC_VI] = { 3,  4, 2,  94, 0 },
		[EDCA_AC_VO] = { 2,  3, 2,  47, 0 }
	},
};

/* Supplicant state for our single association. */
static struct wpa_sm	wpa_sm;
static uint8_t		wpa_own_ie[WPA_IE_MAX];
static size_t		wpa_own_ie_len;

/* Last failure, for the status display in wifi.c */
char ieee80211_icda_last_error[96];

static void
set_error(const char *msg)
{
	strlcpy(ieee80211_icda_last_error, msg,
	    sizeof(ieee80211_icda_last_error));
	printf("wlan: %s\n", msg);
}

static void ieee80211_reset_erp(struct ieee80211com *);
static void ieee80211_node_newstate(struct ieee80211_node *, int);
static void ieee80211_node_join_bss(struct ieee80211com *,
	    struct ieee80211_node *);

/* ==== attach, channels, rates ========================================== */

static struct ieee80211_node *
ieee80211_node_alloc_default(struct ieee80211com *ic)
{
	(void)ic;
	return malloc(sizeof(struct ieee80211_node), M_DEVBUF, M_ZERO);
}

static void
ieee80211_node_free_default(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	ieee80211_node_cleanup(ic, ni);
	free(ni, M_DEVBUF, 0);
}

static void
ieee80211_node_copy_default(struct ieee80211com *ic,
    struct ieee80211_node *dst, const struct ieee80211_node *src)
{
	ieee80211_node_cleanup(ic, dst);
	*dst = *src;
	dst->ni_rsnie = NULL;
	if (src->ni_rsnie != NULL) {
		dst->ni_rsnie = malloc(2 + src->ni_rsnie[1], M_DEVBUF, 0);
		if (dst->ni_rsnie)
			memcpy(dst->ni_rsnie, src->ni_rsnie,
			    2 + src->ni_rsnie[1]);
	}
}

static int
ieee80211_node_checkrssi_default(struct ieee80211com *ic,
    const struct ieee80211_node *ni)
{
	uint8_t thres;

	if (ni->ni_chan == IEEE80211_CHAN_ANYC || ic->ic_max_rssi == 0)
		return 0;
	thres = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan) ?
	    IEEE80211_RSSI_THRES_RATIO_2GHZ : IEEE80211_RSSI_THRES_RATIO_5GHZ;
	return (ni->ni_rssi * 100) / ic->ic_max_rssi >= thres;
}

static void
ieee80211_setup_node(struct ieee80211com *ic, struct ieee80211_node *ni,
    const u_int8_t *macaddr)
{
	int i;

	IEEE80211_ADDR_COPY(ni->ni_macaddr, macaddr);
	ni->ni_state = IEEE80211_STA_CACHE;
	ni->ni_ic = ic;
	ni->ni_rxseq = 0xffffU;
	for (i = 0; i < IEEE80211_NUM_TID; i++)
		ni->ni_qos_rxseqs[i] = 0xffffU;
	ni->ni_chan = IEEE80211_CHAN_ANYC;
}

static void
ieee80211_setbasicrates(struct ieee80211com *ic)
{
	static const struct ieee80211_rateset basic[] = {
	    { 0, { 0 } },			/* AUTO */
	    { 3, { 12, 24, 48 } },		/* 11A */
	    { 2, { 2, 4 } },			/* 11B */
	    { 4, { 2, 4, 11, 22 } },		/* 11G */
	};
	int mode, i, j;

	for (mode = 0; mode < IEEE80211_MODE_MAX; mode++) {
		struct ieee80211_rateset *rs = &ic->ic_sup_rates[mode];

		for (i = 0; i < rs->rs_nrates; i++) {
			rs->rs_rates[i] &= IEEE80211_RATE_VAL;
			if (mode >= (int)nitems(basic))
				continue;
			for (j = 0; j < basic[mode].rs_nrates; j++) {
				if (basic[mode].rs_rates[j] == rs->rs_rates[i]) {
					rs->rs_rates[i] |= IEEE80211_RATE_BASIC;
					break;
				}
			}
		}
	}
}

static void
ieee80211_set_edca(struct ieee80211com *ic, int mode)
{
	int ac;

	if (mode != IEEE80211_MODE_11A && mode != IEEE80211_MODE_11B)
		mode = IEEE80211_MODE_11G;
	for (ac = 0; ac < EDCA_NUM_AC; ac++)
		ic->ic_edca_ac[ac] = edca_table[mode][ac];
}

void
ieee80211_ifattach(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;

	memcpy(((struct arpcom *)ifp)->ac_enaddr, ic->ic_myaddr,
	    ETHER_ADDR_LEN);
	if (ic->ic_send_mgmt == NULL)
		ic->ic_send_mgmt = ieee80211_send_mgmt;
	if (ic->ic_newstate == NULL)
		ic->ic_newstate = ieee80211_newstate;
	if (ic->ic_node_alloc == NULL)
		ic->ic_node_alloc = ieee80211_node_alloc_default;
	ic->ic_node_free = ieee80211_node_free_default;
	ic->ic_node_copy = ieee80211_node_copy_default;
	ic->ic_node_checkrssi = ieee80211_node_checkrssi_default;
	if (ic->ic_set_key == NULL)
		ic->ic_set_key = ieee80211_set_key;
	if (ic->ic_delete_key == NULL)
		ic->ic_delete_key = ieee80211_delete_key;

	mq_init(&ic->ic_mgtq, 64, IPL_NET);
	mq_init(&ic->ic_pwrsaveq, 64, IPL_NET);
	ic->ic_media.ifm_cur = &ic->ic_media.ifm_auto;
	ic->ic_fixed_rate = -1;
	ic->ic_fixed_mcs = -1;
	ic->ic_rtsthreshold = IEEE80211_RTS_MAX;
	ic->ic_fragthreshold = 2346;
	ic->ic_lintval = 10;
	ic->ic_bmissthres = IEEE80211_BEACON_MISS_THRES;
	ic->ic_dtim_period = 1;
	ic->ic_userflags |= IEEE80211_F_NOMIMO;
	ic->ic_rsnprotos = IEEE80211_PROTO_RSN;
	ic->ic_rsnakms = IEEE80211_AKM_PSK;
	ic->ic_rsnciphers = IEEE80211_CIPHER_CCMP;
	ic->ic_rsngroupcipher = IEEE80211_CIPHER_CCMP;
	ic->ic_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
	ic->ic_state = IEEE80211_S_INIT;

	ieee80211_channel_init(ifp);
	ieee80211_setbasicrates(ic);
	ieee80211_set_edca(ic, IEEE80211_MODE_11G);

	ic->ic_bss = ic->ic_node_alloc(ic);
	if (ic->ic_bss != NULL) {
		ieee80211_setup_node(ic, ic->ic_bss, etherbroadcastaddr);
		ic->ic_bss->ni_chan = ic->ic_ibss_chan ?
		    ic->ic_ibss_chan : IEEE80211_CHAN_ANYC;
	}
	ifp->if_link_state = LINK_STATE_DOWN;
}

void
ieee80211_channel_init(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;
	struct ieee80211_channel *c;
	int i;

	memset(ic->ic_chan_avail, 0, sizeof(ic->ic_chan_avail));
	ic->ic_modecaps |= 1 << IEEE80211_MODE_AUTO;
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_flags == 0)
			continue;
		if ((u_int)i != ieee80211_chan2ieee(ic, c)) {
			printf("%s: bad channel ignored; freq %u flags %x "
			    "number %u\n", ifp->if_xname, c->ic_freq,
			    c->ic_flags, i);
			c->ic_flags = 0;
			continue;
		}
		setbit(ic->ic_chan_avail, i);
		if (IEEE80211_IS_CHAN_A(c))
			ic->ic_modecaps |= 1 << IEEE80211_MODE_11A;
		if (IEEE80211_IS_CHAN_B(c))
			ic->ic_modecaps |= 1 << IEEE80211_MODE_11B;
		if (IEEE80211_IS_CHAN_PUREG(c))
			ic->ic_modecaps |= 1 << IEEE80211_MODE_11G;
	}
	if ((ic->ic_modecaps & (1 << ic->ic_curmode)) == 0)
		ic->ic_curmode = IEEE80211_MODE_AUTO;
	ic->ic_des_chan = IEEE80211_CHAN_ANYC;
	(void)ieee80211_setmode(ic, ic->ic_curmode);
}

u_int
ieee80211_mhz2ieee(u_int freq, u_int flags)
{
	if (flags & IEEE80211_CHAN_2GHZ) {
		if (freq == 2484)
			return 14;
		if (freq < 2484)
			return (freq - 2407) / 5;
		return 15 + ((freq - 2512) / 20);
	} else if (flags & IEEE80211_CHAN_5GHZ) {
		return (freq - 5000) / 5;
	}
	if (freq == 2484)
		return 14;
	if (freq < 2484)
		return (freq - 2407) / 5;
	if (freq < 5000)
		return 15 + ((freq - 2512) / 20);
	return (freq - 5000) / 5;
}

u_int
ieee80211_chan2ieee(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
	if (ic->ic_channels <= c && c <= &ic->ic_channels[IEEE80211_CHAN_MAX])
		return (u_int)(c - ic->ic_channels);
	return IEEE80211_CHAN_ANY;
}

u_int
ieee80211_ieee2mhz(u_int chan, u_int flags)
{
	if (flags & IEEE80211_CHAN_2GHZ) {
		if (chan == 14)
			return 2484;
		if (chan < 14)
			return 2407 + chan * 5;
		return 2512 + ((chan - 15) * 20);
	} else if (flags & IEEE80211_CHAN_5GHZ)
		return 5000 + (chan * 5);
	if (chan == 14)
		return 2484;
	if (chan < 14)
		return 2407 + chan * 5;
	if (chan < 27)
		return 2512 + ((chan - 15) * 20);
	return 5000 + (chan * 5);
}

static void
ieee80211_reset_scan(struct ieee80211com *ic)
{
	memcpy(ic->ic_chan_scan, ic->ic_chan_active, sizeof(ic->ic_chan_active));
}

int
ieee80211_setmode(struct ieee80211com *ic, enum ieee80211_phymode mode)
{
	static const u_int16_t chanflags[] = {
		0, IEEE80211_CHAN_A, IEEE80211_CHAN_B, IEEE80211_CHAN_PUREG,
		IEEE80211_CHAN_HT, IEEE80211_CHAN_VHT, 0
	};
	const struct ieee80211_channel *c;
	u_int16_t modeflags;
	int i;

	if ((ic->ic_modecaps & (1 << mode)) == 0)
		return EINVAL;
	modeflags = chanflags[mode];
	memset(ic->ic_chan_active, 0, sizeof(ic->ic_chan_active));
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_flags == 0)
			continue;
		if (mode == IEEE80211_MODE_AUTO ||
		    (c->ic_flags & modeflags) == modeflags)
			setbit(ic->ic_chan_active, i);
	}
	if (ic->ic_ibss_chan == NULL || isclr(ic->ic_chan_active,
	    ieee80211_chan2ieee(ic, ic->ic_ibss_chan))) {
		for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
			if (isset(ic->ic_chan_active, i)) {
				ic->ic_ibss_chan = &ic->ic_channels[i];
				break;
			}
		}
	}
	ieee80211_reset_scan(ic);
	ic->ic_curmode = mode;
	ieee80211_set_edca(ic, mode);
	if (ic->ic_ibss_chan != NULL)
		ieee80211_reset_erp(ic);
	return 0;
}

int
ieee80211_min_basic_rate(struct ieee80211com *ic)
{
	struct ieee80211_rateset *rs = &ic->ic_bss->ni_rates;
	int i, min = -1, rval;

	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_BASIC) == 0)
			continue;
		rval = rs->rs_rates[i] & IEEE80211_RATE_VAL;
		if (min == -1 || rval < min)
			min = rval;
	}
	if (min == -1) {
		struct ieee80211_channel *c = ic->ic_bss->ni_chan;
		min = (c != IEEE80211_CHAN_ANYC && IEEE80211_IS_CHAN_5GHZ(c)) ?
		    12 : 2;
	}
	return min;
}

int
ieee80211_max_basic_rate(struct ieee80211com *ic)
{
	struct ieee80211_rateset *rs = &ic->ic_bss->ni_rates;
	struct ieee80211_channel *c = ic->ic_bss->ni_chan;
	int i, max, rval;

	max = (c != IEEE80211_CHAN_ANYC && IEEE80211_IS_CHAN_5GHZ(c)) ? 12 : 2;
	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_BASIC) == 0)
			continue;
		rval = rs->rs_rates[i] & IEEE80211_RATE_VAL;
		if (rval > max)
			max = rval;
	}
	return max;
}

enum ieee80211_phymode
ieee80211_node_abg_mode(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	(void)ic;
	if (ni->ni_chan == IEEE80211_CHAN_ANYC || ni->ni_chan == NULL)
		return IEEE80211_MODE_11G;
	if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan))
		return IEEE80211_MODE_11A;
	if ((ni->ni_flags & IEEE80211_NODE_ERP) &&
	    (ni->ni_chan->ic_flags &
	    (IEEE80211_CHAN_OFDM | IEEE80211_CHAN_DYN)) != 0)
		return IEEE80211_MODE_11G;
	return IEEE80211_MODE_11B;
}

int
ieee80211_fix_rate(struct ieee80211com *ic, struct ieee80211_node *ni,
    int flags)
{
#define	RV(v)	((v) & IEEE80211_RATE_VAL)
	int i, j, ignore, okrate = 0, badrate = 0, fixedrate = 0;
	const struct ieee80211_rateset *srs;
	struct ieee80211_rateset *nrs;
	u_int8_t r;

	if ((flags & IEEE80211_F_DOFRATE) && ic->ic_fixed_rate == -1)
		flags &= ~IEEE80211_F_DOFRATE;
	srs = &ic->ic_sup_rates[ieee80211_node_abg_mode(ic, ni)];
	nrs = &ni->ni_rates;
	for (i = 0; i < nrs->rs_nrates; ) {
		ignore = 0;
		if (flags & IEEE80211_F_DOSORT) {
			for (j = i + 1; j < nrs->rs_nrates; j++) {
				if (RV(nrs->rs_rates[i]) > RV(nrs->rs_rates[j])) {
					r = nrs->rs_rates[i];
					nrs->rs_rates[i] = nrs->rs_rates[j];
					nrs->rs_rates[j] = r;
				}
			}
		}
		r = nrs->rs_rates[i] & IEEE80211_RATE_VAL;
		badrate = r;
		if (flags & IEEE80211_F_DOFRATE) {
			if (r == RV(srs->rs_rates[ic->ic_fixed_rate]))
				fixedrate = r;
		}
		if (flags & IEEE80211_F_DONEGO) {
			for (j = 0; j < srs->rs_nrates; j++) {
				if (r == RV(srs->rs_rates[j])) {
					nrs->rs_rates[i] = srs->rs_rates[j];
					break;
				}
			}
			if (j == srs->rs_nrates)
				ignore++;
		}
		if ((flags & IEEE80211_F_DODEL) && ignore) {
			nrs->rs_nrates--;
			for (j = i; j < nrs->rs_nrates; j++)
				nrs->rs_rates[j] = nrs->rs_rates[j + 1];
			nrs->rs_rates[j] = 0;
			continue;
		}
		if (!ignore)
			okrate = nrs->rs_rates[i];
		i++;
	}
	if (okrate == 0 ||
	    ((flags & IEEE80211_F_DOFRATE) && fixedrate == 0))
		return badrate | IEEE80211_RATE_BASIC;
	return RV(okrate);
#undef RV
}

static void
ieee80211_set_shortslottime(struct ieee80211com *ic, int on)
{
	if (on)
		ic->ic_flags |= IEEE80211_F_SHSLOT;
	else
		ic->ic_flags &= ~IEEE80211_F_SHSLOT;
	if (ic->ic_updateslot != NULL && ic->ic_state == IEEE80211_S_RUN)
		ic->ic_updateslot(ic);
}

static void
ieee80211_reset_erp(struct ieee80211com *ic)
{
	ic->ic_flags &= ~IEEE80211_F_USEPROT;
	ieee80211_set_shortslottime(ic, ic->ic_curmode == IEEE80211_MODE_11A);
	if (ic->ic_curmode == IEEE80211_MODE_11A ||
	    (ic->ic_caps & IEEE80211_C_SHPREAMBLE))
		ic->ic_flags |= IEEE80211_F_SHPREAMBLE;
	else
		ic->ic_flags &= ~IEEE80211_F_SHPREAMBLE;
}

static int
ieee80211_node_is_11g(struct ieee80211_node *ni)
{
	const struct ieee80211_rateset *ofdm = &ieee80211_std_rateset_11a;
	int i, j;

	if (ni->ni_chan == IEEE80211_CHAN_ANYC ||
	    !IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
		return 0;
	for (i = 0; i < ni->ni_rates.rs_nrates; i++)
		for (j = 0; j < ofdm->rs_nrates; j++)
			if ((ni->ni_rates.rs_rates[i] & IEEE80211_RATE_VAL) ==
			    ofdm->rs_rates[j])
				return 1;
	return 0;
}

static int
ieee80211_setup_rates(struct ieee80211com *ic, struct ieee80211_node *ni,
    const u_int8_t *rates, const u_int8_t *xrates, int flags)
{
	struct ieee80211_rateset *rs = &ni->ni_rates;

	memset(rs, 0, sizeof(*rs));
	rs->rs_nrates = rates[1];
	memcpy(rs->rs_rates, rates + 2, rs->rs_nrates);
	if (xrates != NULL) {
		u_int8_t nx = xrates[1];
		if (rs->rs_nrates + nx > IEEE80211_RATE_MAXSIZE) {
			nx = IEEE80211_RATE_MAXSIZE - rs->rs_nrates;
			ic->ic_stats.is_rx_rstoobig++;
		}
		memcpy(rs->rs_rates + rs->rs_nrates, xrates + 2, nx);
		rs->rs_nrates += nx;
	}
	if (ieee80211_node_is_11g(ni))
		ni->ni_flags |= IEEE80211_NODE_ERP;
	return ieee80211_fix_rate(ic, ni, flags);
}

void
ieee80211_media_init(struct ifnet *ifp, int (*change)(struct ifnet *),
    void (*status)(struct ifnet *, struct ifmediareq *))
{
	(void)ifp; (void)change; (void)status;
}

void
ieee80211_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	(void)ifp; (void)imr;
}

void
ieee80211_watchdog(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;

	if (ic->ic_mgt_timer && --ic->ic_mgt_timer == 0) {
		if (ic->ic_state == IEEE80211_S_AUTH ||
		    ic->ic_state == IEEE80211_S_ASSOC) {
			struct ieee80211_node *ni;

			printf("wlan: %s timed out for %s\n",
			    ic->ic_state == IEEE80211_S_ASSOC ?
			    "association" : "authentication",
			    ether_sprintf(ic->ic_bss->ni_macaddr));
			set_error(ic->ic_state == IEEE80211_S_ASSOC ?
			    "association timed out" :
			    "authentication timed out");
			ni = ieee80211_find_node(ic, ic->ic_bss->ni_macaddr);
			if (ni)
				ni->ni_fails++;
		}
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	}
	if (ic->ic_mgt_timer != 0)
		ifp->if_timer = 1;
}

/* ==== rate adaptation ================================================== */

/*
 * AMRR (Lacage, Manshaei, Turletti, "IEEE 802.11 Rate Adaptation: A
 * Practical Approach", 2004) over the legacy rate set.
 */
#define amrr_success(amn)	((amn)->amn_retrycnt < (amn)->amn_txcnt / 10)
#define amrr_failure(amn)	((amn)->amn_retrycnt > (amn)->amn_txcnt / 3)
#define amrr_enough(amn)	((amn)->amn_txcnt > 10)

void
ieee80211_amrr_node_init(const struct ieee80211_amrr *amrr,
    struct ieee80211_amrr_node *amn)
{
	amn->amn_success = 0;
	amn->amn_recovery = 0;
	amn->amn_txcnt = amn->amn_retrycnt = 0;
	amn->amn_success_threshold = amrr->amrr_min_success_threshold;
}

void
ieee80211_amrr_choose(struct ieee80211_amrr *amrr, struct ieee80211_node *ni,
    struct ieee80211_amrr_node *amn)
{
	int changed = 0;

	if (amrr_success(amn) && amrr_enough(amn)) {
		amn->amn_success++;
		if ((u_int)amn->amn_success >= amn->amn_success_threshold &&
		    ni->ni_txrate + 1 < ni->ni_rates.rs_nrates) {
			amn->amn_recovery = 1;
			amn->amn_success = 0;
			ni->ni_txrate++;
			changed = 1;
		} else
			amn->amn_recovery = 0;
	} else if (amrr_failure(amn)) {
		amn->amn_success = 0;
		if (ni->ni_txrate > 0) {
			if (amn->amn_recovery) {
				amn->amn_success_threshold *= 2;
				if (amn->amn_success_threshold >
				    amrr->amrr_max_success_threshold)
					amn->amn_success_threshold =
					    amrr->amrr_max_success_threshold;
			} else
				amn->amn_success_threshold =
				    amrr->amrr_min_success_threshold;
			ni->ni_txrate--;
			changed = 1;
		}
		amn->amn_recovery = 0;
	}
	if (amrr_enough(amn) || changed)
		amn->amn_txcnt = amn->amn_retrycnt = 0;
}

const struct ieee80211_ht_rateset *
ieee80211_ra_get_ht_rateset(int mcs, int chan40, int sgi)
{
	static const struct ieee80211_ht_rateset rs = {
		8, { 13, 26, 39, 52, 78, 104, 117, 130 }, 0xff, 0, 7, 0, 0
	};
	(void)mcs; (void)chan40; (void)sgi;
	return &rs;
}

/* ==== nodes ============================================================ */

struct ieee80211_node *
ieee80211_alloc_node(struct ieee80211com *ic, const u_int8_t *macaddr)
{
	struct ieee80211_node *ni;
	int i, slot = -1, worst = -1;

	for (i = 0; i < IEEE80211_NODE_MAX; i++) {
		if (ic->ic_nodes[i] == NULL) {
			slot = i;
			break;
		}
		/* table full: recycle the stalest unreferenced entry */
		if (ic->ic_nodes[i]->ni_refcnt == 0 && (worst < 0 ||
		    ic->ic_nodes[i]->ni_inact > ic->ic_nodes[worst]->ni_inact))
			worst = i;
	}
	if (slot < 0) {
		if (worst < 0) {
			ic->ic_stats.is_rx_nodealloc++;
			return NULL;
		}
		ic->ic_node_free(ic, ic->ic_nodes[worst]);
		ic->ic_nodes[worst] = NULL;
		ic->ic_nnodes--;
		slot = worst;
	}
	ni = ic->ic_node_alloc(ic);
	if (ni == NULL) {
		ic->ic_stats.is_rx_nodealloc++;
		return NULL;
	}
	ieee80211_setup_node(ic, ni, macaddr);
	ic->ic_nodes[slot] = ni;
	ic->ic_nnodes++;
	return ni;
}

struct ieee80211_node *
ieee80211_find_node(struct ieee80211com *ic, const u_int8_t *macaddr)
{
	int i;

	for (i = 0; i < IEEE80211_NODE_MAX; i++)
		if (ic->ic_nodes[i] != NULL &&
		    IEEE80211_ADDR_EQ(ic->ic_nodes[i]->ni_macaddr, macaddr))
			return ic->ic_nodes[i];
	return NULL;
}

/* Station mode: every frame is accounted to our BSS. */
struct ieee80211_node *
ieee80211_find_rxnode(struct ieee80211com *ic, const struct ieee80211_frame *wh)
{
	(void)wh;
	return ieee80211_ref_node(ic->ic_bss);
}

struct ieee80211_node *
ieee80211_find_txnode(struct ieee80211com *ic, const u_int8_t *macaddr)
{
	(void)macaddr;
	return ieee80211_ref_node(ic->ic_bss);
}

void
ieee80211_release_node(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	(void)ic;
	if (ni != NULL && ni->ni_refcnt > 0)
		ni->ni_refcnt--;
}

static void
ieee80211_free_node(struct ieee80211com *ic, int slot)
{
	struct ieee80211_node *ni = ic->ic_nodes[slot];

	ic->ic_nodes[slot] = NULL;
	ic->ic_nnodes--;
	ic->ic_node_free(ic, ni);
}

void
ieee80211_free_allnodes(struct ieee80211com *ic, int clear_ic_bss)
{
	int i;

	for (i = 0; i < IEEE80211_NODE_MAX; i++)
		if (ic->ic_nodes[i] != NULL)
			ieee80211_free_node(ic, i);
	if (clear_ic_bss && ic->ic_bss != NULL)
		ieee80211_node_cleanup(ic, ic->ic_bss);
}

void
ieee80211_node_cleanup(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	(void)ic;
	if (ni->ni_rsnie != NULL) {
		free(ni->ni_rsnie, M_DEVBUF, 2 + ni->ni_rsnie[1]);
		ni->ni_rsnie = NULL;
	}
	ieee80211_ba_del(ni);
}

void
ieee80211_ba_del(struct ieee80211_node *ni)
{
	int tid;

	for (tid = 0; tid < IEEE80211_NUM_TID; tid++) {
		ni->ni_rx_ba[tid].ba_state = IEEE80211_BA_INIT;
		ni->ni_tx_ba[tid].ba_state = IEEE80211_BA_INIT;
	}
}

static void
ieee80211_node_newstate(struct ieee80211_node *ni, int state)
{
	ni->ni_state = state;
}

/* Age the scan table; called at the start of each scan round. */
static void
ieee80211_age_nodes(struct ieee80211com *ic)
{
	int i;

	for (i = 0; i < IEEE80211_NODE_MAX; i++) {
		struct ieee80211_node *ni = ic->ic_nodes[i];
		if (ni == NULL || ni->ni_refcnt > 0)
			continue;
		if (++ni->ni_inact > IEEE80211_INACT_SCAN / 2)
			ieee80211_free_node(ic, i);
	}
}

/* ==== state machine ==================================================== */

int
ieee80211_new_state(struct ieee80211com *ic, enum ieee80211_state nstate,
    int arg)
{
	return ic->ic_newstate(ic, nstate, arg);
}

void
ieee80211_set_link_state(struct ieee80211com *ic, int nstate)
{
	struct ifnet *ifp = &ic->ic_if;

	if (nstate != ifp->if_link_state) {
		ifp->if_link_state = nstate;
		if_link_state_change(ifp);
	}
}

static void
ieee80211_set_beacon_miss_threshold(struct ieee80211com *ic)
{
	int intval = ic->ic_bss->ni_intval;
	int btimeout = MIN(IEEE80211_BEACON_MISS_THRES * intval,
	    IEEE80211_BEACON_MISS_THRES * (IEEE80211_DUR_TU / 10));

	btimeout = MAX(btimeout, 2 * intval);
	if (intval > 0)
		ic->ic_bmissthres = (u_int16_t)(btimeout / intval);
}

/*
 * Station-mode subset of OpenBSD's ieee80211_newstate(): the driver calls
 * this after it has done its own part of the transition.
 */
int
ieee80211_newstate(struct ieee80211com *ic, enum ieee80211_state nstate,
    int mgt)
{
	struct ieee80211_node *ni = ic->ic_bss;
	enum ieee80211_state ostate = ic->ic_state;

	printf("wlan: %s -> %s\n", ieee80211_state_name[ostate],
	    ieee80211_state_name[nstate]);
	ic->ic_state = nstate;
	ieee80211_set_link_state(ic, LINK_STATE_DOWN);
	ic->ic_xflags &= ~IEEE80211_F_TX_MGMT_ONLY;

	switch (nstate) {
	case IEEE80211_S_INIT:
		switch (ostate) {
		case IEEE80211_S_RUN:
			if (mgt != -1)
				IEEE80211_SEND_MGMT(ic, ni,
				    IEEE80211_FC0_SUBTYPE_DISASSOC,
				    IEEE80211_REASON_ASSOC_LEAVE);
			/* FALLTHROUGH */
		case IEEE80211_S_ASSOC:
			if (mgt != -1 && ostate != IEEE80211_S_INIT)
				IEEE80211_SEND_MGMT(ic, ni,
				    IEEE80211_FC0_SUBTYPE_DEAUTH,
				    IEEE80211_REASON_AUTH_LEAVE);
			/* FALLTHROUGH */
		case IEEE80211_S_AUTH:
		case IEEE80211_S_SCAN:
			ieee80211_ba_del(ni);
			ic->ic_mgt_timer = 0;
			if (mgt == -1)
				mq_purge(&ic->ic_mgtq);
			break;
		case IEEE80211_S_INIT:
			break;
		}
		ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
		wpa_sm.state = WPA_SUPP_INITIALIZE;
		ni->ni_assoc_fail = 0;
		ieee80211_crypto_clear_groupkeys(ic);
		break;
	case IEEE80211_S_SCAN:
		IEEE80211_ADDR_COPY(ni->ni_macaddr, etherbroadcastaddr);
		IEEE80211_ADDR_COPY(ni->ni_bssid, etherbroadcastaddr);
		ni->ni_rates = ic->ic_sup_rates[ieee80211_node_abg_mode(ic, ni)];
		ni->ni_associd = 0;
		ni->ni_rstamp = 0;
		ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
		wpa_sm.state = WPA_SUPP_INITIALIZE;
		ieee80211_crypto_clear_groupkeys(ic);
		break;
	case IEEE80211_S_AUTH:
		ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
		wpa_sm.state = WPA_SUPP_INITIALIZE;
		ieee80211_crypto_clear_groupkeys(ic);
		switch (ostate) {
		case IEEE80211_S_SCAN:
			IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_AUTH,
			    IEEE80211_AUTH_OPEN_REQUEST);
			break;
		case IEEE80211_S_AUTH:
		case IEEE80211_S_ASSOC:
			if (mgt == IEEE80211_FC0_SUBTYPE_AUTH)
				IEEE80211_SEND_MGMT(ic, ni,
				    IEEE80211_FC0_SUBTYPE_AUTH,
				    IEEE80211_AUTH_OPEN_REQUEST);
			break;
		case IEEE80211_S_RUN:
			ieee80211_ba_del(ni);
			if (mgt == IEEE80211_FC0_SUBTYPE_DEAUTH)
				IEEE80211_SEND_MGMT(ic, ni,
				    IEEE80211_FC0_SUBTYPE_AUTH,
				    IEEE80211_AUTH_OPEN_REQUEST);
			break;
		default:
			printf("wlan: invalid transition %s -> %s\n",
			    ieee80211_state_name[ostate],
			    ieee80211_state_name[nstate]);
			break;
		}
		break;
	case IEEE80211_S_ASSOC:
		switch (ostate) {
		case IEEE80211_S_AUTH:
			IEEE80211_SEND_MGMT(ic, ni,
			    IEEE80211_FC0_SUBTYPE_ASSOC_REQ, 0);
			break;
		case IEEE80211_S_RUN:
			ieee80211_ba_del(ni);
			IEEE80211_SEND_MGMT(ic, ni,
			    IEEE80211_FC0_SUBTYPE_ASSOC_REQ, 1);
			break;
		default:
			printf("wlan: invalid transition %s -> %s\n",
			    ieee80211_state_name[ostate],
			    ieee80211_state_name[nstate]);
			break;
		}
		break;
	case IEEE80211_S_RUN:
		if (ostate != IEEE80211_S_ASSOC) {
			printf("wlan: invalid transition %s -> %s\n",
			    ieee80211_state_name[ostate],
			    ieee80211_state_name[nstate]);
			break;
		}
		if (ni->ni_txrate >= ni->ni_rates.rs_nrates)
			ni->ni_txrate = 0;
		printf("wlan: associated with %s ssid \"%.*s\" channel %u, "
		    "%s preamble, %s slot%s\n", ether_sprintf(ni->ni_bssid),
		    ni->ni_esslen, (const char *)ni->ni_essid,
		    ieee80211_chan2ieee(ic, ni->ni_chan),
		    (ic->ic_flags & IEEE80211_F_SHPREAMBLE) ? "short" : "long",
		    (ic->ic_flags & IEEE80211_F_SHSLOT) ? "short" : "long",
		    (ic->ic_flags & IEEE80211_F_USEPROT) ? ", protection" : "");
		if (!(ic->ic_flags & IEEE80211_F_RSNON)) {
			/* Open network: no handshake, link is up now. */
			ni->ni_port_valid = 1;
			ieee80211_set_link_state(ic, LINK_STATE_UP);
			ni->ni_assoc_fail = 0;
		}
		ic->ic_mgt_timer = 0;
		ieee80211_set_beacon_miss_threshold(ic);
		if_start(&ic->ic_if);
		break;
	}
	return 0;
}

static void
ieee80211_choose_rsnparams(struct ieee80211com *ic)
{
	struct ieee80211_node *ni = ic->ic_bss;

	ni->ni_rsnprotos = IEEE80211_PROTO_RSN;
	ni->ni_rsnakms = IEEE80211_AKM_PSK;
	ni->ni_rsnciphers = IEEE80211_CIPHER_CCMP;
	ni->ni_rsncipher = IEEE80211_CIPHER_CCMP;
	/* MFP is not supported; never set IEEE80211_NODE_MFP */
}

static int
ieee80211_match_bss(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	u_int8_t rate;
	int fail = 0;

	if (ni->ni_chan == IEEE80211_CHAN_ANYC ||
	    isclr(ic->ic_chan_active, ieee80211_chan2ieee(ic, ni->ni_chan)))
		fail |= IEEE80211_NODE_ASSOCFAIL_CHAN;
	if ((ni->ni_capinfo & IEEE80211_CAPINFO_ESS) == 0)
		fail |= IEEE80211_NODE_ASSOCFAIL_IBSS;
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		if ((ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) == 0)
			fail |= IEEE80211_NODE_ASSOCFAIL_PRIVACY;
	} else if (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY)
		fail |= IEEE80211_NODE_ASSOCFAIL_PRIVACY;
	rate = (u_int8_t)ieee80211_fix_rate(ic, ni, IEEE80211_F_DONEGO);
	if (rate & IEEE80211_RATE_BASIC)
		fail |= IEEE80211_NODE_ASSOCFAIL_BASIC_RATE;
	if (ic->ic_des_esslen == 0 || ni->ni_esslen != ic->ic_des_esslen ||
	    memcmp(ni->ni_essid, ic->ic_des_essid, ic->ic_des_esslen) != 0)
		fail |= IEEE80211_NODE_ASSOCFAIL_ESSID;
	if (ni->ni_flags & IEEE80211_NODE_CSA)
		fail |= IEEE80211_NODE_ASSOCFAIL_CSA;
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		/* WPA2-PSK with CCMP, without required MFP */
		if ((ni->ni_rsnprotos & IEEE80211_PROTO_RSN) == 0 ||
		    (ni->ni_rsnakms & IEEE80211_AKM_PSK) == 0 ||
		    (ni->ni_rsnciphers & IEEE80211_CIPHER_CCMP) == 0 ||
		    ni->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP ||
		    (ni->ni_rsncaps & IEEE80211_RSNCAP_MFPR))
			fail |= IEEE80211_NODE_ASSOCFAIL_WPA_PROTO;
	}
	ni->ni_assoc_fail = (u_int32_t)fail;
	return fail;
}

static struct ieee80211_node *
ieee80211_node_choose_bss(struct ieee80211com *ic)
{
	struct ieee80211_node *ni, *selbs = NULL;
	int i;

	for (i = 0; i < IEEE80211_NODE_MAX; i++) {
		ni = ic->ic_nodes[i];
		if (ni == NULL)
			continue;
		if (ni->ni_fails) {
			/* Give an AP that failed us a few rounds of rest. */
			if (ni->ni_fails++ > 2)
				ni->ni_fails = 0;
			continue;
		}
		if (ieee80211_match_bss(ic, ni) != 0)
			continue;
		/* Prefer 5 GHz unless it is much weaker. */
		if (selbs == NULL)
			selbs = ni;
		else if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) &&
		    IEEE80211_IS_CHAN_2GHZ(selbs->ni_chan)) {
			if (ni->ni_rssi + 10 >= selbs->ni_rssi)
				selbs = ni;
		} else if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan) &&
		    IEEE80211_IS_CHAN_5GHZ(selbs->ni_chan)) {
			if (ni->ni_rssi > selbs->ni_rssi + 10)
				selbs = ni;
		} else if (ni->ni_rssi > selbs->ni_rssi)
			selbs = ni;
	}
	return selbs;
}

static void
ieee80211_node_join_bss(struct ieee80211com *ic, struct ieee80211_node *selbs)
{
	enum ieee80211_phymode mode;
	struct ieee80211_node *ni;
	int auth_next = (ic->ic_state == IEEE80211_S_AUTH);

	mode = ieee80211_node_abg_mode(ic, selbs);
	if (mode != ic->ic_curmode)
		ieee80211_setmode(ic, mode);

	ic->ic_node_copy(ic, ic->ic_bss, selbs);
	ni = ic->ic_bss;
	ni->ni_refcnt = 0;
	ieee80211_fix_rate(ic, ni, IEEE80211_F_DOSORT | IEEE80211_F_DOFRATE |
	    IEEE80211_F_DONEGO | IEEE80211_F_DODEL);
	if (ic->ic_flags & IEEE80211_F_RSNON)
		ieee80211_choose_rsnparams(ic);
	ieee80211_node_newstate(selbs, IEEE80211_STA_BSS);

	printf("wlan: joining %s ssid \"%.*s\" channel %u rssi %u\n",
	    ether_sprintf(ni->ni_bssid), ni->ni_esslen,
	    (const char *)ni->ni_essid, ieee80211_chan2ieee(ic, ni->ni_chan),
	    ni->ni_rssi);
	ieee80211_new_state(ic, IEEE80211_S_AUTH,
	    auth_next ? IEEE80211_FC0_SUBTYPE_AUTH : -1);
}

void
ieee80211_begin_scan(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;

	ic->ic_flags |= IEEE80211_F_ASCAN;
	ic->ic_stats.is_scan_active++;
	ieee80211_node_cleanup(ic, ic->ic_bss);
	ieee80211_age_nodes(ic);
	ic->ic_curmode = IEEE80211_MODE_AUTO;
	ieee80211_setmode(ic, ic->ic_curmode);
	ic->ic_scan_count = 0;
	ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
}

/*
 * The firmware scanned every channel.  Join the configured network if it
 * was seen; otherwise start another round (unless only a scan was asked
 * for, in which case the results stay in the table for the UI).
 */
void
ieee80211_end_scan(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;
	struct ieee80211_node *selbs;

	ic->ic_scan_gen++;
	ic->ic_scan_count++;
	printf("wlan: scan round %u done, %d networks in table\n",
	    ic->ic_scan_gen, ic->ic_nnodes);

	if (ic->ic_scan_only || ic->ic_des_esslen == 0)
		return;

	selbs = ieee80211_node_choose_bss(ic);
	if (selbs == NULL) {
		if (ic->ic_scan_count == 1 || ic->ic_scan_count % 10 == 0)
			set_error("network not found yet, still scanning");
		ieee80211_age_nodes(ic);
		ieee80211_reset_scan(ic);
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		return;
	}
	ieee80211_node_join_bss(ic, selbs);
}

void
ieee80211_icda_configure(struct ieee80211com *ic, const char *ssid,
    size_t ssidlen, const u_int8_t *pmk)
{
	if (ssidlen > IEEE80211_NWID_LEN)
		ssidlen = IEEE80211_NWID_LEN;
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	memcpy(ic->ic_des_essid, ssid, ssidlen);
	ic->ic_des_esslen = (u_int8_t)ssidlen;
	if (pmk != NULL) {
		memcpy(ic->ic_psk, pmk, IEEE80211_PMK_LEN);
		ic->ic_flags |= IEEE80211_F_RSNON | IEEE80211_F_PSK;
	} else {
		explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
		ic->ic_flags &= ~(IEEE80211_F_RSNON | IEEE80211_F_PSK);
	}
	ic->ic_scan_only = (ssidlen == 0);
	ieee80211_icda_last_error[0] = '\0';
}

/* ==== input ============================================================ */

u_int
ieee80211_get_hdrlen(const struct ieee80211_frame *wh)
{
	u_int size = sizeof(*wh);

	if (ieee80211_has_addr4(wh))
		size += IEEE80211_ADDR_LEN;
	if (ieee80211_has_qos(wh))
		size += sizeof(u_int16_t);
	if (ieee80211_has_htc(wh))
		size += sizeof(u_int32_t);
	return size;
}

static enum ieee80211_cipher
ieee80211_parse_rsn_cipher(const u_int8_t selector[4])
{
	static const u_int8_t oui[3] = { 0x00, 0x0f, 0xac };

	if (memcmp(selector, oui, 3) == 0) {
		switch (selector[3]) {
		case 0: return IEEE80211_CIPHER_USEGROUP;
		case 1: return IEEE80211_CIPHER_WEP40;
		case 2: return IEEE80211_CIPHER_TKIP;
		case 4: return IEEE80211_CIPHER_CCMP;
		case 5: return IEEE80211_CIPHER_WEP104;
		case 6: return IEEE80211_CIPHER_BIP;
		}
	}
	return IEEE80211_CIPHER_NONE;
}

static enum ieee80211_akm
ieee80211_parse_rsn_akm(const u_int8_t selector[4])
{
	static const u_int8_t oui[3] = { 0x00, 0x0f, 0xac };

	if (memcmp(selector, oui, 3) == 0) {
		switch (selector[3]) {
		case 1: return IEEE80211_AKM_8021X;
		case 2: return IEEE80211_AKM_PSK;
		case 5: return IEEE80211_AKM_SHA256_8021X;
		case 6: return IEEE80211_AKM_SHA256_PSK;
		case 8: return IEEE80211_AKM_SAE;
		}
	}
	return IEEE80211_AKM_NONE;
}

/* RSN element body (802.11-2012 8.4.2.27); 0 on success. */
static int
ieee80211_parse_rsn(const u_int8_t *ie, struct ieee80211_rsnparams *rsn)
{
	const u_int8_t *frm = ie + 2, *efrm = ie + 2 + ie[1];
	u_int16_t m, n;

	if (ie[1] < 2 || LE_READ_2(frm) != 1)
		return -1;
	frm += 2;
	rsn->rsn_groupcipher = IEEE80211_CIPHER_CCMP;
	rsn->rsn_nciphers = 1;
	rsn->rsn_ciphers = IEEE80211_CIPHER_CCMP;
	rsn->rsn_groupmgmtcipher = IEEE80211_CIPHER_BIP;
	rsn->rsn_nakms = 1;
	rsn->rsn_akms = IEEE80211_AKM_8021X;
	rsn->rsn_caps = 0;
	rsn->rsn_npmkids = 0;

	if (frm + 4 > efrm)
		return 0;
	rsn->rsn_groupcipher = ieee80211_parse_rsn_cipher(frm);
	frm += 4;
	if (frm + 2 > efrm)
		return 0;
	m = rsn->rsn_nciphers = LE_READ_2(frm);
	frm += 2;
	if (frm + m * 4 > efrm)
		return -1;
	rsn->rsn_ciphers = IEEE80211_CIPHER_NONE;
	while (m-- > 0) {
		rsn->rsn_ciphers |= ieee80211_parse_rsn_cipher(frm);
		frm += 4;
	}
	if (frm + 2 > efrm)
		return 0;
	n = rsn->rsn_nakms = LE_READ_2(frm);
	frm += 2;
	if (frm + n * 4 > efrm)
		return -1;
	rsn->rsn_akms = IEEE80211_AKM_NONE;
	while (n-- > 0) {
		rsn->rsn_akms |= ieee80211_parse_rsn_akm(frm);
		frm += 4;
	}
	if (frm + 2 > efrm)
		return 0;
	rsn->rsn_caps = LE_READ_2(frm);
	return 0;
}

static int
ieee80211_save_ie(const u_int8_t *frm, u_int8_t **ie)
{
	int olen = *ie ? 2 + (*ie)[1] : 0;
	int len = 2 + frm[1];

	if (*ie == NULL || olen != len) {
		if (*ie != NULL)
			free(*ie, M_DEVBUF, olen);
		*ie = malloc(len, M_DEVBUF, 0);
		if (*ie == NULL)
			return ENOMEM;
	}
	memcpy(*ie, frm, len);
	return 0;
}

/* Beacon and probe response: maintain the scan table. */
static void
ieee80211_recv_probe_resp(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_rxinfo *rxi, int isprobe)
{
	struct ieee80211_node *ni;
	const struct ieee80211_frame *wh;
	const u_int8_t *frm, *efrm, *ssid, *rates, *xrates, *rsnie, *tim, *csa;
	const u_int8_t *edcaie, *wmmie;
	u_int16_t capinfo, bintval;
	u_int8_t chan, bchan, erp;

	if (m->m_len < (int)sizeof(*wh) + 12)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	frm = (const u_int8_t *)&wh[1];
	efrm = mtod(m, u_int8_t *) + m->m_len;
	frm += 8;	/* timestamp */
	bintval = LE_READ_2(frm); frm += 2;
	capinfo = LE_READ_2(frm); frm += 2;

	ssid = rates = xrates = rsnie = tim = csa = edcaie = wmmie = NULL;
	bchan = rxi->rxi_chan ? rxi->rxi_chan :
	    (u_int8_t)ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
	chan = bchan;
	erp = 0;
	while (frm + 2 <= efrm) {
		if (frm + 2 + frm[1] > efrm) {
			ic->ic_stats.is_rx_elem_toosmall++;
			break;
		}
		switch (frm[0]) {
		case IEEE80211_ELEMID_SSID:
			ssid = frm;
			break;
		case IEEE80211_ELEMID_RATES:
			rates = frm;
			break;
		case IEEE80211_ELEMID_DSPARMS:
			if (frm[1] >= 1)
				chan = frm[2];
			break;
		case IEEE80211_ELEMID_XRATES:
			xrates = frm;
			break;
		case IEEE80211_ELEMID_ERP:
			if (frm[1] >= 1)
				erp = frm[2];
			break;
		case IEEE80211_ELEMID_CSA:
		case IEEE80211_ELEMID_XCSA:
			csa = frm;
			break;
		case IEEE80211_ELEMID_RSN:
			rsnie = frm;
			break;
		case IEEE80211_ELEMID_EDCAPARMS:
			edcaie = frm;
			break;
		case IEEE80211_ELEMID_HTOP:
			if (frm[1] >= 22)
				chan = frm[2];
			break;
		case IEEE80211_ELEMID_TIM:
			if (frm[1] >= 4)
				tim = frm;
			break;
		case IEEE80211_ELEMID_VENDOR:
			if (frm[1] >= 5 && memcmp(frm + 2, MICROSOFT_OUI, 3) == 0 &&
			    frm[5] == 2 && frm[6] == 1)
				wmmie = frm;
			break;
		}
		frm += 2 + frm[1];
	}
	if (rates == NULL || rates[1] > IEEE80211_RATE_MAXSIZE)
		return;
	if (ssid == NULL || ssid[1] > IEEE80211_NWID_LEN)
		return;
	if (isclr(ic->ic_chan_avail, chan)) {	/* u8: always <= CHAN_MAX */
		ic->ic_stats.is_rx_badchan++;
		return;
	}
	if (rxi->rxi_chan != 0 && chan != rxi->rxi_chan) {
		ic->ic_stats.is_rx_chanmismatch++;
		return;
	}

	if ((ni = ieee80211_find_node(ic, wh->i_addr2)) == NULL) {
		ni = ieee80211_alloc_node(ic, wh->i_addr2);
		if (ni == NULL)
			return;
	}
	ni->ni_chan = &ic->ic_channels[chan];
	ni->ni_inact = 0;
	ni->ni_flags &= ~IEEE80211_NODE_CSA;
	if (csa != NULL)
		ni->ni_flags |= IEEE80211_NODE_CSA;
	if (tim) {
		ni->ni_dtimcount = tim[2];
		ni->ni_dtimperiod = tim[3];
	}

	/* While associated, follow ERP/slot changes of our AP. */
	if (ic->ic_state == IEEE80211_S_RUN &&
	    IEEE80211_ADDR_EQ(wh->i_addr2, ic->ic_bss->ni_macaddr)) {
		struct ieee80211_node *bss = ic->ic_bss;

		if (bss->ni_erp != erp) {
			if (ic->ic_curmode == IEEE80211_MODE_11G &&
			    (erp & IEEE80211_ERP_USE_PROTECTION))
				ic->ic_flags |= IEEE80211_F_USEPROT;
			else
				ic->ic_flags &= ~IEEE80211_F_USEPROT;
			bss->ni_erp = erp;
			if (ic->ic_updateprot != NULL)
				ic->ic_updateprot(ic);
		}
		if ((bss->ni_capinfo ^ capinfo) &
		    IEEE80211_CAPINFO_SHORT_SLOTTIME) {
			bss->ni_capinfo = capinfo;
			ieee80211_set_shortslottime(ic,
			    ic->ic_curmode == IEEE80211_MODE_11A ||
			    (capinfo & IEEE80211_CAPINFO_SHORT_SLOTTIME));
		}
		if (tim && bss->ni_dtimperiod != ni->ni_dtimperiod) {
			bss->ni_dtimperiod = ni->ni_dtimperiod;
			bss->ni_dtimcount = ni->ni_dtimcount;
			if (ic->ic_updatedtim != NULL)
				ic->ic_updatedtim(ic);
		}
		/* The AP answers; a beacon-miss probe is not needed. */
		ic->ic_mgt_timer = 0;
		bss->ni_rssi = (u_int8_t)rxi->rxi_rssi;
	}

	if (ic->ic_state == IEEE80211_S_SCAN) {
		struct ieee80211_rsnparams rsn;

		if (edcaie != NULL || wmmie != NULL)
			ni->ni_flags |= IEEE80211_NODE_QOS;
		else
			ni->ni_flags &= ~IEEE80211_NODE_QOS;
		ni->ni_rsnprotos = IEEE80211_PROTO_NONE;
		ni->ni_supported_rsnprotos = IEEE80211_PROTO_NONE;
		ni->ni_rsnakms = 0;
		ni->ni_supported_rsnakms = 0;
		ni->ni_rsnciphers = 0;
		ni->ni_rsngroupcipher = 0;
		ni->ni_rsngroupmgmtcipher = 0;
		ni->ni_rsncaps = 0;
		if (rsnie != NULL && rsnie[1] + 2 <= WPA_IE_MAX &&
		    ieee80211_parse_rsn(rsnie, &rsn) == 0 &&
		    ieee80211_save_ie(rsnie, &ni->ni_rsnie) == 0) {
			ni->ni_supported_rsnprotos = IEEE80211_PROTO_RSN;
			ni->ni_supported_rsnakms = rsn.rsn_akms;
			ni->ni_rsnprotos = IEEE80211_PROTO_RSN;
			ni->ni_rsnakms = rsn.rsn_akms;
			ni->ni_rsnciphers = rsn.rsn_ciphers;
			ni->ni_rsngroupcipher = rsn.rsn_groupcipher;
			ni->ni_rsngroupmgmtcipher = rsn.rsn_groupmgmtcipher;
			ni->ni_rsncaps = rsn.rsn_caps;
		}
	}

	if (ssid[1] != 0 && ni->ni_essid[0] == '\0') {
		ni->ni_esslen = ssid[1];
		memset(ni->ni_essid, 0, sizeof(ni->ni_essid));
		memcpy(ni->ni_essid, &ssid[2], ssid[1]);
	}
	IEEE80211_ADDR_COPY(ni->ni_bssid, wh->i_addr3);
	if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) && !isprobe &&
	    ni->ni_rssi > rxi->rxi_rssi)
		;	/* keep the stronger probe-response RSSI on 5 GHz */
	else
		ni->ni_rssi = (u_int8_t)rxi->rxi_rssi;
	ni->ni_rstamp = rxi->rxi_tstamp;
	ni->ni_intval = bintval;
	ni->ni_capinfo = capinfo;
	ni->ni_erp = erp;
	ieee80211_setup_rates(ic, ni, rates, xrates, IEEE80211_F_DOSORT);
}

static void
ieee80211_recv_auth(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni)
{
	const struct ieee80211_frame *wh;
	const u_int8_t *frm;
	u_int16_t algo, seq, status;

	if (m->m_len < (int)sizeof(*wh) + 6)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	frm = (const u_int8_t *)&wh[1];
	algo = LE_READ_2(frm); frm += 2;
	seq = LE_READ_2(frm); frm += 2;
	status = LE_READ_2(frm);

	if (algo != IEEE80211_AUTH_ALG_OPEN) {
		ic->ic_stats.is_rx_auth_unsupported++;
		return;
	}
	if (ic->ic_state != IEEE80211_S_AUTH ||
	    seq != IEEE80211_AUTH_OPEN_RESPONSE) {
		ic->ic_stats.is_rx_bad_auth++;
		return;
	}
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
		ni->ni_flags &= ~IEEE80211_NODE_RXMGMTPROT;
		ni->ni_flags &= ~IEEE80211_NODE_TXMGMTPROT;
		ni->ni_port_valid = 0;
		ni->ni_replaycnt_ok = 0;
		if (ni->ni_pairwise_key.k_cipher != IEEE80211_CIPHER_NONE)
			(*ic->ic_delete_key)(ic, ni, &ni->ni_pairwise_key);
	}
	if (status != 0) {
		char msg[64];

		snprintf(msg, sizeof(msg), "authentication refused (status %u)",
		    status);
		set_error(msg);
		ic->ic_stats.is_rx_auth_fail++;
		return;	/* the watchdog moves us back to SCAN */
	}
	printf("wlan: authenticated with %s\n", ether_sprintf(wh->i_addr3));
	ieee80211_new_state(ic, IEEE80211_S_ASSOC,
	    wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK);
}

static void
ieee80211_recv_assoc_resp(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni)
{
	const struct ieee80211_frame *wh;
	const u_int8_t *frm, *efrm, *rates, *xrates, *edcaie, *wmmie;
	u_int16_t capinfo, status, associd;
	u_int8_t rate;

	if (ic->ic_state != IEEE80211_S_ASSOC) {
		ic->ic_stats.is_rx_mgtdiscard++;
		return;
	}
	if (m->m_len < (int)sizeof(*wh) + 6)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	frm = (const u_int8_t *)&wh[1];
	efrm = mtod(m, u_int8_t *) + m->m_len;
	capinfo = LE_READ_2(frm); frm += 2;
	status = LE_READ_2(frm); frm += 2;
	if (status != IEEE80211_STATUS_SUCCESS) {
		char msg[64];

		snprintf(msg, sizeof(msg), "association refused (status %u)",
		    status);
		set_error(msg);
		ic->ic_stats.is_rx_auth_fail++;
		return;
	}
	associd = LE_READ_2(frm); frm += 2;

	rates = xrates = edcaie = wmmie = NULL;
	while (frm + 2 <= efrm) {
		if (frm + 2 + frm[1] > efrm)
			break;
		switch (frm[0]) {
		case IEEE80211_ELEMID_RATES:
			rates = frm;
			break;
		case IEEE80211_ELEMID_XRATES:
			xrates = frm;
			break;
		case IEEE80211_ELEMID_EDCAPARMS:
			edcaie = frm;
			break;
		case IEEE80211_ELEMID_VENDOR:
			if (frm[1] >= 5 && memcmp(frm + 2, MICROSOFT_OUI, 3) == 0 &&
			    frm[5] == 2 && frm[6] == 1)
				wmmie = frm;
			break;
		}
		frm += 2 + frm[1];
	}
	if (rates == NULL || rates[1] > IEEE80211_RATE_MAXSIZE)
		return;
	rate = (u_int8_t)ieee80211_setup_rates(ic, ni, rates, xrates,
	    IEEE80211_F_DOSORT | IEEE80211_F_DOFRATE | IEEE80211_F_DONEGO |
	    IEEE80211_F_DODEL);
	if (rate & IEEE80211_RATE_BASIC) {
		set_error("no common rates with the access point");
		ic->ic_stats.is_rx_assoc_norate++;
		return;
	}
	ni->ni_capinfo = capinfo;
	ni->ni_associd = associd;
	/* QoS is not negotiated (no WME IE was sent); keep plain data. */
	ni->ni_flags &= ~IEEE80211_NODE_QOS;
	(void)edcaie; (void)wmmie;

	ieee80211_setmode(ic, ieee80211_node_abg_mode(ic, ni));
	ieee80211_reset_erp(ic);
	if (ic->ic_curmode == IEEE80211_MODE_11A ||
	    (ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_PREAMBLE))
		ic->ic_flags |= IEEE80211_F_SHPREAMBLE;
	else
		ic->ic_flags &= ~IEEE80211_F_SHPREAMBLE;
	ieee80211_set_shortslottime(ic, ic->ic_curmode == IEEE80211_MODE_11A ||
	    (ni->ni_capinfo & IEEE80211_CAPINFO_SHORT_SLOTTIME));
	if (ic->ic_curmode == IEEE80211_MODE_11G &&
	    (ni->ni_erp & IEEE80211_ERP_USE_PROTECTION))
		ic->ic_flags |= IEEE80211_F_USEPROT;
	else
		ic->ic_flags &= ~IEEE80211_F_USEPROT;

	if (ic->ic_flags & IEEE80211_F_RSNON) {
		ni->ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
		wpa_sm_init(&wpa_sm, ic->ic_psk, ni->ni_macaddr, ic->ic_myaddr,
		    ni->ni_rsnie, ni->ni_rsnie ? 2 + ni->ni_rsnie[1] : 0,
		    wpa_own_ie, wpa_own_ie_len, arc4random_buf);
		printf("wlan: associated (aid %u), waiting for the 4-way "
		    "handshake\n", associd & 0x3fff);
	}
	ieee80211_new_state(ic, IEEE80211_S_RUN,
	    IEEE80211_FC0_SUBTYPE_ASSOC_RESP);
}

static void
ieee80211_recv_deauth(struct ieee80211com *ic, struct mbuf *m)
{
	const struct ieee80211_frame *wh;
	u_int16_t reason;
	char msg[64];

	if (m->m_len < (int)sizeof(*wh) + 2)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	reason = LE_READ_2((const u_int8_t *)&wh[1]);
	ic->ic_stats.is_rx_deauth++;
	if (ic->ic_state < IEEE80211_S_AUTH)
		return;
	if (ic->ic_state == IEEE80211_S_RUN &&
	    (ic->ic_flags & IEEE80211_F_RSNON) && !ic->ic_bss->ni_port_valid)
		ic->ic_stats.is_handshake_fail++;
	snprintf(msg, sizeof(msg), "deauthenticated by AP (reason %u%s)",
	    reason, reason == 15 || reason == 2 ? ", wrong password?" : "");
	set_error(msg);
	ieee80211_new_state(ic, IEEE80211_S_AUTH, IEEE80211_FC0_SUBTYPE_DEAUTH);
}

static void
ieee80211_recv_disassoc(struct ieee80211com *ic, struct mbuf *m)
{
	const struct ieee80211_frame *wh;
	u_int16_t reason;
	char msg[64];

	if (m->m_len < (int)sizeof(*wh) + 2)
		return;
	wh = mtod(m, struct ieee80211_frame *);
	reason = LE_READ_2((const u_int8_t *)&wh[1]);
	ic->ic_stats.is_rx_disassoc++;
	if (ic->ic_state != IEEE80211_S_RUN)
		return;
	snprintf(msg, sizeof(msg), "disassociated by AP (reason %u)", reason);
	set_error(msg);
	ieee80211_new_state(ic, IEEE80211_S_ASSOC,
	    IEEE80211_FC0_SUBTYPE_DISASSOC);
}

static void
ieee80211_recv_mgmt(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni, struct ieee80211_rxinfo *rxi, int subtype)
{
	switch (subtype) {
	case IEEE80211_FC0_SUBTYPE_BEACON:
		ieee80211_recv_probe_resp(ic, m, rxi, 0);
		break;
	case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
		ieee80211_recv_probe_resp(ic, m, rxi, 1);
		break;
	case IEEE80211_FC0_SUBTYPE_AUTH:
		ieee80211_recv_auth(ic, m, ni);
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
		ieee80211_recv_assoc_resp(ic, m, ni);
		break;
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
		ieee80211_recv_deauth(ic, m);
		break;
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
		ieee80211_recv_disassoc(ic, m);
		break;
	default:
		ic->ic_stats.is_rx_badsubtype++;
		break;
	}
}

static int
ieee80211_ccmp_get_pn(uint64_t *pn, uint64_t **prsc, struct mbuf *m,
    struct ieee80211_key *k)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int hdrlen = (int)ieee80211_get_hdrlen(wh);
	const u_int8_t *ivp;
	u_int8_t tid;

	if (m->m_len < hdrlen + IEEE80211_CCMP_HDRLEN)
		return EINVAL;
	ivp = (u_int8_t *)wh + hdrlen;
	if (!(ivp[3] & IEEE80211_WEP_EXTIV))
		return EINVAL;
	tid = ieee80211_has_qos(wh) ?
	    ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;
	*prsc = &k->k_rsc[tid];
	*pn = (u_int64_t)ivp[0] | (u_int64_t)ivp[1] << 8 |
	    (u_int64_t)ivp[4] << 16 | (u_int64_t)ivp[5] << 24 |
	    (u_int64_t)ivp[6] << 32 | (u_int64_t)ivp[7] << 40;
	return 0;
}

static struct ieee80211_key *
ieee80211_get_rxkey(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni)
{
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	int hdrlen;

	if ((ic->ic_flags & IEEE80211_F_RSNON) &&
	    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
	    ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
		return &ni->ni_pairwise_key;
	hdrlen = (int)ieee80211_get_hdrlen(wh);
	if (m->m_len < hdrlen + 4)
		return NULL;
	return &ic->ic_nw_keys[((u_int8_t *)wh)[hdrlen + 3] >> 6];
}

/* After hardware decryption: check and record the PN, strip the IV. */
static struct mbuf *
ieee80211_input_hwdecrypt(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct mbuf *m, struct ieee80211_rxinfo *rxi)
{
	struct ieee80211_key *k = ieee80211_get_rxkey(ic, m, ni);
	struct ieee80211_frame *wh;
	uint64_t pn, *prsc;
	int hdrlen;

	if (k == NULL) {
		m_freem(m);
		return NULL;
	}
	wh = mtod(m, struct ieee80211_frame *);
	if (k->k_cipher != IEEE80211_CIPHER_CCMP ||
	    !(wh->i_fc[1] & IEEE80211_FC1_PROTECTED))
		return m;
	if (ieee80211_ccmp_get_pn(&pn, &prsc, m, k) != 0) {
		ic->ic_stats.is_ccmp_dec_errs++;
		m_freem(m);
		return NULL;
	}
	if ((rxi->rxi_flags & IEEE80211_RXI_HWDEC_SAME_PN) ? pn < *prsc :
	    pn <= *prsc) {
		ic->ic_stats.is_ccmp_replays++;
		m_freem(m);
		return NULL;
	}
	*prsc = pn;
	hdrlen = (int)ieee80211_get_hdrlen(wh);
	wh->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;
	memmove(mtod(m, caddr_t) + IEEE80211_CCMP_HDRLEN, wh, hdrlen);
	m_adj(m, IEEE80211_CCMP_HDRLEN);
	return m;
}

static void
ieee80211_enqueue_data(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni, struct mbuf_list *ml)
{
	struct ether_header *eh = mtod(m, struct ether_header *);

	if ((ic->ic_flags & IEEE80211_F_RSNON) && !ni->ni_port_valid &&
	    eh->ether_type != htons(ETHERTYPE_EAPOL)) {
		ic->ic_stats.is_rx_unauth++;
		m_freem(m);
		return;
	}
	if ((ic->ic_flags & IEEE80211_F_RSNON) &&
	    eh->ether_type == htons(ETHERTYPE_EAPOL)) {
		ieee80211_eapol_key_input(ic, m, ni);
		return;
	}
	ml_enqueue(ml, m);
}

static void
ieee80211_decap(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni, int hdrlen, struct mbuf_list *ml)
{
	struct ether_header eh;
	struct ieee80211_frame *wh;
	struct llc *llc;

	if (m->m_len < hdrlen + LLC_SNAPFRAMELEN) {
		ic->ic_stats.is_rx_decap++;
		m_freem(m);
		return;
	}
	wh = mtod(m, struct ieee80211_frame *);
	switch (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) {
	case IEEE80211_FC1_DIR_NODS:
		IEEE80211_ADDR_COPY(eh.ether_dhost, wh->i_addr1);
		IEEE80211_ADDR_COPY(eh.ether_shost, wh->i_addr2);
		break;
	case IEEE80211_FC1_DIR_TODS:
		IEEE80211_ADDR_COPY(eh.ether_dhost, wh->i_addr3);
		IEEE80211_ADDR_COPY(eh.ether_shost, wh->i_addr2);
		break;
	case IEEE80211_FC1_DIR_FROMDS:
		IEEE80211_ADDR_COPY(eh.ether_dhost, wh->i_addr1);
		IEEE80211_ADDR_COPY(eh.ether_shost, wh->i_addr3);
		break;
	default:
		IEEE80211_ADDR_COPY(eh.ether_dhost, wh->i_addr3);
		IEEE80211_ADDR_COPY(eh.ether_shost,
		    ((struct ieee80211_frame_addr4 *)wh)->i_addr4);
		break;
	}
	llc = (struct llc *)((caddr_t)wh + hdrlen);
	if (llc->llc_dsap == LLC_SNAP_LSAP && llc->llc_ssap == LLC_SNAP_LSAP &&
	    llc->llc_control == LLC_UI && llc->llc_snap.org_code[0] == 0 &&
	    llc->llc_snap.org_code[1] == 0 && llc->llc_snap.org_code[2] == 0) {
		eh.ether_type = llc->llc_snap.ether_type;
		m_adj(m, hdrlen + LLC_SNAPFRAMELEN - ETHER_HDR_LEN);
	} else {
		eh.ether_type = htons((u_int16_t)(m->m_len - hdrlen));
		m_adj(m, hdrlen - ETHER_HDR_LEN);
	}
	memcpy(mtod(m, caddr_t), &eh, ETHER_HDR_LEN);
	ieee80211_enqueue_data(ic, m, ni, ml);
}

/*
 * Process a received frame: management frames now, data frames are
 * decapsulated onto `ml' for if_input().
 */
void
ieee80211_inputm(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node *ni,
    struct ieee80211_rxinfo *rxi, struct mbuf_list *ml)
{
	struct ieee80211com *ic = (void *)ifp;
	struct ieee80211_frame *wh;
	u_int16_t *orxseq, nrxseq;
	u_int8_t dir, type, subtype, tid;
	int hdrlen, hasqos;

	if (m->m_len < (int)sizeof(struct ieee80211_frame_min)) {
		ic->ic_stats.is_rx_tooshort++;
		goto out;
	}
	wh = mtod(m, struct ieee80211_frame *);
	if ((wh->i_fc[0] & IEEE80211_FC0_VERSION_MASK) !=
	    IEEE80211_FC0_VERSION_0) {
		ic->ic_stats.is_rx_badversion++;
		goto err;
	}
	dir = wh->i_fc[1] & IEEE80211_FC1_DIR_MASK;
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

	if (type == IEEE80211_FC0_TYPE_CTL) {
		ic->ic_stats.is_rx_ctl++;
		goto out;
	}
	hdrlen = (int)ieee80211_get_hdrlen(wh);
	if (m->m_len < hdrlen) {
		ic->ic_stats.is_rx_tooshort++;
		goto err;
	}
	hasqos = ieee80211_has_qos(wh);
	tid = hasqos ? ieee80211_get_qos(wh) & IEEE80211_QOS_TID : 0;

	/* No fragment support: drop fragments (see OpenBSD's rationale). */
	{
		u_int16_t rxseq = letoh16(*(const u_int16_t *)wh->i_seq);
		if ((wh->i_fc[1] & IEEE80211_FC1_MORE_FRAG) ||
		    (rxseq & IEEE80211_SEQ_FRAG_MASK))
			goto err;
	}

	/* duplicate detection (802.11-2012 9.3.2.10) */
	if (ic->ic_state != IEEE80211_S_SCAN) {
		nrxseq = letoh16(*(u_int16_t *)wh->i_seq) >>
		    IEEE80211_SEQ_SEQ_SHIFT;
		orxseq = hasqos ? &ni->ni_qos_rxseqs[tid] : &ni->ni_rxseq;
		if (rxi->rxi_flags & IEEE80211_RXI_SAME_SEQ) {
			if (nrxseq != *orxseq) {
				ic->ic_stats.is_rx_dup++;
				goto out;
			}
		} else if ((wh->i_fc[1] & IEEE80211_FC1_RETRY) &&
		    nrxseq == *orxseq) {
			ic->ic_stats.is_rx_dup++;
			goto out;
		}
		*orxseq = nrxseq;
	}
	if (ic->ic_state > IEEE80211_S_SCAN && type == IEEE80211_FC0_TYPE_DATA) {
		if (rxi->rxi_rssi != 0)
			ni->ni_rssi = (u_int8_t)rxi->rxi_rssi;
		ni->ni_rstamp = rxi->rxi_tstamp;
		ni->ni_inact = 0;
	}

	switch (type) {
	case IEEE80211_FC0_TYPE_DATA:
		if (dir != IEEE80211_FC1_DIR_FROMDS) {
			ic->ic_stats.is_rx_wrongdir++;
			goto out;
		}
		if (ic->ic_state != IEEE80211_S_RUN ||
		    !IEEE80211_ADDR_EQ(wh->i_addr2, ni->ni_bssid)) {
			ic->ic_stats.is_rx_wrongbss++;
			goto out;
		}
		if (IEEE80211_IS_MULTICAST(wh->i_addr1) &&
		    IEEE80211_ADDR_EQ(wh->i_addr3, ic->ic_myaddr)) {
			ic->ic_stats.is_rx_mcastecho++;
			goto out;
		}
		if (subtype & IEEE80211_FC0_SUBTYPE_NODATA)
			goto out;

		if ((ic->ic_flags & IEEE80211_F_RSNON) &&
		    (ni->ni_flags & IEEE80211_NODE_RXPROT)) {
			if (!(rxi->rxi_flags & IEEE80211_RXI_HWDEC)) {
				if (!(wh->i_fc[1] & IEEE80211_FC1_PROTECTED)) {
					ic->ic_stats.is_rx_unencrypted++;
					goto err;
				}
				m = ieee80211_decrypt(ic, m, ni);
			} else
				m = ieee80211_input_hwdecrypt(ic, ni, m, rxi);
			if (m == NULL) {
				ic->ic_stats.is_rx_wepfail++;
				ifp->if_ierrors++;
				return;
			}
			wh = mtod(m, struct ieee80211_frame *);
		} else if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) ||
		    (rxi->rxi_flags & IEEE80211_RXI_HWDEC)) {
			ic->ic_stats.is_rx_nowep++;
			goto out;
		}
		ieee80211_decap(ic, m, ni, hdrlen, ml);
		return;

	case IEEE80211_FC0_TYPE_MGT:
		if (dir != IEEE80211_FC1_DIR_NODS) {
			ic->ic_stats.is_rx_wrongdir++;
			goto err;
		}
		if (ic->ic_state == IEEE80211_S_SCAN &&
		    subtype != IEEE80211_FC0_SUBTYPE_BEACON &&
		    subtype != IEEE80211_FC0_SUBTYPE_PROBE_RESP) {
			ic->ic_stats.is_rx_mgtdiscard++;
			goto out;
		}
		if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
			/* no MFP: protected management frames are dropped */
			ic->ic_stats.is_rx_nowep++;
			goto out;
		}
		/* Only listen to our AP (or anyone while scanning). */
		if (subtype != IEEE80211_FC0_SUBTYPE_BEACON &&
		    subtype != IEEE80211_FC0_SUBTYPE_PROBE_RESP &&
		    !IEEE80211_ADDR_EQ(wh->i_addr2, ni->ni_macaddr)) {
			ic->ic_stats.is_rx_wrongbss++;
			goto out;
		}
		ieee80211_recv_mgmt(ic, m, ni, rxi, subtype);
		goto out;
	}
 err:
	ifp->if_ierrors++;
 out:
	m_freem(m);
}

/* ==== output =========================================================== */

enum ieee80211_edca_ac
ieee80211_up_to_ac(struct ieee80211com *ic, int up)
{
	static const enum ieee80211_edca_ac up_to_ac[] = {
		EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_BK, EDCA_AC_BE,
		EDCA_AC_VI, EDCA_AC_VI, EDCA_AC_VO, EDCA_AC_VO
	};
	(void)ic;
	return up_to_ac[up & 7];
}

u_int8_t *
ieee80211_add_rates(u_int8_t *frm, const struct ieee80211_rateset *rs)
{
	int nrates;

	*frm++ = IEEE80211_ELEMID_RATES;
	nrates = MIN(rs->rs_nrates, IEEE80211_RATE_SIZE);
	*frm++ = (u_int8_t)nrates;
	memcpy(frm, rs->rs_rates, nrates);
	return frm + nrates;
}

u_int8_t *
ieee80211_add_xrates(u_int8_t *frm, const struct ieee80211_rateset *rs)
{
	int nrates = rs->rs_nrates - IEEE80211_RATE_SIZE;

	*frm++ = IEEE80211_ELEMID_XRATES;
	*frm++ = (u_int8_t)nrates;
	memcpy(frm, rs->rs_rates + IEEE80211_RATE_SIZE, nrates);
	return frm + nrates;
}

static u_int8_t *
ieee80211_add_ssid(u_int8_t *frm, const u_int8_t *ssid, u_int len)
{
	*frm++ = IEEE80211_ELEMID_SSID;
	*frm++ = (u_int8_t)len;
	memcpy(frm, ssid, len);
	return frm + len;
}

u_int8_t *
ieee80211_add_htcaps(u_int8_t *frm, struct ieee80211com *ic)
{
	*frm++ = IEEE80211_ELEMID_HTCAPS;
	*frm++ = 26;
	LE_WRITE_2(frm, ic->ic_htcaps); frm += 2;
	*frm++ = ic->ic_ampdu_params;
	memcpy(frm, ic->ic_sup_mcs, 10); frm += 10;
	LE_WRITE_2(frm, (ic->ic_max_rxrate & IEEE80211_MCS_RX_RATE_HIGH));
	frm += 2;
	*frm++ = ic->ic_tx_mcs_set;
	*frm++ = 0; *frm++ = 0; *frm++ = 0;	/* reserved */
	LE_WRITE_2(frm, ic->ic_htxcaps); frm += 2;
	frm[0] = (u_int8_t)ic->ic_txbfcaps;
	frm[1] = (u_int8_t)(ic->ic_txbfcaps >> 8);
	frm[2] = (u_int8_t)(ic->ic_txbfcaps >> 16);
	frm[3] = (u_int8_t)(ic->ic_txbfcaps >> 24);
	frm += 4;
	*frm++ = ic->ic_aselcaps;
	return frm;
}

u_int8_t *
ieee80211_add_vhtcaps(u_int8_t *frm, struct ieee80211com *ic)
{
	*frm++ = IEEE80211_ELEMID_VHTCAPS;
	*frm++ = 12;
	frm[0] = (u_int8_t)ic->ic_vhtcaps;
	frm[1] = (u_int8_t)(ic->ic_vhtcaps >> 8);
	frm[2] = (u_int8_t)(ic->ic_vhtcaps >> 16);
	frm[3] = (u_int8_t)(ic->ic_vhtcaps >> 24);
	frm += 4;
	LE_WRITE_2(frm, ic->ic_vht_rxmcs); frm += 2;
	LE_WRITE_2(frm, ic->ic_vht_rx_max_lgi_mbit_s); frm += 2;
	LE_WRITE_2(frm, ic->ic_vht_txmcs); frm += 2;
	LE_WRITE_2(frm, ic->ic_vht_tx_max_lgi_mbit_s); frm += 2;
	return frm;
}

/* Queue a management frame (802.11 header prepended here). */
static int
ieee80211_mgmt_output(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct mbuf *m, int type)
{
	struct ieee80211_frame *wh;

	ni->ni_inact = 0;
	M_PREPEND(m, sizeof(struct ieee80211_frame), M_DONTWAIT);
	if (m == NULL)
		return ENOMEM;
	m->m_pkthdr.ph_cookie = ni;
	wh = mtod(m, struct ieee80211_frame *);
	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_MGT | type;
	wh->i_fc[1] = IEEE80211_FC1_DIR_NODS;
	*(u_int16_t *)&wh->i_dur[0] = 0;
	*(u_int16_t *)&wh->i_seq[0] =
	    htole16(ni->ni_txseq << IEEE80211_SEQ_SEQ_SHIFT);
	ni->ni_txseq = (ni->ni_txseq + 1) & 0xfff;
	IEEE80211_ADDR_COPY(wh->i_addr1, ni->ni_macaddr);
	IEEE80211_ADDR_COPY(wh->i_addr2, ic->ic_myaddr);
	IEEE80211_ADDR_COPY(wh->i_addr3, ni->ni_bssid);
	mq_enqueue(&ic->ic_mgtq, m);
	ic->ic_if.if_timer = 1;
	if_start(&ic->ic_if);
	return 0;
}

int
ieee80211_send_mgmt(struct ieee80211com *ic, struct ieee80211_node *ni,
    int type, int arg1, int arg2)
{
	const struct ieee80211_rateset *rs;
	struct mbuf *m;
	u_int8_t *frm, *start;
	u_int16_t capinfo;
	int timer = 0, ret;

	(void)arg2;
	m = m_get_lead(FRAME_LEAD, 0);
	if (m == NULL) {
		ic->ic_stats.is_tx_nombuf++;
		return ENOMEM;
	}
	start = frm = mtod(m, u_int8_t *);
	switch (type) {
	case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
		rs = &ic->ic_sup_rates[ieee80211_node_abg_mode(ic, ni)];
		frm = ieee80211_add_ssid(frm, ni->ni_essid, ni->ni_esslen);
		frm = ieee80211_add_rates(frm, rs);
		if (rs->rs_nrates > IEEE80211_RATE_SIZE)
			frm = ieee80211_add_xrates(frm, rs);
		timer = IEEE80211_TRANS_WAIT;
		break;
	case IEEE80211_FC0_SUBTYPE_AUTH:
		LE_WRITE_2(frm, IEEE80211_AUTH_ALG_OPEN); frm += 2;
		LE_WRITE_2(frm, arg1 & 0xffff); frm += 2;
		LE_WRITE_2(frm, arg1 >> 16); frm += 2;
		timer = IEEE80211_TRANS_WAIT;
		printf("wlan: sending auth to %s channel %u\n",
		    ether_sprintf(ni->ni_macaddr),
		    ieee80211_chan2ieee(ic, ni->ni_chan));
		break;
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
		LE_WRITE_2(frm, arg1); frm += 2;
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
	case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
		rs = &ni->ni_rates;
		capinfo = IEEE80211_CAPINFO_ESS;
		if ((ic->ic_flags & IEEE80211_F_SHPREAMBLE) &&
		    ni->ni_chan != IEEE80211_CHAN_ANYC &&
		    IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
			capinfo |= IEEE80211_CAPINFO_SHORT_PREAMBLE;
		if (ic->ic_caps & IEEE80211_C_SHSLOT)
			capinfo |= IEEE80211_CAPINFO_SHORT_SLOTTIME;
		LE_WRITE_2(frm, capinfo); frm += 2;
		LE_WRITE_2(frm, ic->ic_lintval); frm += 2;
		if (type == IEEE80211_FC0_SUBTYPE_REASSOC_REQ) {
			IEEE80211_ADDR_COPY(frm, ni->ni_bssid);
			frm += IEEE80211_ADDR_LEN;
		}
		frm = ieee80211_add_ssid(frm, ni->ni_essid, ni->ni_esslen);
		frm = ieee80211_add_rates(frm, rs);
		if (rs->rs_nrates > IEEE80211_RATE_SIZE)
			frm = ieee80211_add_xrates(frm, rs);
		if (ic->ic_flags & IEEE80211_F_RSNON) {
			/* Keep the AP's PTKSA/GTKSA replay counter sizes. */
			wpa_own_ie_len = wpa_build_rsn_ie(wpa_own_ie,
			    ni->ni_rsncaps & (IEEE80211_RSNCAP_PTKSA_RCNT_MASK |
			    IEEE80211_RSNCAP_GTKSA_RCNT_MASK));
			memcpy(frm, wpa_own_ie, wpa_own_ie_len);
			frm += wpa_own_ie_len;
		}
		timer = IEEE80211_TRANS_WAIT;
		printf("wlan: sending %sassociation request\n",
		    type == IEEE80211_FC0_SUBTYPE_REASSOC_REQ ? "re" : "");
		break;
	default:
		m_freem(m);
		ic->ic_stats.is_tx_unknownmgt++;
		return EINVAL;
	}
	m->m_len = m->m_pkthdr.len = (int)(frm - start);

	ieee80211_ref_node(ni);
	ret = ieee80211_mgmt_output(ic, ni, m, type);
	if (ret == 0) {
		if (timer)
			ic->ic_mgt_timer = timer;
	} else
		ieee80211_release_node(ic, ni);
	return ret;
}

/*
 * Ethernet -> 802.11 (ToDS, LLC/SNAP).  Non-QoS data frames only: QoS
 * framing is only needed for aggregation, which this port does not do.
 */
struct mbuf *
ieee80211_encap(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node **pni)
{
	struct ieee80211com *ic = (void *)ifp;
	struct ether_header eh;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct llc *llc;

	if (m->m_len < (int)sizeof(struct ether_header)) {
		m_freem(m);
		*pni = NULL;
		return NULL;
	}
	memcpy(&eh, mtod(m, caddr_t), sizeof(eh));
	ni = ieee80211_find_txnode(ic, eh.ether_dhost);
	if ((ic->ic_flags & IEEE80211_F_RSNON) && !ni->ni_port_valid &&
	    eh.ether_type != htons(ETHERTYPE_EAPOL)) {
		ic->ic_stats.is_tx_noauth++;
		goto bad;
	}
	ni->ni_inact = 0;

	m_adj(m, sizeof(struct ether_header) - LLC_SNAPFRAMELEN);
	llc = mtod(m, struct llc *);
	llc->llc_dsap = llc->llc_ssap = LLC_SNAP_LSAP;
	llc->llc_control = LLC_UI;
	llc->llc_snap.org_code[0] = 0;
	llc->llc_snap.org_code[1] = 0;
	llc->llc_snap.org_code[2] = 0;
	llc->llc_snap.ether_type = eh.ether_type;
	M_PREPEND(m, sizeof(struct ieee80211_frame), M_DONTWAIT);
	if (m == NULL) {
		ic->ic_stats.is_tx_nombuf++;
		ieee80211_release_node(ic, ni);
		*pni = NULL;
		return NULL;
	}
	wh = mtod(m, struct ieee80211_frame *);
	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_DATA;
	wh->i_fc[1] = IEEE80211_FC1_DIR_TODS;
	*(u_int16_t *)&wh->i_dur[0] = 0;
	*(u_int16_t *)&wh->i_seq[0] =
	    htole16(ni->ni_txseq << IEEE80211_SEQ_SEQ_SHIFT);
	ni->ni_txseq = (ni->ni_txseq + 1) & 0xfff;
	IEEE80211_ADDR_COPY(wh->i_addr1, ni->ni_bssid);
	IEEE80211_ADDR_COPY(wh->i_addr2, eh.ether_shost);
	IEEE80211_ADDR_COPY(wh->i_addr3, eh.ether_dhost);
	if ((ic->ic_flags & IEEE80211_F_RSNON) &&
	    (ni->ni_flags & IEEE80211_NODE_TXPROT))
		wh->i_fc[1] |= IEEE80211_FC1_PROTECTED;
	*pni = ni;
	return m;
 bad:
	m_freem(m);
	ieee80211_release_node(ic, ni);
	*pni = NULL;
	return NULL;
}

/* ==== crypto =========================================================== */

int
ieee80211_cipher_keylen(enum ieee80211_cipher cipher)
{
	switch (cipher) {
	case IEEE80211_CIPHER_WEP40:	return 5;
	case IEEE80211_CIPHER_TKIP:	return 32;
	case IEEE80211_CIPHER_CCMP:	return 16;
	case IEEE80211_CIPHER_WEP104:	return 13;
	case IEEE80211_CIPHER_BIP:	return 16;
	default:			return 0;
	}
}

/* Software keys: only CCMP is implemented (group data decryption). */
int
ieee80211_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	(void)ic; (void)ni;
	if (k->k_cipher != IEEE80211_CIPHER_CCMP)
		return EINVAL;
	k->k_flags |= IEEE80211_KEY_SWCRYPTO;
	return 0;
}

void
ieee80211_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	(void)ic; (void)ni;
	explicit_bzero(k, sizeof(*k));
}

void
ieee80211_crypto_clear_groupkeys(struct ieee80211com *ic)
{
	int i;

	for (i = 0; i < IEEE80211_GROUP_NKID; i++) {
		struct ieee80211_key *k = &ic->ic_nw_keys[i];
		if (k->k_cipher != IEEE80211_CIPHER_NONE)
			(*ic->ic_delete_key)(ic, NULL, k);
		explicit_bzero(k, sizeof(*k));
	}
}

struct ieee80211_key *
ieee80211_get_txkey(struct ieee80211com *ic, const struct ieee80211_frame *wh,
    struct ieee80211_node *ni)
{
	if ((ic->ic_flags & IEEE80211_F_RSNON) &&
	    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
	    ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
		return &ni->ni_pairwise_key;
	return &ic->ic_nw_keys[ic->ic_def_txkey];
}

struct mbuf *
ieee80211_encrypt(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_key *k)
{
	/* Only the pairwise key transmits, in hardware. */
	(void)ic; (void)k;
	printf("wlan: no software encryption for cipher %d, frame dropped\n",
	    k->k_cipher);
	m_freem(m);
	return NULL;
}

/* Software CCMP decryption (group-addressed frames under the GTK). */
struct mbuf *
ieee80211_decrypt(struct ieee80211com *ic, struct mbuf *m0,
    struct ieee80211_node *ni)
{
	struct ieee80211_key *k = ieee80211_get_rxkey(ic, m0, ni);
	struct ieee80211_frame *wh;
	struct mbuf *n;
	uint64_t pn, *prsc;
	int hdrlen, len;

	if (k == NULL || (k->k_flags & IEEE80211_KEY_SWCRYPTO) == 0 ||
	    k->k_cipher != IEEE80211_CIPHER_CCMP)
		goto drop;
	wh = mtod(m0, struct ieee80211_frame *);
	hdrlen = (int)ieee80211_get_hdrlen(wh);
	len = m0->m_len - hdrlen - IEEE80211_CCMP_HDRLEN;
	if (len < IEEE80211_CCMP_MICLEN)
		goto drop;
	if (ieee80211_ccmp_get_pn(&pn, &prsc, m0, k) != 0)
		goto drop;
	if (pn <= *prsc) {
		ic->ic_stats.is_ccmp_replays++;
		goto drop;
	}
	n = m_get_lead(FRAME_LEAD, (u_int)(hdrlen + len - IEEE80211_CCMP_MICLEN));
	if (n == NULL)
		goto drop;
	if (wpa_ccmp_decrypt(k->k_key, (const uint8_t *)wh, (size_t)hdrlen, pn,
	    (const uint8_t *)wh + hdrlen + IEEE80211_CCMP_HDRLEN, (size_t)len,
	    mtod(n, uint8_t *) + hdrlen) != 0) {
		ic->ic_stats.is_ccmp_dec_errs++;
		m_freem(n);
		goto drop;
	}
	*prsc = pn;
	memcpy(mtod(n, caddr_t), wh, hdrlen);
	mtod(n, struct ieee80211_frame *)->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;
	m_freem(m0);
	return n;
 drop:
	m_freem(m0);
	return NULL;
}

/* ==== EAPOL ============================================================ */

static void
ieee80211_install_keys(struct ieee80211com *ic, struct ieee80211_node *ni,
    const struct wpa_result *res)
{
	struct ieee80211_key *k;
	int err;

	if (res->install_ptk) {
		k = &ni->ni_pairwise_key;
		memset(k, 0, sizeof(*k));
		k->k_cipher = IEEE80211_CIPHER_CCMP;
		k->k_rsc[0] = res->ptk_rsc;
		k->k_len = 16;
		memcpy(k->k_key, res->tk, 16);
		err = (*ic->ic_set_key)(ic, ni, k);
		if (err != 0 && err != EBUSY)
			set_error("could not install the pairwise key");
		ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
		ni->ni_flags |= IEEE80211_NODE_RXPROT;
		printf("wlan: pairwise key installed\n");
	}
	if (res->install_gtk) {
		int i;

		k = &ic->ic_nw_keys[res->gtk_kid];
		memset(k, 0, sizeof(*k));
		k->k_id = (u_int8_t)res->gtk_kid;
		k->k_cipher = IEEE80211_CIPHER_CCMP;
		k->k_flags = IEEE80211_KEY_GROUP;
		if (res->gtk_tx)
			k->k_flags |= IEEE80211_KEY_TX;
		for (i = 0; i < IEEE80211_NUM_TID; i++)
			k->k_rsc[i] = res->gtk_rsc;
		k->k_len = res->gtk_len;
		memcpy(k->k_key, res->gtk, res->gtk_len);
		if ((*ic->ic_set_key)(ic, ni, k) != 0)
			set_error("could not install the group key");
		printf("wlan: group key %d installed\n", res->gtk_kid);
	}
	if (res->port_valid) {
		ni->ni_flags |= IEEE80211_NODE_TXRXPROT;
		if (!ni->ni_port_valid)
			printf("wlan: WPA2 handshake complete, link up\n");
		ni->ni_port_valid = 1;
		ni->ni_assoc_fail = 0;
		ni->ni_rsn_supp_state = RSNA_SUPP_PTKDONE;
		ic->ic_rsngroupcipher = ni->ni_rsngroupcipher;
		ieee80211_icda_last_error[0] = '\0';
		ieee80211_set_link_state(ic, LINK_STATE_UP);
	}
}

/*
 * EAPOL frames go out through the management queue, encapsulated right
 * away: message 4 must leave unencrypted, before the new PTK switches on
 * transmit protection.
 */
static void
ieee80211_send_eapol(struct ieee80211com *ic, struct ieee80211_node *ni,
    const uint8_t *frame, size_t len)
{
	struct ether_header *eh;
	struct ieee80211_node *tni;
	struct mbuf *m;

	m = m_get_lead(FRAME_LEAD, (u_int)(sizeof(*eh) + len));
	if (m == NULL)
		return;
	eh = mtod(m, struct ether_header *);
	IEEE80211_ADDR_COPY(eh->ether_dhost, ni->ni_macaddr);
	IEEE80211_ADDR_COPY(eh->ether_shost, ic->ic_myaddr);
	eh->ether_type = htons(ETHERTYPE_EAPOL);
	memcpy(eh + 1, frame, len);
	m = ieee80211_encap(&ic->ic_if, m, &tni);
	if (m == NULL)
		return;
	m->m_pkthdr.ph_cookie = tni;
	mq_enqueue(&ic->ic_mgtq, m);
	if_start(&ic->ic_if);
}

void
ieee80211_eapol_key_input(struct ieee80211com *ic, struct mbuf *m,
    struct ieee80211_node *ni)
{
	static struct wpa_result res;
	struct ether_header *eh = mtod(m, struct ether_header *);
	int rc;

	if (IEEE80211_IS_MULTICAST(eh->ether_dhost) ||
	    !(ic->ic_flags & IEEE80211_F_RSNON))
		goto done;
	m_adj(m, sizeof(*eh));
	ic->ic_stats.is_rx_eapol_key++;

	rc = wpa_sm_rx(&wpa_sm, mtod(m, const uint8_t *), (size_t)m->m_len,
	    &res);
	if (rc != 0) {
		set_error(res.error ? res.error : "EAPOL frame dropped");
		if (res.error && strncmp(res.error, "msg3: bad MIC", 13) == 0) {
			ic->ic_stats.is_rx_eapol_badmic++;
			ic->ic_stats.is_handshake_fail++;
		}
		goto done;
	}
	if (res.msg == 1)
		printf("wlan: 4-way handshake 1/4 received, sending 2/4\n");
	else if (res.msg == 3)
		printf("wlan: 4-way handshake 3/4 received, sending 4/4\n");
	else if (res.msg == 11)
		printf("wlan: group key handshake 1/2, sending 2/2\n");
	ni->ni_rsn_supp_state = wpa_sm.state;
	if (res.tx_len > 0)
		ieee80211_send_eapol(ic, ni, res.tx, res.tx_len);
	if (res.deauth_reason) {
		set_error(res.error ? res.error : "handshake failed");
		IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_DEAUTH,
		    res.deauth_reason);
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
		goto done;
	}
	ieee80211_install_keys(ic, ni, &res);
 done:
	explicit_bzero(&res, sizeof(res));
	m_freem(m);
}
