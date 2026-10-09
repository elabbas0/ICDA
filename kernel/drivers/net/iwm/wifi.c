/*
 * ICDA glue for the iwm(4) port: PCI probe of the Intel Wireless 8260,
 * the driver thread, frame rings for the network stack, /dev/wifi and
 * saved networks.
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
 *
 * Threads and contexts:
 *  - The "wifi" kernel thread owns the driver.  It runs on a private
 *    128 KiB stack (kernel threads get 8 KiB, too little for iwm at -O0),
 *    polls iwm_intr(), runs tasks/timeouts, moves frames and executes
 *    requests from /dev/wifi.
 *  - wifi_send_frame()/wifi_recv_frame() are called by the network stack,
 *    also from the timer interrupt; they only touch the frame rings.
 *  - The "wifi-dhcp" thread re-runs DHCP whenever the link comes up (DHCP
 *    waits for frames that the wifi thread delivers, so it cannot run on
 *    the wifi thread itself).
 */

#include "iwm_compat.h"
#include "net80211.h"

/* as in if_iwm.c; used by inline helpers in if_iwmreg.h */
#define le16_to_cpup(_a_) (le16toh(*(const uint16_t *)(_a_)))
#define le32_to_cpup(_a_) (le32toh(*(const uint32_t *)(_a_)))

#include "if_iwmreg.h"
#include "if_iwmvar.h"
#include "iwm_port.h"
#include "wpa_crypto.h"
#include "wifi.h"

#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "proc/sched.h"
#include "fs/vfs.h"
#include "net/net.h"

#define WIFI_VENDOR_INTEL	0x8086
#define WIFI_DEVICE_8260	0x24f3

#define WIFI_SAVED_PATH		"/home/.wifi-networks"
#define WIFI_STACK_PAGES	32
#define FRAME_CAP		1600
#define RX_SLOTS		128
#define TX_SLOTS		64
#define SCAN_MAX		48

extern char ieee80211_icda_last_error[];
extern int net_reconfigure(void);
extern void net_link_down(void);

/* ---- state ------------------------------------------------------------ */

enum wifi_phase {
	WP_ABSENT,	/* no supported card */
	WP_STARTING,	/* thread is bringing the card up */
	WP_RUNNING,	/* firmware running */
	WP_ERROR,	/* bring-up failed; "restart" retries */
	WP_RFKILL	/* radio disabled by the kill switch */
};

static const pci_device_t	*wifi_pci;
static struct iwm_softc		*sc;
static volatile int		 phase = WP_ABSENT;
static uint8_t			 wifi_macaddr[6];
static char			 phase_error[96];

/* requests from /dev/wifi, executed by the wifi thread */
enum { REQ_NONE, REQ_SCAN, REQ_CONNECT, REQ_DISCONNECT, REQ_FORGET,
    REQ_RESTART };
static volatile int	req;
static char		req_ssid[IEEE80211_NWID_LEN + 1];
static char		req_pass[72];

/* connection bookkeeping (wifi thread) */
static char		target_ssid[IEEE80211_NWID_LEN + 1];
static uint8_t		target_pmk[32];
static int		target_secure;
static int		target_saved;	/* save on success */
static int		autoconnect = 1;
static uint32_t		badmic_base;
static uint32_t		last_scan_gen;
static int		link_up;

static int	ic_is_running(void);

/* DHCP */
static volatile int	dhcp_request;
static volatile int	dhcp_status;	/* 0 none, 1 running, 2 ok, 3 failed */

/* snapshots for readers in other contexts */
struct scan_entry {
	char		ssid[IEEE80211_NWID_LEN + 1];
	uint8_t		bssid[6];
	uint8_t		chan;
	uint8_t		pct;
	int		dbm;
	char		sec[12];
	int		saved;
};
static struct scan_entry scan_snap[SCAN_MAX];
static int		scan_snap_n;
static uint32_t		scan_snap_gen;
static char		status_snap[1024];

enum { VIEW_ALL, VIEW_STATUS, VIEW_SCAN, VIEW_LOG, VIEW_SAVED };
static volatile int	view = VIEW_ALL;

/* ---- interrupt-safe frame rings --------------------------------------- */

struct frame_slot {
	uint16_t	len;
	uint8_t		data[FRAME_CAP];
};

static struct frame_slot rx_ring[RX_SLOTS];
static struct frame_slot tx_ring[TX_SLOTS];
static volatile uint32_t rx_head, rx_tail, tx_head, tx_tail;
static uint64_t rx_drops, tx_drops;

static inline uint64_t
irq_save(void)
{
	uint64_t f;

	__asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
	return f;
}

static inline void
irq_restore(uint64_t f)
{
	__asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

int
wifi_present(void)
{
	return wifi_pci != NULL;
}

int
wifi_ready(void)
{
	return phase == WP_RUNNING && link_up;
}

int
wifi_mac(uint8_t out[6])
{
	if (phase != WP_RUNNING || out == NULL)
		return -1;
	memcpy(out, wifi_macaddr, 6);
	return 0;
}

int
wifi_send_frame(const void *data, uint16_t len)
{
	uint64_t f;
	int rc = -1;

	if (!wifi_ready() || data == NULL || len == 0 || len > FRAME_CAP)
		return -1;
	f = irq_save();
	if (tx_head - tx_tail < TX_SLOTS) {
		struct frame_slot *s = &tx_ring[tx_head % TX_SLOTS];
		memcpy(s->data, data, len);
		s->len = len;
		tx_head++;
		rc = 0;
	} else
		tx_drops++;
	irq_restore(f);
	return rc;
}

int
wifi_recv_frame(void *data, uint16_t cap, uint16_t *len_out)
{
	uint64_t f;
	int rc = 0;

	if (data == NULL || len_out == NULL)
		return -1;
	f = irq_save();
	if (rx_tail != rx_head) {
		struct frame_slot *s = &rx_ring[rx_tail % RX_SLOTS];
		uint16_t n = s->len < cap ? s->len : cap;
		memcpy(data, s->data, n);
		*len_out = n;
		rx_tail++;
		rc = 1;
	}
	irq_restore(f);
	return rc;
}

/* if_input(): a decapsulated Ethernet frame from net80211. */
static void
wifi_if_input(struct ifnet *ifp, struct mbuf *m)
{
	uint64_t f;

	(void)ifp;
	if (m->m_len > 0 && m->m_len <= FRAME_CAP) {
		f = irq_save();
		if (rx_head - rx_tail < RX_SLOTS) {
			struct frame_slot *s = &rx_ring[rx_head % RX_SLOTS];
			memcpy(s->data, m->m_data, (size_t)m->m_len);
			s->len = (uint16_t)m->m_len;
			rx_head++;
		} else
			rx_drops++;
		irq_restore(f);
	}
	m_freem(m);
}

/* Move frames queued by the network stack onto the interface queue. */
static int
wifi_drain_tx(struct ifnet *ifp)
{
	int n = 0;

	for (;;) {
		struct mbuf *m;
		uint8_t buf[FRAME_CAP];
		uint16_t len;
		uint64_t f = irq_save();

		if (tx_tail == tx_head) {
			irq_restore(f);
			break;
		}
		len = tx_ring[tx_tail % TX_SLOTS].len;
		memcpy(buf, tx_ring[tx_tail % TX_SLOTS].data, len);
		tx_tail++;
		irq_restore(f);

		m = m_get_lead(128, len);
		if (m == NULL) {
			tx_drops++;
			continue;
		}
		memcpy(m->m_data, buf, len);
		if (ifq_enqueue(&ifp->if_snd, m) != 0)
			tx_drops++;
		n++;
	}
	if (n)
		ifp->if_start_pending = 1;
	return n;
}

/* ---- saved networks ---------------------------------------------------- */

static int
hexdigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * File format, one network per line: <ssid> TAB <64 hex digits of the PSK>
 * (the PSK derived from the passphrase; the passphrase itself is not kept)
 * or <ssid> TAB "open".
 */
static int
saved_lookup(const char *ssid, uint8_t pmk[32], int *secure)
{
	uint64_t size = 0;
	const char *data = vfs_read(vfs_root(), WIFI_SAVED_PATH, &size);
	size_t sl = strlen(ssid);
	uint64_t i = 0;

	if (data == NULL)
		return 0;
	while (i < size) {
		uint64_t s = i, tab, e;
		while (i < size && data[i] != '\n')
			i++;
		e = i++;
		for (tab = s; tab < e && data[tab] != '\t'; tab++)
			;
		if (tab >= e || tab - s != sl || memcmp(data + s, ssid, sl) != 0)
			continue;
		if (e - tab - 1 == 4 && memcmp(data + tab + 1, "open", 4) == 0) {
			*secure = 0;
			return 1;
		}
		if (e - tab - 1 != 64)
			continue;
		for (int k = 0; k < 32; k++) {
			int hi = hexdigit(data[tab + 1 + 2 * k]);
			int lo = hexdigit(data[tab + 2 + 2 * k]);
			if (hi < 0 || lo < 0)
				return 0;
			pmk[k] = (uint8_t)(hi << 4 | lo);
		}
		*secure = 1;
		return 1;
	}
	return 0;
}

/* Rewrite the file without `ssid' and, if add, with it appended. */
static void
saved_update(const char *ssid, const uint8_t *pmk, int secure, int add)
{
	uint64_t size = 0, i = 0, o = 0;
	const char *data = vfs_read(vfs_root(), WIFI_SAVED_PATH, &size);
	size_t sl = strlen(ssid);
	char *out = kmalloc((size_t)size + 128);

	if (out == NULL)
		return;
	while (data != NULL && i < size) {
		uint64_t s = i, e;
		while (i < size && data[i] != '\n')
			i++;
		e = i++;
		if (e - s > sl && data[s + sl] == '\t' &&
		    memcmp(data + s, ssid, sl) == 0)
			continue;
		if (e > s) {
			memcpy(out + o, data + s, e - s);
			o += e - s;
			out[o++] = '\n';
		}
	}
	if (add) {
		memcpy(out + o, ssid, sl);
		o += sl;
		out[o++] = '\t';
		if (secure) {
			for (int k = 0; k < 32; k++) {
				snprintf(out + o, 3, "%02x", pmk[k]);
				o += 2;
			}
		} else {
			memcpy(out + o, "open", 4);
			o += 4;
		}
		out[o++] = '\n';
	}
	(void)vfs_mkdir(vfs_root(), "/home");
	if (vfs_write(vfs_root(), WIFI_SAVED_PATH, out, o) != 0)
		printf("wifi: could not write %s\n", WIFI_SAVED_PATH);
	explicit_bzero(out, (size_t)o);
	kfree(out);
}

/* ---- snapshots --------------------------------------------------------- */

static int
node_dbm(const struct ieee80211_node *ni)
{
	return (int)ni->ni_rssi + IWM_MIN_DBM;
}

static const char *
node_security(const struct ieee80211_node *ni)
{
	if (!(ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY))
		return "open";
	if (ni->ni_rsnprotos & IEEE80211_PROTO_RSN) {
		if (ni->ni_rsnakms & IEEE80211_AKM_PSK)
			return (ni->ni_rsncaps & IEEE80211_RSNCAP_MFPR) ?
			    "WPA3" :
			    ni->ni_rsngroupcipher != IEEE80211_CIPHER_CCMP ?
			    "WPA/WPA2" : "WPA2";
		if (ni->ni_rsnakms & IEEE80211_AKM_SAE)
			return "WPA3";
		if (ni->ni_rsnakms & IEEE80211_AKM_8021X)
			return "802.1X";
		return "RSN?";
	}
	return "WEP/WPA1";
}

static void
update_scan_snapshot(void)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct scan_entry tmp[SCAN_MAX];
	int n = 0, i, j;
	uint64_t f;

	for (i = 0; i < IEEE80211_NODE_MAX && n < SCAN_MAX; i++) {
		struct ieee80211_node *ni = ic->ic_nodes[i];
		struct scan_entry *e;
		uint8_t pmk[32];
		int secure;

		if (ni == NULL || ni->ni_esslen == 0 || ni->ni_inact > 2 ||
		    ni->ni_chan == IEEE80211_CHAN_ANYC)
			continue;
		e = &tmp[n];
		memset(e, 0, sizeof(*e));
		memcpy(e->ssid, ni->ni_essid, ni->ni_esslen);
		/* hide control characters */
		for (j = 0; j < ni->ni_esslen; j++)
			if ((uint8_t)e->ssid[j] < 32 || e->ssid[j] == 127)
				e->ssid[j] = '?';
		memcpy(e->bssid, ni->ni_bssid, 6);
		e->chan = (uint8_t)ieee80211_chan2ieee(ic, ni->ni_chan);
		e->dbm = node_dbm(ni);
		e->pct = (uint8_t)(ic->ic_max_rssi ?
		    MIN(100, ni->ni_rssi * 100 / ic->ic_max_rssi) : 0);
		strlcpy(e->sec, node_security(ni), sizeof(e->sec));
		e->saved = saved_lookup(e->ssid, pmk, &secure);
		explicit_bzero(pmk, sizeof(pmk));
		/* one line per SSID: keep the strongest BSS */
		for (j = 0; j < n; j++)
			if (strcmp(tmp[j].ssid, e->ssid) == 0)
				break;
		if (j < n) {
			if (e->dbm > tmp[j].dbm)
				tmp[j] = *e;
			continue;
		}
		n++;
	}
	/* strongest first */
	for (i = 1; i < n; i++) {
		struct scan_entry t = tmp[i];
		for (j = i; j > 0 && tmp[j - 1].dbm < t.dbm; j--)
			tmp[j] = tmp[j - 1];
		tmp[j] = t;
	}
	f = irq_save();
	memcpy(scan_snap, tmp, sizeof(tmp[0]) * (size_t)n);
	scan_snap_n = n;
	scan_snap_gen = ic->ic_scan_gen;
	irq_restore(f);
}

static const char *
state_text(void)
{
	struct ieee80211com *ic;

	switch (phase) {
	case WP_ABSENT:
		return "no-adapter";
	case WP_STARTING:
		return "starting";
	case WP_ERROR:
		return "error";
	case WP_RFKILL:
		return "radio-off";
	}
	ic = &sc->sc_ic;
	switch (ic->ic_state) {
	case IEEE80211_S_INIT:
		return "down";
	case IEEE80211_S_SCAN:
		if (target_ssid[0])
			return "searching";
		return (sc->sc_flags & IWM_FLAG_SCANNING) ? "scanning" : "idle";
	case IEEE80211_S_AUTH:
		return "authenticating";
	case IEEE80211_S_ASSOC:
		return "associating";
	case IEEE80211_S_RUN:
		if (!link_up)
			return "handshake";
		if (dhcp_status == 1)
			return "dhcp";
		if (dhcp_status == 3)
			return "no-ip";
		return "connected";
	}
	return "?";
}

static void
fmt_ip(char *out, size_t cap, uint32_t ip)
{
	snprintf(out, cap, "%u.%u.%u.%u", ip & 0xff, (ip >> 8) & 0xff,
	    (ip >> 16) & 0xff, ip >> 24);
}

static void
update_status_snapshot(void)
{
	char buf[1024], ip[20], gw[20], dns[20];
	struct ieee80211com *ic = sc ? &sc->sc_ic : NULL;
	struct ieee80211_node *ni = ic ? ic->ic_bss : NULL;
	size_t o = 0;
	uint64_t f;

#define ADD(...) do { o += (size_t)snprintf(buf + o, sizeof(buf) - o, \
    __VA_ARGS__); if (o >= sizeof(buf)) o = sizeof(buf) - 1; } while (0)

	ADD("state: %s\n", state_text());
	ADD("adapter: %s\n", wifi_pci ?
	    "Intel Dual Band Wireless-AC 8260 (iwm)" : "none");
	if (phase == WP_RUNNING) {
		ADD("mac: %s\n", ether_sprintf(wifi_macaddr));
		ADD("firmware: %s\n", sc->sc_fwver);
	}
	if (target_ssid[0])
		ADD("ssid: %s\n", target_ssid);
	if (ic != NULL && ic->ic_state >= IEEE80211_S_AUTH && ni != NULL) {
		ADD("bssid: %s\n", ether_sprintf(ni->ni_bssid));
		ADD("channel: %u\n", ieee80211_chan2ieee(ic, ni->ni_chan));
		ADD("signal: %d dBm\n", node_dbm(ni));
		if (ic->ic_state == IEEE80211_S_RUN &&
		    ni->ni_txrate < ni->ni_rates.rs_nrates) {
			int r = ni->ni_rates.rs_rates[ni->ni_txrate] &
			    IEEE80211_RATE_VAL;
			ADD("rate: %d%s Mbit/s\n", r / 2, (r & 1) ? ".5" : "");
		}
		ADD("security: %s\n", (ic->ic_flags & IEEE80211_F_RSNON) ?
		    "WPA2-PSK (CCMP)" : "open");
	}
	if (link_up && net_state.ready) {
		fmt_ip(ip, sizeof(ip), net_state.ip);
		fmt_ip(gw, sizeof(gw), net_state.gateway);
		fmt_ip(dns, sizeof(dns), net_state.dns);
		ADD("ip: %s\ngateway: %s\ndns: %s\n", ip, gw, dns);
	}
	ADD("autoconnect: %s\n", autoconnect ? "on" : "off");
	if (phase_error[0])
		ADD("error: %s\n", phase_error);
	else if (ieee80211_icda_last_error[0] && !link_up)
		ADD("error: %s\n", ieee80211_icda_last_error);
	if (ic != NULL)
		ADD("stats: rx %llu tx %llu rx-errors %llu tx-errors %llu "
		    "drops %llu/%llu\n",
		    (unsigned long long)ic->ic_if.if_ipackets,
		    (unsigned long long)ic->ic_if.if_opackets,
		    (unsigned long long)ic->ic_if.if_ierrors,
		    (unsigned long long)ic->ic_if.if_oerrors,
		    (unsigned long long)rx_drops,
		    (unsigned long long)tx_drops);
#undef ADD
	f = irq_save();
	memcpy(status_snap, buf, o + 1);
	irq_restore(f);
}

/* ---- /dev/wifi ---------------------------------------------------------- */

static size_t
append(char *buf, size_t cap, size_t o, const char *s)
{
	while (*s && o + 1 < cap)
		buf[o++] = *s++;
	buf[o] = '\0';
	return o;
}

uint64_t
wifi_node_read(char *buf, uint64_t cap)
{
	size_t o = 0;
	char line[128];
	uint64_t f;
	int v = view, i;

	if (cap == 0)
		return 0;
	buf[0] = '\0';
	if (v == VIEW_LOG)
		return iwm_compat_log_read(buf, (size_t)cap);
	if (v == VIEW_ALL || v == VIEW_STATUS) {
		f = irq_save();
		if (sc == NULL && status_snap[0] == '\0')
			snprintf(status_snap, sizeof(status_snap),
			    "state: %s\n", wifi_pci ? "starting" : "no-adapter");
		o = append(buf, (size_t)cap, o, status_snap);
		irq_restore(f);
	}
	if (v == VIEW_ALL || v == VIEW_SCAN) {
		f = irq_save();
		snprintf(line, sizeof(line), "--- networks (scan %u)\n",
		    scan_snap_gen);
		o = append(buf, (size_t)cap, o, line);
		for (i = 0; i < scan_snap_n; i++) {
			const struct scan_entry *e = &scan_snap[i];
			snprintf(line, sizeof(line), "%u\t%d\t%u\t%s\t%s\t%s\n",
			    e->pct, e->dbm, e->chan, e->sec,
			    e->saved ? "saved" : "-", e->ssid);
			o = append(buf, (size_t)cap, o, line);
		}
		irq_restore(f);
	}
	if (v == VIEW_SAVED) {
		uint64_t size = 0, k;
		const char *data = vfs_read(vfs_root(), WIFI_SAVED_PATH, &size);
		for (k = 0; data && k < size && o + 1 < cap; k++) {
			/* names only, never the keys */
			if (data[k] == '\t') {
				while (k < size && data[k] != '\n')
					k++;
				buf[o++] = '\n';
				continue;
			}
			buf[o++] = data[k];
		}
		buf[o] = '\0';
	}
	return o;
}

static int
is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Commands (one per write):
 *   scan | disconnect | restart | status | log | saved | all
 *   connect <ssid> [password]	(fields separated by TAB, or by a space
 *				 when the SSID has no spaces)
 *   forget <ssid> | autoconnect on|off | log clear
 */
uint64_t
wifi_node_write(const char *ubuf, uint64_t len)
{
	char cmd[160], *arg, *a1, *a2;
	size_t n = len < sizeof(cmd) - 1 ? (size_t)len : sizeof(cmd) - 1;
	uint64_t f;
	char sep;

	memcpy(cmd, ubuf, n);
	cmd[n] = '\0';
	while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r'))
		cmd[--n] = '\0';
	for (arg = cmd; *arg && !is_space(*arg); arg++)
		;
	sep = *arg;
	if (*arg)
		*arg++ = '\0';

	if (strcmp(cmd, "status") == 0) {
		view = VIEW_STATUS;
	} else if (strcmp(cmd, "all") == 0) {
		view = VIEW_ALL;
	} else if (strcmp(cmd, "saved") == 0) {
		view = VIEW_SAVED;
	} else if (strcmp(cmd, "log") == 0) {
		if (strcmp(arg, "clear") == 0)
			iwm_compat_log_clear();
		view = VIEW_LOG;
	} else if (strcmp(cmd, "scan") == 0) {
		view = VIEW_SCAN;
		req = REQ_SCAN;
	} else if (strcmp(cmd, "disconnect") == 0) {
		req = REQ_DISCONNECT;
	} else if (strcmp(cmd, "restart") == 0) {
		req = REQ_RESTART;
	} else if (strcmp(cmd, "autoconnect") == 0) {
		autoconnect = (strcmp(arg, "off") != 0);
	} else if (strcmp(cmd, "connect") == 0 || strcmp(cmd, "forget") == 0) {
		int connect = cmd[0] == 'c';
		/* TAB-separated if the command used a TAB, else spaces */
		char fs = (sep == '\t') ? '\t' : ' ';

		a1 = arg;
		for (a2 = a1; *a2 && *a2 != fs; a2++)
			;
		if (*a2)
			*a2++ = '\0';
		if (*a1 == '\0' || strlen(a1) > IEEE80211_NWID_LEN ||
		    strlen(a2) > 64)
			return (uint64_t)-1;
		f = irq_save();
		strlcpy(req_ssid, a1, sizeof(req_ssid));
		strlcpy(req_pass, connect ? a2 : "", sizeof(req_pass));
		req = connect ? REQ_CONNECT : REQ_FORGET;
		irq_restore(f);
		explicit_bzero(cmd, sizeof(cmd));
		view = VIEW_STATUS;
	} else
		return (uint64_t)-1;
	return len;
}

/* ---- bring-up ----------------------------------------------------------- */

static void
dump_regs(const char *why)
{
	if (sc == NULL || sc->sc_sh == NULL)
		return;
	printf("wifi: registers (%s): HW_REV %08x GP_CNTRL %08x "
	    "HW_IF_CONFIG %08x INT %08x INT_MASK %08x FH_INT %08x "
	    "RESET %08x GIO %08x\n", why,
	    IWM_READ(sc, IWM_CSR_HW_REV), IWM_READ(sc, IWM_CSR_GP_CNTRL),
	    IWM_READ(sc, IWM_CSR_HW_IF_CONFIG_REG), IWM_READ(sc, IWM_CSR_INT),
	    IWM_READ(sc, IWM_CSR_INT_MASK),
	    IWM_READ(sc, IWM_CSR_FH_INT_STATUS), IWM_READ(sc, IWM_CSR_RESET),
	    IWM_READ(sc, IWM_CSR_GIO_REG));
}

static void
wifi_link_change(struct ifnet *ifp)
{
	int up = LINK_STATE_IS_UP(ifp->if_link_state);

	if (up && !link_up) {
		struct ieee80211com *ic = &sc->sc_ic;
		printf("wifi: link up (%s), starting DHCP\n",
		    ether_sprintf(ic->ic_bss->ni_bssid));
		link_up = 1;
		dhcp_status = 1;
		dhcp_request = 1;
		if (target_saved && target_ssid[0]) {
			saved_update(target_ssid, target_pmk, target_secure, 1);
			target_saved = 0;
		}
	} else if (!up && link_up) {
		printf("wifi: link down\n");
		link_up = 0;
		dhcp_status = 0;
		net_link_down();
	}
}

static int
wifi_attach(void)
{
	uint32_t bar0, bar1, cmd;
	uint64_t mmio_phys;
	volatile uint8_t *mmio;
	struct ifnet *ifp;
	int err;

	printf("wifi: [1/6] PCI %02x:%02x.%u %04x:%04x subsystem %08x "
	    "rev %02x\n", wifi_pci->bus, wifi_pci->device, wifi_pci->function,
	    wifi_pci->vendor_id, wifi_pci->device_id,
	    pci_read_config32(wifi_pci, 0x2c), wifi_pci->revision_id);

	if (pci_enable_memory_busmaster(wifi_pci) != 0) {
		snprintf(phase_error, sizeof(phase_error),
		    "could not enable PCI memory/bus master");
		return ENXIO;
	}
	/* No interrupts: mask INTx; iwm_intr() is polled. */
	cmd = pci_read_config32(wifi_pci, PCI_COMMAND_STATUS_REG);
	pci_write_config16(wifi_pci, PCI_COMMAND_STATUS_REG,
	    (uint16_t)(cmd | PCI_COMMAND_INTERRUPT_DISABLE));

	bar0 = pci_read_config32(wifi_pci, 0x10);
	bar1 = pci_read_config32(wifi_pci, 0x14);
	if (bar0 & 1) {
		snprintf(phase_error, sizeof(phase_error),
		    "BAR0 is an I/O BAR (0x%08x)", bar0);
		return ENXIO;
	}
	mmio_phys = bar0 & ~0xFULL;
	if (((bar0 >> 1) & 3) == 2)
		mmio_phys |= (uint64_t)bar1 << 32;
	if (mmio_phys == 0) {
		snprintf(phase_error, sizeof(phase_error),
		    "BAR0 not assigned by firmware");
		return ENXIO;
	}
	mmio = vmm_map_physical(mmio_phys, 0x2000,
	    VMM_FLAGS_KERNEL_RW | PTE_NO_CACHE | PTE_WRITE_THRU);
	printf("wifi: [2/6] BAR0 0x%llx mapped at %p, PCI command 0x%04x "
	    "status 0x%04x\n", (unsigned long long)mmio_phys, (void *)mmio,
	    pci_read_config16(wifi_pci, 0x04),
	    pci_read_config16(wifi_pci, 0x06));
	if (mmio == NULL) {
		snprintf(phase_error, sizeof(phase_error),
		    "could not map BAR0");
		return ENOMEM;
	}

	sc = kmalloc(sizeof(*sc));
	if (sc == NULL) {
		snprintf(phase_error, sizeof(phase_error),
		    "out of memory for the driver (%u bytes)",
		    (unsigned)sizeof(*sc));
		return ENOMEM;
	}
	memset(sc, 0, sizeof(*sc));
	strlcpy(sc->sc_dev.dv_xname, "iwm0", sizeof(sc->sc_dev.dv_xname));
	sc->sc_sh = mmio;
	sc->sc_sz = 0x2000;
	sc->sc_pcitag = wifi_pci;

	printf("wifi: [3/6] attaching (softc %u bytes)\n",
	    (unsigned)sizeof(*sc));
	err = iwm_icda_attach(sc);
	if (err) {
		dump_regs("attach failed");
		snprintf(phase_error, sizeof(phase_error),
		    "attach failed (error %d), see wifi log", err);
		return err;
	}
	ifp = &sc->sc_ic.ic_if;
	ifp->if_input_icda = wifi_if_input;
	ifp->if_link_icda = wifi_link_change;
	ifp->if_flags |= IFF_UP | IFF_DEBUG;
	iwm_compat_set_poll(iwm_intr, sc);
	return 0;
}

/* Load the firmware and start scanning; also used to restart. */
static int
wifi_start(void)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int err;

	phase = WP_STARTING;
	phase_error[0] = '\0';
	printf("wifi: [4/6] loading firmware %s\n", sc->sc_fwname);
	err = iwm_init(ifp);
	if (err) {
		dump_regs("init failed");
		if (sc->sc_flags & IWM_FLAG_RFKILL) {
			phase = WP_RFKILL;
			snprintf(phase_error, sizeof(phase_error),
			    "radio is off (RF kill switch / airplane mode)");
		} else {
			phase = WP_ERROR;
			snprintf(phase_error, sizeof(phase_error),
			    "firmware start failed (error %d), see wifi log",
			    err);
		}
		printf("wifi: %s\n", phase_error);
		return err;
	}
	memcpy(wifi_macaddr, sc->sc_ic.ic_myaddr, 6);
	printf("wifi: [5/6] firmware %s running, MAC %s, 5 GHz %s\n",
	    sc->sc_fwver, ether_sprintf(wifi_macaddr),
	    sc->sc_nvm.sku_cap_band_52GHz_enable ? "yes" : "no");
	if (sc->sc_flags & IWM_FLAG_RFKILL) {
		phase = WP_RFKILL;
		snprintf(phase_error, sizeof(phase_error),
		    "radio is off (RF kill switch / airplane mode)");
		printf("wifi: %s\n", phase_error);
	} else
		phase = WP_RUNNING;
	printf("wifi: [6/6] scanning\n");
	return 0;
}

static void
wifi_restart(void)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	if (ifp->if_flags & IFF_RUNNING)
		iwm_stop(ifp);
	link_up = 0;
	(void)wifi_start();
}

/* ---- requests ------------------------------------------------------------ */

static struct ieee80211_node *
find_ssid(const char *ssid)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *best = NULL;
	size_t len = strlen(ssid);
	int i;

	for (i = 0; i < IEEE80211_NODE_MAX; i++) {
		struct ieee80211_node *ni = ic->ic_nodes[i];
		if (ni == NULL || ni->ni_esslen != len ||
		    memcmp(ni->ni_essid, ssid, len) != 0)
			continue;
		if (best == NULL || ni->ni_rssi > best->ni_rssi)
			best = ni;
	}
	return best;
}

static void
do_disconnect(int keep_target)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;

	if (ic->ic_state == IEEE80211_S_RUN) {
		/* Tell the AP politely, then let the frame go out. */
		IEEE80211_SEND_MGMT(ic, ic->ic_bss,
		    IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
		iwm_start(ifp);
		(void)tsleep_nsec(&ic, 0, "wifidis", MSEC_TO_NSEC(100));
	}
	if (!keep_target) {
		target_ssid[0] = '\0';
		explicit_bzero(target_pmk, sizeof(target_pmk));
	}
	ieee80211_icda_configure(ic, "", 0, NULL);
	wifi_restart();
}

/* Configure and (re)start association with the current target. */
static void
start_connect(void)
{
	struct ieee80211com *ic = &sc->sc_ic;

	ieee80211_icda_configure(ic, target_ssid, strlen(target_ssid),
	    target_secure ? target_pmk : NULL);
	badmic_base = ic->ic_stats.is_handshake_fail;
	printf("wifi: connecting to \"%s\" (%s)\n", target_ssid,
	    target_secure ? "WPA2-PSK" : "open");
	if (ic->ic_state == IEEE80211_S_SCAN &&
	    (ic->ic_if.if_flags & IFF_RUNNING))
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	else
		wifi_restart();
}

static void
do_connect(const char *ssid, const char *pass)
{
	struct ieee80211_node *ni = find_ssid(ssid);
	int secure = 1;

	phase_error[0] = '\0';
	if (ni != NULL) {
		const char *sec = node_security(ni);
		if (strcmp(sec, "open") == 0)
			secure = 0;
		else if (strcmp(sec, "WPA/WPA2") == 0) {
			snprintf(phase_error, sizeof(phase_error),
			    "\"%s\" is in WPA/WPA2 mixed mode (TKIP group key); "
			    "set the router to WPA2 (AES) only", ssid);
			printf("wifi: %s\n", phase_error);
			return;
		} else if (strcmp(sec, "WPA2") != 0) {
			snprintf(phase_error, sizeof(phase_error),
			    "\"%s\" uses %s, only WPA2-PSK and open networks "
			    "are supported", ssid, sec);
			printf("wifi: %s\n", phase_error);
			return;
		}
	} else if (pass[0] == '\0')
		secure = 0;	/* not seen: assume open unless saved */

	if (ic_is_running() && target_ssid[0] &&
	    strcmp(target_ssid, ssid) != 0 &&
	    sc->sc_ic.ic_state > IEEE80211_S_SCAN)
		do_disconnect(1);

	strlcpy(target_ssid, ssid, sizeof(target_ssid));
	target_saved = 1;
	if (pass[0] != '\0') {
		printf("wifi: deriving key for \"%s\"\n", ssid);
		if (wpa_passphrase_to_pmk(pass, (const uint8_t *)ssid,
		    strlen(ssid), target_pmk) != 0) {
			snprintf(phase_error, sizeof(phase_error),
			    "the password must be 8-63 characters "
			    "(or 64 hex digits)");
			printf("wifi: %s\n", phase_error);
			target_ssid[0] = '\0';
			return;
		}
		secure = 1;
	} else {
		int saved_secure;
		if (saved_lookup(ssid, target_pmk, &saved_secure)) {
			secure = saved_secure;
			target_saved = 0;
		} else if (secure) {
			snprintf(phase_error, sizeof(phase_error),
			    "\"%s\" needs a password", ssid);
			printf("wifi: %s\n", phase_error);
			target_ssid[0] = '\0';
			return;
		}
	}
	target_secure = secure;
	start_connect();
}

static int
ic_is_running(void)
{
	return sc != NULL && (sc->sc_ic.ic_if.if_flags & IFF_RUNNING);
}

static void
handle_request(void)
{
	char ssid[sizeof(req_ssid)], pass[sizeof(req_pass)];
	uint64_t f;
	int r;

	f = irq_save();
	r = req;
	req = REQ_NONE;
	memcpy(ssid, req_ssid, sizeof(ssid));
	memcpy(pass, req_pass, sizeof(pass));
	explicit_bzero(req_pass, sizeof(req_pass));
	irq_restore(f);

	if (r == REQ_RESTART) {
		printf("wifi: restart requested\n");
		wifi_restart();
		goto out;
	}
	if (phase != WP_RUNNING)
		goto out;
	switch (r) {
	case REQ_SCAN:
		if (sc->sc_ic.ic_state == IEEE80211_S_SCAN &&
		    !(sc->sc_flags & IWM_FLAG_SCANNING)) {
			printf("wifi: scan requested\n");
			ieee80211_new_state(&sc->sc_ic, IEEE80211_S_SCAN, -1);
		}
		break;
	case REQ_CONNECT:
		autoconnect = 1;
		do_connect(ssid, pass);
		break;
	case REQ_DISCONNECT:
		printf("wifi: disconnect requested\n");
		autoconnect = 0;
		do_disconnect(0);
		break;
	case REQ_FORGET:
		printf("wifi: forgetting \"%s\"\n", ssid);
		saved_update(ssid, NULL, 0, 0);
		if (strcmp(ssid, target_ssid) == 0)
			do_disconnect(0);
		break;
	}
out:
	explicit_bzero(pass, sizeof(pass));
}

/* After a scan round: join the best saved network that is in range. */
static void
maybe_autoconnect(void)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *best = NULL;
	uint8_t pmk[32], best_pmk[32];
	int i, secure, best_secure = 0;

	if (!autoconnect || target_ssid[0] || ic->ic_state != IEEE80211_S_SCAN)
		return;
	for (i = 0; i < IEEE80211_NODE_MAX; i++) {
		struct ieee80211_node *ni = ic->ic_nodes[i];
		char ssid[IEEE80211_NWID_LEN + 1];

		if (ni == NULL || ni->ni_esslen == 0 || ni->ni_inact > 1)
			continue;
		memcpy(ssid, ni->ni_essid, ni->ni_esslen);
		ssid[ni->ni_esslen] = '\0';
		if (!saved_lookup(ssid, pmk, &secure))
			continue;
		if (best == NULL || ni->ni_rssi > best->ni_rssi) {
			best = ni;
			best_secure = secure;
			memcpy(best_pmk, pmk, sizeof(pmk));
		}
	}
	if (best != NULL) {
		memcpy(target_ssid, best->ni_essid, best->ni_esslen);
		target_ssid[best->ni_esslen] = '\0';
		memcpy(target_pmk, best_pmk, sizeof(best_pmk));
		target_secure = best_secure;
		target_saved = 0;
		printf("wifi: auto-connecting to saved network \"%s\"\n",
		    target_ssid);
		start_connect();
	}
	explicit_bzero(pmk, sizeof(pmk));
	explicit_bzero(best_pmk, sizeof(best_pmk));
}

/* Stop retrying a network whose handshake keeps failing on the MIC. */
static void
check_wrong_password(void)
{
	struct ieee80211com *ic = &sc->sc_ic;
	static uint64_t handshake_since;
	uint64_t now = iwm_compat_nsecuptime();
	const char *why = NULL;

	if (!target_ssid[0] || !target_secure || link_up) {
		handshake_since = 0;
		return;
	}
	/*
	 * With a wrong password the AP rejects message 2 and deauthenticates
	 * us; a bad MIC on message 3 is the other symptom.  Either twice, or
	 * a handshake that never completes, means: stop retrying.
	 */
	if (ic->ic_state == IEEE80211_S_RUN) {
		if (handshake_since == 0)
			handshake_since = now;
		else if (now - handshake_since > 20000000000ULL)
			why = "the WPA2 handshake timed out (wrong password?)";
	} else
		handshake_since = 0;
	if (ic->ic_stats.is_handshake_fail - badmic_base >= 2)
		why = "wrong password (the access point rejected the handshake)";
	if (why == NULL)
		return;
	handshake_since = 0;
	printf("wifi: %s, giving up on \"%s\"\n", why, target_ssid);
	if (!target_saved)
		saved_update(target_ssid, NULL, 0, 0);	/* stale saved key */
	autoconnect = 0;
	do_disconnect(0);
	snprintf(phase_error, sizeof(phase_error), "%s", why);
}

/* ---- threads ------------------------------------------------------------- */

static void
dhcp_thread(void)
{
	for (;;) {
		if (dhcp_request && link_up) {
			int tries;

			dhcp_request = 0;
			dhcp_status = 1;
			for (tries = 0; tries < 4 && link_up; tries++) {
				if (net_reconfigure() == 0)
					break;
				sched_sleep(100);
			}
			dhcp_status = (link_up && net_state.ready) ? 2 : 3;
			if (dhcp_status == 2) {
				char ip[20];
				fmt_ip(ip, sizeof(ip), net_state.ip);
				printf("wifi: DHCP ok, address %s\n", ip);
			} else
				printf("wifi: DHCP failed\n");
		}
		sched_sleep(10);
	}
}

void sock_tick(void);		/* net/sock.c: moves received frames into the stack */

void icda_wifi_main(void) __attribute__((noreturn));

void
icda_wifi_main(void)
{
	struct ifnet *ifp;
	uint64_t last_sec = 0, last_snap = 0, last_rescan = 0, last_work = 0;

	iwm_compat_init();		/* TSC calibration for DELAY() */
	iwm_compat_set_console(1);	/* bring-up steps on screen too */
	if (wifi_attach() != 0) {
		phase = WP_ERROR;
		printf("wifi: %s\n", phase_error);
	} else
		(void)wifi_start();
	iwm_compat_set_console(0);
	if (sc == NULL) {
		update_status_snapshot();
		for (;;)
			sched_sleep(1000);
	}
	ifp = &sc->sc_ic.ic_if;
	(void)proc_create_kernel(dhcp_thread);

	for (;;) {
		uint64_t now = iwm_compat_nsecuptime();
		int work = 0;

		if (req != REQ_NONE)
			handle_request();

		if (ifp->if_flags & IFF_RUNNING) {
			work += iwm_intr(sc);
			work += iwm_compat_run_tasks();
			work += iwm_compat_run_timeouts();
			work += wifi_drain_tx(ifp);
			if (ifp->if_start_pending) {
				ifp->if_start_pending = 0;
				iwm_start(ifp);
				work++;
			}
		} else {
			/* restart tasks still need to run */
			work += iwm_compat_run_tasks();
		}

		if (now - last_sec >= 1000000000ULL) {
			last_sec = now;
			if (ifp->if_timer > 0 && --ifp->if_timer == 0 &&
			    ifp->if_watchdog)
				ifp->if_watchdog(ifp);
			if (phase == WP_RUNNING)
				check_wrong_password();
			/*
			 * Not connected but networks are saved: look again every
			 * 30 s (saved networks may appear, or /home may only have
			 * been loaded after the first scan at boot).
			 */
			if (phase == WP_RUNNING && autoconnect && !target_ssid[0] &&
			    sc->sc_ic.ic_state == IEEE80211_S_SCAN &&
			    !(sc->sc_flags & IWM_FLAG_SCANNING) &&
			    now - last_rescan >= 30000000000ULL) {
				uint64_t size = 0;
				last_rescan = now;
				if (vfs_read(vfs_root(), WIFI_SAVED_PATH, &size) &&
				    size > 0)
					ieee80211_new_state(&sc->sc_ic,
					    IEEE80211_S_SCAN, -1);
			}
		}
		if (sc->sc_ic.ic_scan_gen != last_scan_gen) {
			last_scan_gen = sc->sc_ic.ic_scan_gen;
			update_scan_snapshot();
			maybe_autoconnect();
		}
		if (now - last_snap >= 250000000ULL) {
			last_snap = now;
			update_status_snapshot();
		}
		/*
		 * The adapter is polled (no interrupts).  Sleeping a whole tick
		 * after every idle pass added up to 10 ms to each packet in and
		 * each ACK out; while traffic flows (work in the last 50 ms) the
		 * thread only yields between passes.
		 */
		if (work) {
			last_work = now;
			sock_tick();	/* received frames to TCP now, not at the next tick */
		}
		if (work || now - last_work < 50000000ULL)
			sched_yield();
		else
			sched_sleep(1);
	}
}

/* Kernel threads get 8 KiB of stack; give the driver a larger one. */
static void
wifi_thread(void)
{
	uint64_t phys = pmm_alloc_contiguous(WIFI_STACK_PAGES);
	uint64_t top;

	if (phys == 0) {
		printf("wifi: no memory for the driver stack\n");
		for (;;)
			sched_sleep(1000);
	}
	top = (uint64_t)PHYS_TO_VIRT(phys) + WIFI_STACK_PAGES * PAGE_SIZE;
	__asm__ volatile("mov %0, %%rsp\n\t"
	    "call icda_wifi_main\n\t"
	    "ud2" :: "r"(top) : "memory");
	__builtin_unreachable();
}

int
wifi_init(void)
{
	uint32_t i;

	for (i = 0; i < pci_device_count(); i++) {
		const pci_device_t *d = pci_device_at(i);
		if (d && d->vendor_id == WIFI_VENDOR_INTEL &&
		    d->device_id == WIFI_DEVICE_8260) {
			wifi_pci = d;
			break;
		}
	}
	if (wifi_pci == NULL)
		return -1;
	phase = WP_STARTING;
	if (proc_create_kernel(wifi_thread) == NULL) {
		phase = WP_ERROR;
		return -1;
	}
	return 0;
}
