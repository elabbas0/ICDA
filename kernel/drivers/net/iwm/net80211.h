/*
 * ICDA port: a compact, station-only replacement for OpenBSD net80211.
 * It keeps the structure, field and function names that if_iwm.c uses.
 * Definitions are taken from OpenBSD's ieee80211_var.h, ieee80211_node.h,
 * ieee80211_proto.h and ieee80211_crypto.h, which carry these notices:
 *
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * Copyright (c) 2007, 2008 Damien Bergamini
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
 * Damien Bergamini's parts (crypto, RSN) are under the ISC license:
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
 *
 * ICDA port: legacy (11a/b/g) rates only; no HT/VHT, no aggregation, no
 * background scan, no hostap/IBSS/monitor.  Block-ack and HT rate adaptation
 * are stubs.  WPA2-PSK with CCMP; pairwise keys go to the hardware via
 * ic_set_key, group keys are handled in software.
 */

#ifndef ICDA_NET80211_H
#define ICDA_NET80211_H

#include "iwm_compat.h"
#ifndef _KERNEL
#define _KERNEL 1		/* ieee80211.h inline helpers */
#endif
#include "ieee80211.h"

#define LE_READ_2(p)	((u_int16_t)(((const u_int8_t *)(p))[0] |	\
			    ((const u_int8_t *)(p))[1] << 8))
#define LE_WRITE_2(p, v) do {					\
	((u_int8_t *)(p))[0] = (u_int8_t)(v);			\
	((u_int8_t *)(p))[1] = (u_int8_t)((v) >> 8);		\
} while (0)

/* ---- ieee80211_var.h ------------------------------------------------- */

#define	IEEE80211_CHAN_MAX	255
#define	IEEE80211_CHAN_ANY	0xffff
#define	IEEE80211_CHAN_ANYC \
	((struct ieee80211_channel *) IEEE80211_CHAN_ANY)

#define IEEE80211_RSSI_THRES_2GHZ		(-60)
#define IEEE80211_RSSI_THRES_5GHZ		(-70)
#define IEEE80211_RSSI_THRES_RATIO_2GHZ		60
#define IEEE80211_RSSI_THRES_RATIO_5GHZ		50
#define IEEE80211_BEACON_MISS_THRES		30

enum ieee80211_phytype {
	IEEE80211_T_DS,
	IEEE80211_T_OFDM,
	IEEE80211_T_XR
};
#define	IEEE80211_T_CCK	IEEE80211_T_DS

enum ieee80211_phymode {
	IEEE80211_MODE_AUTO	= 0,
	IEEE80211_MODE_11A	= 1,
	IEEE80211_MODE_11B	= 2,
	IEEE80211_MODE_11G	= 3,
	IEEE80211_MODE_11N	= 4,
	IEEE80211_MODE_11AC	= 5,
	IEEE80211_MODE_11AX	= 6,
};
#define	IEEE80211_MODE_MAX	(IEEE80211_MODE_11AX+1)

enum ieee80211_opmode {
	IEEE80211_M_STA		= 1,
	IEEE80211_M_IBSS	= 0,
	IEEE80211_M_AHDEMO	= 3,
	IEEE80211_M_HOSTAP	= 6,
	IEEE80211_M_MONITOR	= 8
};

struct ieee80211_channel {
	u_int16_t	ic_freq;	/* setting in MHz */
	u_int16_t	ic_flags;	/* see below */
	u_int32_t	ic_xflags;	/* see below */
};

#define IEEE80211_CHAN_CCK	0x0020
#define IEEE80211_CHAN_OFDM	0x0040
#define IEEE80211_CHAN_2GHZ	0x0080
#define IEEE80211_CHAN_5GHZ	0x0100
#define IEEE80211_CHAN_PASSIVE	0x0200
#define IEEE80211_CHAN_DYN	0x0400
#define IEEE80211_CHAN_XR	0x1000
#define IEEE80211_CHAN_HT	0x2000
#define IEEE80211_CHAN_VHT	0x4000
#define IEEE80211_CHAN_40MHZ	0x8000
#define IEEE80211_CHANX_80MHZ	0x00000001
#define IEEE80211_CHANX_160MHZ	0x00000002
#define IEEE80211_CHANX_HE	0x00000004

#define IEEE80211_CHAN_A	(IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM)
#define IEEE80211_CHAN_B	(IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_CCK)
#define IEEE80211_CHAN_PUREG	(IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_OFDM)
#define IEEE80211_CHAN_G	(IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_DYN)

#define	IEEE80211_IS_CHAN_A(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_A) == IEEE80211_CHAN_A)
#define	IEEE80211_IS_CHAN_B(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_B) == IEEE80211_CHAN_B)
#define	IEEE80211_IS_CHAN_PUREG(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_PUREG) == IEEE80211_CHAN_PUREG)
#define	IEEE80211_IS_CHAN_G(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_G) == IEEE80211_CHAN_G)
#define	IEEE80211_IS_CHAN_N(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_HT) == IEEE80211_CHAN_HT)
#define	IEEE80211_IS_CHAN_AC(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_VHT) == IEEE80211_CHAN_VHT)
#define	IEEE80211_IS_CHAN_2GHZ(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_2GHZ) != 0)
#define	IEEE80211_IS_CHAN_5GHZ(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_5GHZ) != 0)
#define	IEEE80211_IS_CHAN_OFDM(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_OFDM) != 0)
#define	IEEE80211_IS_CHAN_CCK(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_CCK) != 0)
#define	IEEE80211_CHAN_40MHZ_ALLOWED(_c) \
	(((_c)->ic_flags & IEEE80211_CHAN_40MHZ) != 0)
#define	IEEE80211_CHAN_80MHZ_ALLOWED(_c) \
	(((_c)->ic_xflags & IEEE80211_CHANX_80MHZ) != 0)
#define	IEEE80211_CHAN_160MHZ_ALLOWED(_c) \
	(((_c)->ic_xflags & IEEE80211_CHANX_160MHZ) != 0)
#define	IEEE80211_CHAN_HE(_c) \
	(((_c)->ic_xflags & IEEE80211_CHANX_HE) != 0)

struct ieee80211_edca_ac_params {
	u_int8_t	ac_ecwmin;
	u_int8_t	ac_ecwmax;
	u_int8_t	ac_aifsn;
	u_int16_t	ac_txoplimit;
	u_int8_t	ac_acm;
};

#define IEEE80211_TXOP_TO_US(txop)	((txop) * 32)

#define IEEE80211_PROTO_NONE	0
#define IEEE80211_PROTO_RSN	(1 << 0)
#define IEEE80211_PROTO_WPA	(1 << 1)

#define IEEE80211_GROUP_NKID	6

#define	IEEE80211_ADDR_EQ(a1,a2)	(memcmp(a1,a2,IEEE80211_ADDR_LEN) == 0)
#define	IEEE80211_ADDR_COPY(dst,src)	memcpy(dst,src,IEEE80211_ADDR_LEN)

#define	IEEE80211_F_ASCAN	0x00000001
#define	IEEE80211_F_SIBSS	0x00000002
#define	IEEE80211_F_WEPON	0x00000100
#define	IEEE80211_F_IBSSON	0x00000200
#define	IEEE80211_F_PMGTON	0x00000400
#define	IEEE80211_F_DESBSSID	0x00000800
#define	IEEE80211_F_ROAMING	0x00002000
#define	IEEE80211_F_SHSLOT	0x00020000
#define	IEEE80211_F_SHPREAMBLE	0x00040000
#define IEEE80211_F_QOS		0x00080000
#define	IEEE80211_F_USEPROT	0x00100000
#define	IEEE80211_F_RSNON	0x00200000
#define	IEEE80211_F_PSK		0x00400000
#define IEEE80211_F_COUNTERM	0x00800000
#define IEEE80211_F_MFPR	0x01000000
#define	IEEE80211_F_HTON	0x02000000
#define	IEEE80211_F_PBAR	0x04000000
#define	IEEE80211_F_BGSCAN	0x08000000
#define IEEE80211_F_AUTO_JOIN	0x10000000
#define	IEEE80211_F_VHTON	0x20000000
#define	IEEE80211_F_HEON	0x40000000

#define	IEEE80211_F_TX_MGMT_ONLY 0x00000001	/* ic_xflags */
#define IEEE80211_F_STAYAUTH	0x00000004	/* ic_userflags */
#define IEEE80211_F_NOMIMO	0x00000008	/* ic_userflags */

#define	IEEE80211_C_WEP		0x00000001
#define	IEEE80211_C_IBSS	0x00000002
#define	IEEE80211_C_PMGT	0x00000004
#define	IEEE80211_C_HOSTAP	0x00000008
#define	IEEE80211_C_AHDEMO	0x00000010
#define	IEEE80211_C_APPMGT	0x00000020
#define	IEEE80211_C_TXPMGT	0x00000040
#define	IEEE80211_C_SHSLOT	0x00000080
#define	IEEE80211_C_SHPREAMBLE	0x00000100
#define	IEEE80211_C_MONITOR	0x00000200
#define IEEE80211_C_SCANALL	0x00000400
#define IEEE80211_C_QOS		0x00000800
#define IEEE80211_C_RSN		0x00001000
#define IEEE80211_C_MFP		0x00002000
#define IEEE80211_C_RAWCTL	0x00004000
#define IEEE80211_C_SCANALLBAND	0x00008000
#define IEEE80211_C_TX_AMPDU	0x00010000
#define IEEE80211_C_ADDBA_OFFLOAD 0x00020000

#define	IEEE80211_F_DOSORT	0x00000001
#define	IEEE80211_F_DOFRATE	0x00000002
#define	IEEE80211_F_DONEGO	0x00000004
#define	IEEE80211_F_DODEL	0x00000008

/* ---- ieee80211_proto.h ----------------------------------------------- */

enum ieee80211_state {
	IEEE80211_S_INIT	= 0,
	IEEE80211_S_SCAN	= 1,
	IEEE80211_S_AUTH	= 2,
	IEEE80211_S_ASSOC	= 3,
	IEEE80211_S_RUN		= 4
};
#define	IEEE80211_S_MAX		(IEEE80211_S_RUN+1)

extern const char * const ieee80211_state_name[IEEE80211_S_MAX];
extern const char * const ieee80211_phymode_name[];

/* ---- ieee80211_crypto.h ---------------------------------------------- */

enum ieee80211_cipher {
	IEEE80211_CIPHER_NONE		= 0x00000000,
	IEEE80211_CIPHER_USEGROUP	= 0x00000001,
	IEEE80211_CIPHER_WEP40		= 0x00000002,
	IEEE80211_CIPHER_TKIP		= 0x00000004,
	IEEE80211_CIPHER_CCMP		= 0x00000008,
	IEEE80211_CIPHER_WEP104		= 0x00000010,
	IEEE80211_CIPHER_BIP		= 0x00000020
};

enum ieee80211_akm {
	IEEE80211_AKM_NONE		= 0x00000000,
	IEEE80211_AKM_8021X		= 0x00000001,
	IEEE80211_AKM_PSK		= 0x00000002,
	IEEE80211_AKM_SHA256_8021X	= 0x00000004,
	IEEE80211_AKM_SHA256_PSK	= 0x00000008,
	IEEE80211_AKM_SAE		= 0x00000010
};

#define IEEE80211_TKIP_HDRLEN	8
#define IEEE80211_TKIP_MICLEN	8
#define IEEE80211_TKIP_ICVLEN	4
#define IEEE80211_CCMP_HDRLEN	8
#define IEEE80211_CCMP_MICLEN	8
#define IEEE80211_PMK_LEN	32

struct ieee80211_key {
	u_int8_t		k_id;
	enum ieee80211_cipher	k_cipher;
	u_int			k_flags;
#define IEEE80211_KEY_GROUP	0x00000001
#define IEEE80211_KEY_TX	0x00000002
#define IEEE80211_KEY_IGTK	0x00000004
#define IEEE80211_KEY_SWCRYPTO	0x00000080
	u_int			k_len;
	u_int64_t		k_rsc[IEEE80211_NUM_TID];
	u_int64_t		k_mgmt_rsc;
	u_int64_t		k_tsc;
	u_int8_t		k_key[32];
	void			*k_priv;
};

static __inline int
ieee80211_is_8021x_akm(enum ieee80211_akm akm)
{
	return akm == IEEE80211_AKM_8021X ||
	    akm == IEEE80211_AKM_SHA256_8021X;
}

static __inline int
ieee80211_is_sha256_akm(enum ieee80211_akm akm)
{
	return akm == IEEE80211_AKM_SHA256_8021X ||
	    akm == IEEE80211_AKM_SHA256_PSK;
}

/* ---- ieee80211_node.h ------------------------------------------------ */

#define	IEEE80211_TRANS_WAIT	5	/* transition wait (watchdog seconds) */
#define	IEEE80211_INACT_SCAN	10
#define IEEE80211_NODE_MAX	64	/* scan table size */

struct ieee80211_rateset {
	u_int8_t	rs_nrates;
	u_int8_t	rs_rates[IEEE80211_RATE_MAXSIZE];
};

extern const struct ieee80211_rateset ieee80211_std_rateset_11a;
extern const struct ieee80211_rateset ieee80211_std_rateset_11b;
extern const struct ieee80211_rateset ieee80211_std_rateset_11g;

/* HT rate sets (rate adaptation stubs only) */
struct ieee80211_ht_rateset {
	uint32_t	nrates;
	uint32_t	rates[IEEE80211_HT_NUM_MCS];
	uint32_t	mcs_mask;
	int		min_mcs;
	int		max_mcs;
	int		chan40;
	int		sgi;
};

enum ieee80211_node_state {
	IEEE80211_STA_CACHE,
	IEEE80211_STA_BSS,
	IEEE80211_STA_AUTH,
	IEEE80211_STA_ASSOC,
	IEEE80211_STA_COLLECT
};

enum {
	RSNA_SUPP_INITIALIZE,
	RSNA_SUPP_PTKSTART,
	RSNA_SUPP_PTKNEGOTIATING,
	RSNA_SUPP_PTKDONE
};

struct ieee80211_rxinfo {
	u_int32_t		rxi_flags;
	u_int32_t		rxi_tstamp;
	int			rxi_rssi;
	uint8_t			rxi_chan;
};
#define IEEE80211_RXI_HWDEC		0x00000001
#define IEEE80211_RXI_AMPDU_DONE	0x00000002
#define IEEE80211_RXI_HWDEC_SAME_PN	0x00000004
#define IEEE80211_RXI_SAME_SEQ		0x00000008

#define IEEE80211_BA_INIT	0
#define IEEE80211_BA_REQUESTED	1
#define IEEE80211_BA_AGREED	2
#define IEEE80211_BA_MAX_WINSZ	64

struct ieee80211_tx_ba {
	struct ieee80211_node	*ba_ni;
	int			 ba_state;
	u_int16_t		 ba_winstart;
	u_int16_t		 ba_winend;
	u_int16_t		 ba_winsize;
	u_int16_t		 ba_timeout_val;
	u_int8_t		 ba_token;
	u_int16_t		 ba_params;
};

struct ieee80211_rx_ba {
	int			 ba_state;
	u_int16_t		 ba_winstart;
	u_int16_t		 ba_winsize;
};

#define IEEE80211_NODE_ERP		0x0001
#define IEEE80211_NODE_QOS		0x0002
#define IEEE80211_NODE_REKEY		0x0004
#define IEEE80211_NODE_RXPROT		0x0008
#define IEEE80211_NODE_TXPROT		0x0010
#define IEEE80211_NODE_TXRXPROT	\
	(IEEE80211_NODE_TXPROT | IEEE80211_NODE_RXPROT)
#define IEEE80211_NODE_RXMGMTPROT	0x0020
#define IEEE80211_NODE_TXMGMTPROT	0x0040
#define IEEE80211_NODE_MFP		0x0080
#define IEEE80211_NODE_PMK		0x0100
#define IEEE80211_NODE_PMKID		0x0200
#define IEEE80211_NODE_HT		0x0400
#define IEEE80211_NODE_SA_QUERY		0x0800
#define IEEE80211_NODE_SA_QUERY_FAILED	0x1000
#define IEEE80211_NODE_RSN_NEW_PTK	0x2000
#define IEEE80211_NODE_HT_SGI20		0x4000
#define IEEE80211_NODE_HT_SGI40		0x8000
#define IEEE80211_NODE_VHT		0x10000
#define IEEE80211_NODE_HTCAP		0x20000
#define IEEE80211_NODE_VHTCAP		0x40000
#define IEEE80211_NODE_VHT_SGI80	0x80000
#define IEEE80211_NODE_VHT_SGI160	0x100000
#define IEEE80211_NODE_HE		0x200000
#define IEEE80211_NODE_HECAP		0x400000
#define IEEE80211_NODE_CSA		0x800000
#define IEEE80211_NODE_UAPSD		0x1000000

#define IEEE80211_NODE_ASSOCFAIL_CHAN		0x01
#define IEEE80211_NODE_ASSOCFAIL_IBSS		0x02
#define IEEE80211_NODE_ASSOCFAIL_PRIVACY	0x04
#define IEEE80211_NODE_ASSOCFAIL_BASIC_RATE	0x08
#define IEEE80211_NODE_ASSOCFAIL_ESSID		0x10
#define IEEE80211_NODE_ASSOCFAIL_BSSID		0x20
#define IEEE80211_NODE_ASSOCFAIL_WPA_PROTO	0x40
#define IEEE80211_NODE_ASSOCFAIL_WPA_KEY	0x80
#define IEEE80211_NODE_ASSOCFAIL_CSA		0x100

struct ieee80211com;

struct ieee80211_node {
	struct ieee80211com	*ni_ic;
	u_int8_t		 ni_macaddr[IEEE80211_ADDR_LEN];
	u_int8_t		 ni_bssid[IEEE80211_ADDR_LEN];
	u_int8_t		 ni_esslen;
	u_int8_t		 ni_essid[IEEE80211_NWID_LEN];
	struct ieee80211_channel *ni_chan;
	struct ieee80211_rateset ni_rates;
	u_int8_t		 ni_tstamp[8];
	u_int16_t		 ni_intval;
	u_int16_t		 ni_capinfo;
	u_int8_t		 ni_erp;
	u_int8_t		 ni_dtimcount;
	u_int8_t		 ni_dtimperiod;
	u_int8_t		 ni_rssi;
	u_int32_t		 ni_rstamp;
	u_int16_t		 ni_associd;
	u_int16_t		 ni_txseq;
	u_int16_t		 ni_rxseq;
	u_int16_t		 ni_qos_txseqs[IEEE80211_NUM_TID];
	u_int16_t		 ni_qos_rxseqs[IEEE80211_NUM_TID];
	int			 ni_txrate;
	int			 ni_fails;
	int			 ni_inact;
	int			 ni_state;
	u_int			 ni_refcnt;
	u_int32_t		 ni_flags;
	u_int32_t		 ni_assoc_fail;

	/* RSN */
	u_int8_t		*ni_rsnie;
	u_int			 ni_rsnprotos;
	u_int			 ni_supported_rsnprotos;
	u_int			 ni_rsnakms;
	u_int			 ni_supported_rsnakms;
	u_int			 ni_rsnciphers;
	enum ieee80211_cipher	 ni_rsncipher;
	enum ieee80211_cipher	 ni_rsngroupcipher;
	enum ieee80211_cipher	 ni_rsngroupmgmtcipher;
	u_int16_t		 ni_rsncaps;
	int			 ni_rsn_supp_state;
	u_int8_t		 ni_pmk[IEEE80211_PMK_LEN];
	u_int8_t		 ni_pmkid[IEEE80211_PMKID_LEN];
	u_int64_t		 ni_replaycnt;
	int			 ni_replaycnt_ok;
	u_int8_t		 ni_nonce[EAPOL_KEY_NONCE_LEN];
	struct ieee80211_ptk	 ni_ptk;
	struct ieee80211_key	 ni_pairwise_key;
	int			 ni_port_valid;

	/* HT/VHT (never negotiated in this port) */
	u_int16_t		 ni_htcaps;
	u_int8_t		 ni_ampdu_param;
	u_int8_t		 ni_rxmcs[howmany(80, NBBY)];
	u_int16_t		 ni_max_rxrate;
	u_int8_t		 ni_tx_mcs_set;
	u_int8_t		 ni_htop0;
	u_int16_t		 ni_htop1;
	u_int16_t		 ni_htop2;
	int			 ni_txmcs;
	u_int32_t		 ni_vhtcaps;
	u_int16_t		 ni_vht_rxmcs;
	u_int16_t		 ni_vht_txmcs;
	int			 ni_vht_ss;

	struct ieee80211_tx_ba	 ni_tx_ba[IEEE80211_NUM_TID];
	struct ieee80211_rx_ba	 ni_rx_ba[IEEE80211_NUM_TID];
};

struct ieee80211_rsnparams {
	u_int16_t		rsn_nakms;
	u_int32_t		rsn_akms;
	u_int16_t		rsn_nciphers;
	u_int32_t		rsn_ciphers;
	enum ieee80211_cipher	rsn_groupcipher;
	enum ieee80211_cipher	rsn_groupmgmtcipher;
	u_int16_t		rsn_caps;
	u_int8_t		rsn_npmkids;
	const u_int8_t		*rsn_pmkids;
};

#define IEEE80211_RSNIE_MAXLEN	(2 + 4 + 2 + 4 * 3 + 2 + 4 * 3 + 2 + 2 + 16 + 4)
#define IEEE80211_WPAIE_MAXLEN	(4 + IEEE80211_RSNIE_MAXLEN)

/* ---- rate adaptation -------------------------------------------------- */

/* AMRR (legacy rates): implemented in net80211.c */
struct ieee80211_amrr {
	u_int	amrr_min_success_threshold;
	u_int	amrr_max_success_threshold;
};

struct ieee80211_amrr_node {
	int	amn_success;
	int	amn_recovery;
	u_int	amn_success_threshold;
	u_int	amn_txcnt;
	u_int	amn_retrycnt;
};

/* HT/VHT rate adaptation: stubs (HT is not negotiated in this port) */
struct ieee80211_ra_node {
	int	ra_dummy;
};
struct ieee80211_ra_vht_node {
	int	ra_dummy;
};

void	ieee80211_amrr_node_init(const struct ieee80211_amrr *,
	    struct ieee80211_amrr_node *);
void	ieee80211_amrr_choose(struct ieee80211_amrr *,
	    struct ieee80211_node *, struct ieee80211_amrr_node *);

#define ieee80211_ra_node_init(rn)		((void)(rn))
#define ieee80211_ra_vht_node_init(rn)		((void)(rn))
#define ieee80211_ra_choose(rn, ic, ni)		((void)(rn))
#define ieee80211_ra_vht_choose(rn, ic, ni)	((void)(rn))
#define ieee80211_ra_add_stats_ht(rn, ic, ni, mcs, total, fail) \
	((void)(rn))
#define ieee80211_ra_use_ht_sgi(ni)		0
const struct ieee80211_ht_rateset *ieee80211_ra_get_ht_rateset(int, int, int);

/* ---- media (fixed to auto) -------------------------------------------- */

#define IFM_AUTO		0
#define IFM_MODE(x)		((x) & 0)
#define IFM_IEEE80211_11A	1
#define IFM_IEEE80211_11B	2
#define IFM_IEEE80211_11G	3

struct ifmedia_entry {
	uint64_t	ifm_media;
};

struct ifmedia {
	struct ifmedia_entry	*ifm_cur;
	struct ifmedia_entry	 ifm_auto;
};

struct ifmediareq;
void	ieee80211_media_init(struct ifnet *, int (*)(struct ifnet *),
	    void (*)(struct ifnet *, struct ifmediareq *));
void	ieee80211_media_status(struct ifnet *, struct ifmediareq *);

/* ---- statistics ------------------------------------------------------- */

struct ieee80211_stats {
	u_int32_t	is_rx_badversion;
	u_int32_t	is_rx_tooshort;
	u_int32_t	is_rx_wrongbss;
	u_int32_t	is_rx_dup;
	u_int32_t	is_rx_wrongdir;
	u_int32_t	is_rx_mcastecho;
	u_int32_t	is_rx_notassoc;
	u_int32_t	is_rx_nowep;
	u_int32_t	is_rx_unencrypted;
	u_int32_t	is_rx_wepfail;
	u_int32_t	is_rx_decap;
	u_int32_t	is_rx_mgtdiscard;
	u_int32_t	is_rx_ctl;
	u_int32_t	is_rx_rstoobig;
	u_int32_t	is_rx_elem_toosmall;
	u_int32_t	is_rx_badchan;
	u_int32_t	is_rx_chanmismatch;
	u_int32_t	is_rx_nodealloc;
	u_int32_t	is_rx_auth_fail;
	u_int32_t	is_rx_auth_unsupported;
	u_int32_t	is_rx_bad_auth;
	u_int32_t	is_rx_assoc_norate;
	u_int32_t	is_rx_deauth;
	u_int32_t	is_rx_disassoc;
	u_int32_t	is_rx_badsubtype;
	u_int32_t	is_rx_nombuf;
	u_int32_t	is_rx_unauth;
	u_int32_t	is_rx_eapol_key;
	u_int32_t	is_rx_eapol_replay;
	u_int32_t	is_rx_eapol_badmic;
	u_int32_t	is_tx_nombuf;
	u_int32_t	is_tx_nonode;
	u_int32_t	is_tx_unknownmgt;
	u_int32_t	is_tx_noauth;
	u_int32_t	is_scan_active;
	u_int32_t	is_scan_passive;
	u_int32_t	is_ccmp_dec_errs;
	u_int32_t	is_ccmp_replays;
	u_int32_t	is_ht_prot_change;
};

/* ---- ieee80211com ----------------------------------------------------- */

struct ieee80211com {
	struct arpcom		 ic_ac;
	int			 (*ic_send_mgmt)(struct ieee80211com *,
				    struct ieee80211_node *, int, int, int);
	int			 (*ic_newstate)(struct ieee80211com *,
				    enum ieee80211_state, int);
	void			 (*ic_newassoc)(struct ieee80211com *,
				    struct ieee80211_node *, int);
	void			 (*ic_node_leave)(struct ieee80211com *,
				    struct ieee80211_node *);
	void			 (*ic_updateslot)(struct ieee80211com *);
	void			 (*ic_updateedca)(struct ieee80211com *);
	void			 (*ic_updateprot)(struct ieee80211com *);
	void			 (*ic_updatechan)(struct ieee80211com *);
	void			 (*ic_updatedtim)(struct ieee80211com *);
	int			 (*ic_set_key)(struct ieee80211com *,
				    struct ieee80211_node *,
				    struct ieee80211_key *);
	void			 (*ic_delete_key)(struct ieee80211com *,
				    struct ieee80211_node *,
				    struct ieee80211_key *);
	int			 (*ic_ampdu_rx_start)(struct ieee80211com *,
				    struct ieee80211_node *, u_int8_t);
	void			 (*ic_ampdu_rx_stop)(struct ieee80211com *,
				    struct ieee80211_node *, u_int8_t);
	int			 (*ic_ampdu_tx_start)(struct ieee80211com *,
				    struct ieee80211_node *, u_int8_t);
	void			 (*ic_ampdu_tx_stop)(struct ieee80211com *,
				    struct ieee80211_node *, u_int8_t);
	void			 (*ic_bgscan_done)(struct ieee80211com *,
				    void *, size_t);
	int			 (*ic_bgscan_start)(struct ieee80211com *);
	struct ieee80211_node	*(*ic_node_alloc)(struct ieee80211com *);
	void			 (*ic_node_free)(struct ieee80211com *,
				    struct ieee80211_node *);
	void			 (*ic_node_copy)(struct ieee80211com *,
				    struct ieee80211_node *,
				    const struct ieee80211_node *);
	int			 (*ic_node_checkrssi)(struct ieee80211com *,
				    const struct ieee80211_node *);
	struct mbuf_queue	 ic_mgtq;
	struct mbuf_queue	 ic_pwrsaveq;
	u_int8_t		 ic_myaddr[IEEE80211_ADDR_LEN];
	struct ieee80211_rateset ic_sup_rates[IEEE80211_MODE_MAX];
	struct ieee80211_channel ic_channels[IEEE80211_CHAN_MAX+1];
	u_char			 ic_chan_avail[howmany(IEEE80211_CHAN_MAX+1, NBBY)];
	u_char			 ic_chan_active[howmany(IEEE80211_CHAN_MAX+1, NBBY)];
	u_char			 ic_chan_scan[howmany(IEEE80211_CHAN_MAX+1, NBBY)];
	struct ieee80211_node	*ic_nodes[IEEE80211_NODE_MAX];
	int			 ic_nnodes;
	struct ifmedia		 ic_media;
	u_int32_t		 ic_flags;
	u_int32_t		 ic_xflags;
	u_int32_t		 ic_userflags;
	u_int32_t		 ic_caps;
	u_int16_t		 ic_modecaps;
	u_int16_t		 ic_curmode;
	enum ieee80211_phytype	 ic_phytype;
	enum ieee80211_opmode	 ic_opmode;
	enum ieee80211_state	 ic_state;
	u_int32_t		*ic_aid_bitmap;
	struct ieee80211_node	*ic_bss;
	struct ieee80211_channel *ic_ibss_chan;
	struct ieee80211_channel *ic_des_chan;
	int			 ic_fixed_rate;
	int			 ic_fixed_mcs;
	u_int16_t		 ic_lintval;
	u_int16_t		 ic_bmisstimeout;
	u_int16_t		 ic_bmissthres;
	int			 ic_mgt_timer;
	int			 ic_scan_count;
	u_int16_t		 ic_rtsthreshold;
	u_int16_t		 ic_fragthreshold;
	u_int8_t		 ic_dtim_period;
	u_int8_t		 ic_max_rssi;
	struct ieee80211_stats	 ic_stats;
	u_int8_t		 ic_des_esslen;
	u_int8_t		 ic_des_essid[IEEE80211_NWID_LEN];
	u_int8_t		 ic_des_bssid[IEEE80211_ADDR_LEN];
	struct ieee80211_key	 ic_nw_keys[IEEE80211_GROUP_NKID];
	int			 ic_def_txkey;
	int			 ic_igtk_kid;
	u_int32_t		 ic_rsnprotos;
	u_int32_t		 ic_rsnakms;
	u_int32_t		 ic_rsnciphers;
	enum ieee80211_cipher	 ic_rsngroupcipher;
	enum ieee80211_cipher	 ic_rsngroupmgmtcipher;
	u_int8_t		 ic_psk[IEEE80211_PMK_LEN];
	u_int8_t		 ic_nonce[EAPOL_KEY_NONCE_LEN];
	struct ieee80211_edca_ac_params ic_edca_ac[EDCA_NUM_AC];
	u_int			 ic_edca_updtcount;
	u_int16_t		 ic_tid_noack;
	u_int8_t		 ic_uapsd_ac;
	/* HT/VHT capabilities (advertised in probe requests only) */
	u_int16_t		 ic_htcaps;
	u_int8_t		 ic_htxcaps;
	u_int32_t		 ic_txbfcaps;
	u_int8_t		 ic_aselcaps;
	u_int8_t		 ic_ampdu_params;
	u_int8_t		 ic_sup_mcs[howmany(80, NBBY)];
	u_int16_t		 ic_max_rxrate;
	u_int8_t		 ic_tx_mcs_set;
	u_int32_t		 ic_vhtcaps;
	u_int16_t		 ic_vht_rxmcs;
	u_int16_t		 ic_vht_txmcs;
	u_int16_t		 ic_vht_rx_max_lgi_mbit_s;
	u_int16_t		 ic_vht_tx_max_lgi_mbit_s;
	void			*ic_rawbpf;
	/* ICDA: incremented after every scan round; for UI polling */
	u_int32_t		 ic_scan_gen;
	/* ICDA: 1 while the user asked for a scan without a target SSID */
	int			 ic_scan_only;
};
#define	ic_if		ic_ac.ac_if
#define	ic_softc	ic_if.if_softc

/* radiotap (bpf is not built; only the header type is needed) */
struct ieee80211_radiotap_header {
	u_int8_t	it_version;
	u_int8_t	it_pad;
	u_int16_t	it_len;
	u_int32_t	it_present;
} __packed;
#define IEEE80211_RADIOTAP_HDRLEN	64

/* ---- functions -------------------------------------------------------- */

#define IEEE80211_SEND_MGMT(_ic,_ni,_type,_arg) \
	((*(_ic)->ic_send_mgmt)(_ic, _ni, _type, _arg, 0))
#define IEEE80211_SEND_ACTION(_ic,_ni,_categ,_action,_arg) \
	((void)0)

void	ieee80211_ifattach(struct ifnet *);
void	ieee80211_channel_init(struct ifnet *);
u_int	ieee80211_mhz2ieee(u_int, u_int);
u_int	ieee80211_chan2ieee(struct ieee80211com *,
	    const struct ieee80211_channel *);
u_int	ieee80211_ieee2mhz(u_int, u_int);
int	ieee80211_setmode(struct ieee80211com *, enum ieee80211_phymode);
int	ieee80211_min_basic_rate(struct ieee80211com *);
int	ieee80211_max_basic_rate(struct ieee80211com *);
void	ieee80211_watchdog(struct ifnet *);
int	ieee80211_fix_rate(struct ieee80211com *, struct ieee80211_node *,
	    int);
enum ieee80211_phymode ieee80211_node_abg_mode(struct ieee80211com *,
	    struct ieee80211_node *);

int	ieee80211_new_state(struct ieee80211com *, enum ieee80211_state, int);
int	ieee80211_newstate(struct ieee80211com *, enum ieee80211_state, int);
void	ieee80211_set_link_state(struct ieee80211com *, int);
void	ieee80211_begin_scan(struct ifnet *);
void	ieee80211_end_scan(struct ifnet *);

struct ieee80211_node *ieee80211_alloc_node(struct ieee80211com *,
	    const u_int8_t *);
struct ieee80211_node *ieee80211_find_node(struct ieee80211com *,
	    const u_int8_t *);
struct ieee80211_node *ieee80211_find_rxnode(struct ieee80211com *,
	    const struct ieee80211_frame *);
struct ieee80211_node *ieee80211_find_txnode(struct ieee80211com *,
	    const u_int8_t *);
void	ieee80211_release_node(struct ieee80211com *,
	    struct ieee80211_node *);
void	ieee80211_free_allnodes(struct ieee80211com *, int);
void	ieee80211_node_cleanup(struct ieee80211com *, struct ieee80211_node *);
void	ieee80211_ba_del(struct ieee80211_node *);

static __inline struct ieee80211_node *
ieee80211_ref_node(struct ieee80211_node *ni)
{
	ni->ni_refcnt++;
	return ni;
}

/* HT/VHT capability queries: nothing is negotiated in this port. */
#define ieee80211_node_supports_ht_chan40(ni)	0
#define ieee80211_node_supports_vht_chan80(ni)	0
#define ieee80211_node_supports_ht_sgi20(ni)	0
#define ieee80211_node_supports_ht_sgi40(ni)	0
#define ieee80211_node_supports_vht_sgi80(ni)	0

void	ieee80211_inputm(struct ifnet *, struct mbuf *,
	    struct ieee80211_node *, struct ieee80211_rxinfo *,
	    struct mbuf_list *);
u_int	ieee80211_get_hdrlen(const struct ieee80211_frame *);
struct mbuf *ieee80211_encap(struct ifnet *, struct mbuf *,
	    struct ieee80211_node **);
enum ieee80211_edca_ac ieee80211_up_to_ac(struct ieee80211com *, int);
u_int8_t *ieee80211_add_rates(u_int8_t *, const struct ieee80211_rateset *);
u_int8_t *ieee80211_add_xrates(u_int8_t *, const struct ieee80211_rateset *);
u_int8_t *ieee80211_add_htcaps(u_int8_t *, struct ieee80211com *);
u_int8_t *ieee80211_add_vhtcaps(u_int8_t *, struct ieee80211com *);
int	ieee80211_send_mgmt(struct ieee80211com *, struct ieee80211_node *,
	    int, int, int);

/* crypto */
int	ieee80211_cipher_keylen(enum ieee80211_cipher);
struct ieee80211_key *ieee80211_get_txkey(struct ieee80211com *,
	    const struct ieee80211_frame *, struct ieee80211_node *);
struct mbuf *ieee80211_encrypt(struct ieee80211com *, struct mbuf *,
	    struct ieee80211_key *);
struct mbuf *ieee80211_decrypt(struct ieee80211com *, struct mbuf *,
	    struct ieee80211_node *);
int	ieee80211_set_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	ieee80211_delete_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	ieee80211_crypto_clear_groupkeys(struct ieee80211com *);
void	ieee80211_eapol_key_input(struct ieee80211com *, struct mbuf *,
	    struct ieee80211_node *);

/* ICDA: station configuration used by wifi.c */
void	ieee80211_icda_configure(struct ieee80211com *, const char *ssid,
	    size_t ssidlen, const u_int8_t *pmk /* NULL: open network */);

#endif /* ICDA_NET80211_H */
