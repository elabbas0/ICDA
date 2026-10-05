/*
 * ICDA port: the OpenBSD kernel interfaces that iwm(4) and the compact
 * net80211 layer use, implemented on top of ICDA.
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
 * Model (see iwm_compat.c):
 *  - Everything iwm does runs on one kernel thread (wifi.c).  Tasks and
 *    timeouts are queued and run from that thread's loop; spl and rwlocks
 *    are no-ops.
 *  - There are no interrupts.  tsleep_nsec() polls the driver's interrupt
 *    routine (CSR_INT) until wakeup() names the sleep channel or the
 *    timeout expires.
 *  - bus_space is plain MMIO on BAR0; bus_dma hands out physically
 *    contiguous pmm pages below 4 GiB; every mbuf is one contiguous,
 *    DMA-able, reference-counted buffer (no chains).
 */

#ifndef ICDA_IWM_COMPAT_H
#define ICDA_IWM_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "../../pci/pci.h"

/* ---- basic types ------------------------------------------------------ */

typedef uint8_t		u_int8_t;
typedef uint16_t	u_int16_t;
typedef uint32_t	u_int32_t;
typedef uint64_t	u_int64_t;
typedef unsigned char	u_char;
typedef unsigned short	u_short;
typedef unsigned int	u_int;
typedef unsigned long	u_long;
typedef char		*caddr_t;
typedef long		ssize_t;
typedef int64_t		off_t;
typedef uint64_t	bus_addr_t;
typedef uint64_t	bus_size_t;
typedef int		bus_space_tag_t;
typedef volatile uint8_t *bus_space_handle_t;
typedef void		*bus_dma_tag_t;
typedef uint32_t	pcireg_t;
typedef const pci_device_t *pcitag_t;
typedef void		*pci_chipset_tag_t;

#ifndef NULL
#define NULL		((void *)0)
#endif

#define __packed	__attribute__((__packed__))
#define __aligned(x)	__attribute__((__aligned__(x)))
#define __unused	__attribute__((__unused__))
#define __predict_false(x)	__builtin_expect(!!(x), 0)
#define __predict_true(x)	__builtin_expect(!!(x), 1)
#define CTASSERT(x)	_Static_assert(x, #x)

#ifndef offsetof
#define offsetof(t, m)	__builtin_offsetof(t, m)
#endif

#define NBBY		8
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#define setbit(a, i)	(((uint8_t *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)	(((uint8_t *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)	(((const uint8_t *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)	((((const uint8_t *)(a))[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)

static inline u_int min(u_int a, u_int b) { return a < b ? a : b; }
static inline u_int max(u_int a, u_int b) { return a > b ? a : b; }

/* ---- errno ------------------------------------------------------------ */

#define EPERM		1
#define ENOENT		2
#define EINTR		4
#define EIO		5
#define ENXIO		6
#define E2BIG		7
#define ENOMEM		12
#define EACCES		13
#define EFAULT		14
#define EBUSY		16
#define EEXIST		17
#define ENODEV		19
#define EINVAL		22
#define EFBIG		27
#define ENOSPC		28
#define ERANGE		34
#define EAGAIN		35
#define EWOULDBLOCK	EAGAIN
#define ENOBUFS		55
#define ETIMEDOUT	60
#define ENOTSUP		91
#define ERESTART	-1

/* ---- byte order (x86 is little endian) -------------------------------- */

#define htole16(x)	((uint16_t)(x))
#define htole32(x)	((uint32_t)(x))
#define htole64(x)	((uint64_t)(x))
#define le16toh(x)	((uint16_t)(x))
#define le32toh(x)	((uint32_t)(x))
#define le64toh(x)	((uint64_t)(x))
#define letoh16(x)	le16toh(x)
#define letoh32(x)	le32toh(x)
#define letoh64(x)	le64toh(x)
#define htobe16(x)	__builtin_bswap16((uint16_t)(x))
#define htobe32(x)	__builtin_bswap32((uint32_t)(x))
#define htobe64(x)	__builtin_bswap64((uint64_t)(x))
#define be16toh(x)	htobe16(x)
#define be32toh(x)	htobe32(x)
#define be64toh(x)	htobe64(x)
#define htons(x)	htobe16(x)
#define ntohs(x)	htobe16(x)
#define htonl(x)	htobe32(x)
#define ntohl(x)	htobe32(x)

/* ---- libc pieces ------------------------------------------------------ */

void	*memcpy(void *, const void *, size_t);
void	*memmove(void *, const void *, size_t);
void	*memset(void *, int, size_t);
int	 memcmp(const void *, const void *, size_t);
size_t	 strlen(const char *);
size_t	 strlcpy(char *, const char *, size_t);
int	 strcmp(const char *, const char *);
int	 strncmp(const char *, const char *, size_t);
int	 timingsafe_bcmp(const void *, const void *, size_t);
void	 explicit_bzero(void *, size_t);
int	 vsnprintf(char *, size_t, const char *, va_list);
int	 snprintf(char *, size_t, const char *, ...)
	    __attribute__((__format__(__printf__, 3, 4)));
int	 printf(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));
void	 panic(const char *, ...) __attribute__((__noreturn__));
void	 arc4random_buf(void *, size_t);
uint32_t arc4random(void);

#define bzero(p, n)	memset((p), 0, (n))

#define KASSERT(e) do {							\
	if (__predict_false(!(e)))					\
		panic("assertion \"%s\" failed: %s:%d", #e, __FILE__,	\
		    __LINE__);						\
} while (0)

/* ---- malloc(9) -------------------------------------------------------- */

#define M_DEVBUF	2
#define M_TEMP		127
#define M_WAITOK	0x0001
#define M_WAIT		M_WAITOK
#define M_NOWAIT	0x0002
#define M_ZERO		0x0008
#define M_CANFAIL	0x0004

void	*malloc(size_t, int, int);
void	 free(void *, int, size_t);
void	*mallocarray(size_t, size_t, int, int);

/* ---- time ------------------------------------------------------------- */

struct timeval {
	int64_t	tv_sec;
	int64_t	tv_usec;
};

#define INFSLP		UINT64_MAX
#define SEC_TO_NSEC(s)	((uint64_t)(s) * 1000000000ULL)
#define MSEC_TO_NSEC(m)	((uint64_t)(m) * 1000000ULL)
#define USEC_TO_NSEC(u)	((uint64_t)(u) * 1000ULL)
#define PCATCH		0x100

void	 DELAY(unsigned int usec);
uint64_t iwm_compat_nsecuptime(void);
void	 getmicrouptime(struct timeval *);
void	 microuptime(struct timeval *);
#define getuptime()	(iwm_compat_nsecuptime() / 1000000000ULL)

/* ---- sleep/wakeup (polling) ------------------------------------------- */

int	 tsleep_nsec(const volatile void *, int, const char *, uint64_t);
void	 wakeup(const volatile void *);
#define wakeup_one(c)	wakeup(c)

/*
 * The routine tsleep_nsec() polls while it waits; wifi.c points it at
 * iwm_intr().  Also used by the driver thread's main loop.
 */
void	 iwm_compat_set_poll(int (*fn)(void *), void *arg);
/* Calibrates the TSC against the 100 Hz tick (call early on the thread). */
void	 iwm_compat_init(void);

/* ---- spl, locks, refcounts (no-ops: one thread owns the driver) ------- */

#define IPL_NET		6
#define IPL_NONE	0
#define splnet()	0
#define splx(s)		((void)(s))
#define splassert(l)	do { } while (0)

struct rwlock { int rwl_dummy; };
#define rw_init(rw, name)	((void)(rw))
#define rw_enter_write(rw)	((void)(rw))
#define rw_exit(rw)		((void)(rw))
#define rw_exit_write(rw)	((void)(rw))
#define rw_assert_wrlock(rw)	((void)(rw))

struct refcnt { unsigned int r_refs; };
void	 refcnt_init(struct refcnt *);
void	 refcnt_take(struct refcnt *);
int	 refcnt_rele(struct refcnt *);
void	 refcnt_rele_wake(struct refcnt *);
void	 refcnt_finalize(struct refcnt *, const char *);

/* ---- tasks and timeouts (run from the driver thread) ------------------ */

struct task {
	void		(*t_func)(void *);
	void		*t_arg;
	int		 t_pending;
	struct task	*t_next;
};

struct taskq {
	const char	*tq_name;
	struct task	*tq_head;
	struct task	*tq_tail;
	struct taskq	*tq_next;
};

extern struct taskq *const systq;

void	 task_set(struct task *, void (*)(void *), void *);
int	 task_add(struct taskq *, struct task *);
int	 task_del(struct taskq *, struct task *);
#define task_pending(t)	((t)->t_pending)
struct taskq *taskq_create(const char *, unsigned int, int, unsigned int);
/* Runs every queued task once; returns how many ran. */
int	 iwm_compat_run_tasks(void);

struct timeout {
	void		(*to_func)(void *);
	void		*to_arg;
	uint64_t	 to_deadline;	/* nsec uptime */
	int		 to_pending;
	struct timeout	*to_next;
};

void	 timeout_set(struct timeout *, void (*)(void *), void *);
int	 timeout_add_msec(struct timeout *, int);
int	 timeout_add_sec(struct timeout *, int);
int	 timeout_add_usec(struct timeout *, int);
int	 timeout_del(struct timeout *);
#define timeout_pending(to)	((to)->to_pending)
#define timeout_initialized(to)	((to)->to_func != NULL)
/* Fires expired timeouts; returns how many ran. */
int	 iwm_compat_run_timeouts(void);

/* ---- bus_space -------------------------------------------------------- */

#define BUS_SPACE_BARRIER_READ	0x01
#define BUS_SPACE_BARRIER_WRITE	0x02

static inline uint32_t
bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	(void)t;
	return *(volatile uint32_t *)(h + o);
}

static inline void
bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint32_t v)
{
	(void)t;
	*(volatile uint32_t *)(h + o) = v;
}

static inline void
bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint8_t v)
{
	(void)t;
	*(volatile uint8_t *)(h + o) = v;
}

static inline void
bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    bus_size_t l, int f)
{
	(void)t; (void)h; (void)o; (void)l; (void)f;
	__asm__ volatile("mfence" ::: "memory");
}

/* ---- bus_dma ---------------------------------------------------------- */

#define BUS_DMA_WAITOK		0x0000
#define BUS_DMA_NOWAIT		0x0001
#define BUS_DMA_ALLOCNOW	0x0002
#define BUS_DMA_COHERENT	0x0004
#define BUS_DMA_READ		0x0200
#define BUS_DMA_WRITE		0x0400
#define BUS_DMA_ZERO		0x1000

#define BUS_DMASYNC_PREREAD	0x01
#define BUS_DMASYNC_POSTREAD	0x02
#define BUS_DMASYNC_PREWRITE	0x04
#define BUS_DMASYNC_POSTWRITE	0x08

#define ICDA_DMA_MAXSEGS	20

typedef struct {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
	void		*_ds_vaddr;	/* ICDA: kernel mapping (HHDM) */
	uint64_t	 _ds_npages;	/* ICDA: pages owned, 0 = none */
} bus_dma_segment_t;

struct bus_dmamap {
	bus_size_t		dm_mapsize;
	int			dm_nsegs;
	bus_size_t		_dm_size;
	int			_dm_segcnt;
	bus_dma_segment_t	dm_segs[ICDA_DMA_MAXSEGS];
};
typedef struct bus_dmamap *bus_dmamap_t;

struct mbuf;
struct uio;

int	 bus_dmamap_create(bus_dma_tag_t, bus_size_t, int, bus_size_t,
	    bus_size_t, int, bus_dmamap_t *);
void	 bus_dmamap_destroy(bus_dma_tag_t, bus_dmamap_t);
int	 bus_dmamap_load(bus_dma_tag_t, bus_dmamap_t, void *, bus_size_t,
	    void *, int);
int	 bus_dmamap_load_mbuf(bus_dma_tag_t, bus_dmamap_t, struct mbuf *,
	    int);
void	 bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
int	 bus_dmamem_alloc(bus_dma_tag_t, bus_size_t, bus_size_t, bus_size_t,
	    bus_dma_segment_t *, int, int *, int);
void	 bus_dmamem_free(bus_dma_tag_t, bus_dma_segment_t *, int);
int	 bus_dmamem_map(bus_dma_tag_t, bus_dma_segment_t *, int, size_t,
	    caddr_t *, int);
void	 bus_dmamem_unmap(bus_dma_tag_t, caddr_t, size_t);

static inline void
bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t m, bus_addr_t o, bus_size_t l,
    int ops)
{
	/* x86 DMA is cache coherent; only order the CPU's accesses. */
	(void)t; (void)m; (void)o; (void)l; (void)ops;
	__asm__ volatile("mfence" ::: "memory");
}

/* Virtual (HHDM or kernel heap) to physical. */
uint64_t iwm_compat_vtophys(const void *);

/* ---- mbufs ------------------------------------------------------------ */

#define MSIZE		256
#define MCLBYTES	2048
#define MHLEN		200
#define MLEN		224
#define MINCLSIZE	(MHLEN + 1)
#define M_COPYALL	1000000000
#define M_DONTWAIT	M_NOWAIT
#define MT_DATA		1
#define MT_HEADER	2

#define M_EXT		0x0001
#define M_PKTHDR	0x0002
#define M_BCAST		0x0100
#define M_MCAST		0x0200

/* ICDA: the reference-counted DMA buffer behind an mbuf. */
struct icda_mbuf_buf {
	int		 refs;
	uint8_t		*buf;
	size_t		 size;
	uint64_t	 npages;	/* 1 = from the page pool */
};

struct pkthdr {
	int		 len;
	void		*ph_cookie;
	struct ifnet	*ph_ifp;
	uint16_t	 ether_vtag;
	uint16_t	 csum_flags;
};

struct mbuf_ext {
	caddr_t		 ext_buf;
	u_int		 ext_size;
};

struct mbuf {
	struct mbuf	*m_next;	/* always NULL: no chains */
	struct mbuf	*m_nextpkt;
	caddr_t		 m_data;
	int		 m_len;
	int		 m_flags;
	int		 m_type;
	struct pkthdr	 m_pkthdr;
	struct mbuf_ext	 m_ext;
	struct icda_mbuf_buf *m_buf;
};

#define mtod(m, t)	((t)((m)->m_data))
#define M_LEADINGSPACE(m) \
	((m)->m_data - (caddr_t)(m)->m_buf->buf)
#define M_TRAILINGSPACE(m) \
	((int)((m)->m_buf->size - M_LEADINGSPACE(m) - (m)->m_len))

struct mbuf *m_gethdr(int, int);
struct mbuf *m_get(int, int);
struct mbuf *m_clget(struct mbuf *, int, u_int);
struct mbuf *m_free(struct mbuf *);
void	 m_freem(struct mbuf *);
void	 m_adj(struct mbuf *, int);
struct mbuf *m_copym(struct mbuf *, int, int, int);
struct mbuf *m_dup_pkt(struct mbuf *, unsigned int, int);
struct mbuf *m_pullup(struct mbuf *, int);
struct mbuf *m_prepend(struct mbuf *, int, int);
int	 m_defrag(struct mbuf *, int);
void	 m_copydata(struct mbuf *, int, int, void *);
void	 m_align(struct mbuf *, int);
/* ICDA: header mbuf with `lead' bytes of leading space and room for len. */
struct mbuf *m_get_lead(u_int lead, u_int len);

#define MGETHDR(m, how, type)	((m) = m_gethdr((how), (type)))
#define MGET(m, how, type)	((m) = m_get((how), (type)))
#define MCLGET(m, how)		(void)m_clget((m), (how), MCLBYTES)
#define MCLGETL(m, how, size)	m_clget((m), (how), (size))
#define M_PREPEND(m, plen, how)	((m) = m_prepend((m), (plen), (how)))

/* mbuf lists and queues */
struct mbuf_list {
	struct mbuf	*ml_head;
	struct mbuf	*ml_tail;
	u_int		 ml_len;
};
#define MBUF_LIST_INITIALIZER()	{ NULL, NULL, 0 }

void	 ml_init(struct mbuf_list *);
void	 ml_enqueue(struct mbuf_list *, struct mbuf *);
struct mbuf *ml_dequeue(struct mbuf_list *);
void	 ml_purge(struct mbuf_list *);
#define ml_len(ml)	((ml)->ml_len)
#define ml_empty(ml)	((ml)->ml_len == 0)

struct mbuf_queue {
	struct mbuf_list mq_list;
	u_int		 mq_maxlen;
	u_int		 mq_drops;
};
void	 mq_init(struct mbuf_queue *, u_int, int);
int	 mq_enqueue(struct mbuf_queue *, struct mbuf *);
struct mbuf *mq_dequeue(struct mbuf_queue *);
void	 mq_purge(struct mbuf_queue *);
#define mq_len(mq)	ml_len(&(mq)->mq_list)

#ifndef PAGE_SIZE
#define PAGE_SIZE	4096ULL
#endif

/* ---- network interfaces ----------------------------------------------- */

#define IFNAMSIZ	16
#define ETHER_ADDR_LEN	6
#define ETHER_TYPE_LEN	2
#define ETHER_HDR_LEN	14
#define ETHER_CRC_LEN	4
#define ETHER_ALIGN	2
#define ETHERMTU	1500
#define ETHERTYPE_IP	0x0800
#define ETHERTYPE_ARP	0x0806
#define ETHERTYPE_EAPOL	0x888e
#define ETHER_IS_MULTICAST(a)	((a)[0] & 0x01)

struct ether_header {
	uint8_t		ether_dhost[ETHER_ADDR_LEN];
	uint8_t		ether_shost[ETHER_ADDR_LEN];
	uint16_t	ether_type;
} __packed;

extern const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN];
extern const uint8_t etheranyaddr[ETHER_ADDR_LEN];
const char *ether_sprintf(const uint8_t *);

/* LLC/SNAP */
#define LLC_SNAP_LSAP	0xaa
#define LLC_UI		0x03
#define LLC_SNAPFRAMELEN 8
struct llc {
	uint8_t		llc_dsap;
	uint8_t		llc_ssap;
	uint8_t		llc_control;
	struct {
		uint8_t		org_code[3];
		uint16_t	ether_type;
	} __packed llc_snap;
} __packed;

#define IFF_UP		0x0001
#define IFF_BROADCAST	0x0002
#define IFF_DEBUG	0x0004
#define IFF_RUNNING	0x0040
#define IFF_PROMISC	0x0100
#define IFF_ALLMULTI	0x0200
#define IFF_SIMPLEX	0x0800
#define IFF_MULTICAST	0x8000

#define LINK_STATE_UNKNOWN	0
#define LINK_STATE_INVALID	1
#define LINK_STATE_DOWN		2
#define LINK_STATE_UP		4
#define LINK_STATE_IS_UP(s)	((s) >= LINK_STATE_UP)

struct ifqueue {
	struct mbuf_list ifq_list;
	u_int		 ifq_maxlen;
	int		 ifq_oactive;
};

struct ifnet {
	void		*if_softc;
	char		 if_xname[IFNAMSIZ];
	int		 if_flags;
	int		 if_link_state;
	int		 if_timer;
	struct ifqueue	 if_snd;
	uint64_t	 if_ipackets;
	uint64_t	 if_ierrors;
	uint64_t	 if_opackets;
	uint64_t	 if_oerrors;
	uint64_t	 if_ibytes;
	uint64_t	 if_obytes;
	uint64_t	 if_imcasts;
	void		(*if_start)(struct ifnet *);
	int		(*if_ioctl)(struct ifnet *, u_long, caddr_t);
	void		(*if_watchdog)(struct ifnet *);
	/* ICDA: frames handed up by if_input() */
	void		(*if_input_icda)(struct ifnet *, struct mbuf *);
	/* ICDA: called when the link state changes */
	void		(*if_link_icda)(struct ifnet *);
	int		 if_start_pending;
};

struct arpcom {
	struct ifnet	ac_if;
	uint8_t		ac_enaddr[ETHER_ADDR_LEN];
};

int	 ifq_enqueue(struct ifqueue *, struct mbuf *);
struct mbuf *ifq_dequeue(struct ifqueue *);
void	 ifq_purge(struct ifqueue *);
#define ifq_len(q)		ml_len(&(q)->ifq_list)
#define ifq_set_oactive(q)	((q)->ifq_oactive = 1)
#define ifq_clr_oactive(q)	((q)->ifq_oactive = 0)
#define ifq_is_oactive(q)	((q)->ifq_oactive)
#define ifq_empty(q)		(ifq_len(q) == 0)

void	 if_input(struct ifnet *, struct mbuf_list *);
void	 if_start(struct ifnet *);
void	 if_link_state_change(struct ifnet *);
int	 if_setlladdr(struct ifnet *, const uint8_t *);

/* ---- PCI -------------------------------------------------------------- */

#define PCI_COMMAND_STATUS_REG		0x04
#define PCI_COMMAND_INTERRUPT_DISABLE	0x00000400
#define PCI_MAPREG_START		0x10
#define PCI_CAP_PCIEXPRESS		0x10
#define PCI_PCIE_DCSR2			0x28
#define PCI_PCIE_DCSR2_LTREN		0x00000400
#define PCI_PCIE_LCSR			0x10
#define PCI_PCIE_LCSR_ASPM_L0S		0x00000001
#define PCI_PCIE_LCSR_ASPM_L1		0x00000002

pcireg_t pci_conf_read(pci_chipset_tag_t, pcitag_t, int);
void	 pci_conf_write(pci_chipset_tag_t, pcitag_t, int, pcireg_t);

/* ---- firmware --------------------------------------------------------- */

/* The only image is iwlwifi-8000C-36.ucode, embedded in the kernel. */
int	 loadfirmware(const char *, u_char **, size_t *);

/* ---- device ----------------------------------------------------------- */

struct device {
	char	dv_xname[16];
};

/* ---- diagnostics ------------------------------------------------------ */

/* printf() output also lands in this ring; wifi.c exposes it. */
#define IWM_LOG_SIZE	32768
size_t	 iwm_compat_log_read(char *, size_t);
void	 iwm_compat_log_clear(void);
/* 1: printf also writes to the text console (boot bring-up). */
void	 iwm_compat_set_console(int);

#endif /* ICDA_IWM_COMPAT_H */
