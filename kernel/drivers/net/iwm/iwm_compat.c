/*
 * ICDA port: OpenBSD kernel interfaces for iwm(4) on ICDA.
 * See iwm_compat.h for the execution model.
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

#include "iwm_compat.h"

#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "proc/sched.h"
#include "drivers/serial/serial.h"
#include "drivers/console/console.h"

#define DMA_LIMIT	0xFFFFFFFFULL	/* keep all DMA below 4 GiB */

/* ---- libc ------------------------------------------------------------- */

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n >= 8) {
		*(uint64_t *)(void *)d = *(const uint64_t *)(const void *)s;
		d += 8; s += 8; n -= 8;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d == s || n == 0)
		return dst;
	if (d < s || d >= s + n)
		return memcpy(dst, src, n);
	while (n--)
		d[n] = s[n];
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;

	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return (int)*x - (int)*y;
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);

	if (size) {
		size_t c = n < size - 1 ? n : size - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (int)(uint8_t)*a - (int)(uint8_t)*b;
		if (*a == '\0')
			break;
	}
	return 0;
}

int
timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const volatile uint8_t *x = a, *y = b;
	uint8_t acc = 0;

	while (n--)
		acc |= *x++ ^ *y++;
	return acc != 0;
}

void
explicit_bzero(void *p, size_t n)
{
	volatile uint8_t *d = p;

	while (n--)
		*d++ = 0;
}

/* Minimal vsnprintf: %[-0][width][.prec][hh|h|l|ll|z|j|t](d|i|u|x|X|o|c|s|p|%) */
struct fmtbuf {
	char	*buf;
	size_t	 cap;
	size_t	 len;
};

static void
fmt_putc(struct fmtbuf *f, char c)
{
	if (f->len + 1 < f->cap)
		f->buf[f->len] = c;
	f->len++;
}

static void
fmt_pad(struct fmtbuf *f, char c, int n)
{
	while (n-- > 0)
		fmt_putc(f, c);
}

int
vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
	struct fmtbuf f = { buf, cap, 0 };

	for (; *fmt; fmt++) {
		int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
		uint64_t uv;
		int neg, base, upper, n;
		char tmp[24];
		const char *s;

		if (*fmt != '%') {
			fmt_putc(&f, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt == '+' || *fmt == ' ' || *fmt == '#')
				;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
		}
		for (;; fmt++) {
			if (*fmt == 'h')
				;
			else if (*fmt == 'l')
				lng++;
			else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't')
				lng = 2;
			else
				break;
		}
		switch (*fmt) {
		case '\0':
			fmt--;
			continue;
		case '%':
			fmt_putc(&f, '%');
			continue;
		case 'c':
			fmt_pad(&f, ' ', left ? 0 : width - 1);
			fmt_putc(&f, (char)va_arg(ap, int));
			fmt_pad(&f, ' ', left ? width - 1 : 0);
			continue;
		case 's':
			s = va_arg(ap, const char *);
			if (s == NULL)
				s = "(null)";
			n = 0;
			while (s[n] && (prec < 0 || n < prec))
				n++;
			fmt_pad(&f, ' ', left ? 0 : width - n);
			for (int i = 0; i < n; i++)
				fmt_putc(&f, s[i]);
			fmt_pad(&f, ' ', left ? width - n : 0);
			continue;
		case 'p':
			uv = (uint64_t)(uintptr_t)va_arg(ap, void *);
			fmt_putc(&f, '0');
			fmt_putc(&f, 'x');
			base = 16; upper = 0; neg = 0;
			goto number;
		case 'd':
		case 'i': {
			int64_t sv = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			neg = sv < 0;
			uv = neg ? (uint64_t)(-sv) : (uint64_t)sv;
			base = 10; upper = 0;
			goto number;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			uv = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
			neg = 0;
			base = (*fmt == 'u') ? 10 : (*fmt == 'o') ? 8 : 16;
			upper = (*fmt == 'X');
		number:
			n = 0;
			do {
				int d = (int)(uv % (uint64_t)base);
				tmp[n++] = (char)(d < 10 ? '0' + d :
				    (upper ? 'A' : 'a') + d - 10);
				uv /= (uint64_t)base;
			} while (uv && n < (int)sizeof(tmp));
			{
				int digits = n;

				if (neg)
					width--;
				if (!left && zero) {
					if (neg)
						fmt_putc(&f, '-');
					fmt_pad(&f, '0', width - digits);
				} else if (!left) {
					fmt_pad(&f, ' ', width - digits);
					if (neg)
						fmt_putc(&f, '-');
				} else if (neg)
					fmt_putc(&f, '-');
				while (n > 0)
					fmt_putc(&f, tmp[--n]);
				if (left)
					fmt_pad(&f, ' ', width - digits);
			}
			continue;
		default:
			fmt_putc(&f, '%');
			fmt_putc(&f, *fmt);
			continue;
		}
	}
	if (cap)
		buf[f.len < cap ? f.len : cap - 1] = '\0';
	return (int)f.len;
}

int
snprintf(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
	return n;
}

/* ---- log ring --------------------------------------------------------- */

static char	log_ring[IWM_LOG_SIZE];
static size_t	log_head;	/* next write position */
static size_t	log_used;
static int	log_console;
static int	log_bol = 1;

void
iwm_compat_set_console(int on)
{
	log_console = on;
}

static void
log_putc(char c)
{
	log_ring[log_head] = c;
	log_head = (log_head + 1) % IWM_LOG_SIZE;
	if (log_used < IWM_LOG_SIZE)
		log_used++;
}

static void
log_write(const char *s)
{
	char stamp[24];
	const char *p;

	for (p = s; *p; p++) {
		if (log_bol) {
			uint64_t ms = iwm_compat_nsecuptime() / 1000000ULL;
			snprintf(stamp, sizeof(stamp), "[%5u.%03u] ",
			    (unsigned)(ms / 1000), (unsigned)(ms % 1000));
			for (const char *q = stamp; *q; q++)
				log_putc(*q);
			log_bol = 0;
		}
		log_putc(*p);
		if (*p == '\n')
			log_bol = 1;
	}
	serial_write(s);
	if (log_console)
		console_write(s, CONSOLE_STYLE_MUTED);
}

size_t
iwm_compat_log_read(char *out, size_t cap)
{
	size_t start, n, i;

	if (cap == 0)
		return 0;
	n = log_used < cap - 1 ? log_used : cap - 1;
	/* newest `n' bytes, oldest first */
	start = (log_head + IWM_LOG_SIZE - n) % IWM_LOG_SIZE;
	for (i = 0; i < n; i++)
		out[i] = log_ring[(start + i) % IWM_LOG_SIZE];
	out[n] = '\0';
	return n;
}

void
iwm_compat_log_clear(void)
{
	log_head = log_used = 0;
	log_bol = 1;
}

int
printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_write(buf);
	return n;
}

void
panic(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_write("iwm: panic: ");
	log_write(buf);
	log_write("\n");
	console_write("iwm: panic: ", CONSOLE_STYLE_ERROR);
	console_write(buf, CONSOLE_STYLE_ERROR);
	console_write("\n", CONSOLE_STYLE_ERROR);
	/* The Wi-Fi thread parks; the rest of the system keeps running. */
	for (;;)
		sched_sleep(100);
}

/* ---- randomness ------------------------------------------------------- */

static inline uint64_t
rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static int
have_rdrand(void)
{
	static int cached = -1;
	uint32_t a, b, c, d;

	if (cached < 0) {
		__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
		    : "a"(1), "c"(0));
		cached = (c >> 30) & 1;
	}
	return cached;
}

static uint64_t
rand64(void)
{
	static uint64_t s0 = 0x9E3779B97F4A7C15ULL, s1 = 0xD1B54A32D192ED03ULL;
	uint64_t v = 0, x, y;
	int i;

	if (have_rdrand()) {
		for (i = 0; i < 16; i++) {
			unsigned char ok;
			__asm__ volatile("rdrand %0; setc %1"
			    : "=r"(v), "=qm"(ok));
			if (ok)
				break;
		}
	}
	/* xorshift128+ mixed with the TSC, folded into whatever rdrand gave */
	s0 ^= rdtsc();
	x = s0;
	y = s1;
	s0 = y;
	x ^= x << 23;
	s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
	return v ^ (s1 + y);
}

void
arc4random_buf(void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		uint64_t v = rand64();
		size_t c = n < 8 ? n : 8;
		memcpy(p, &v, c);
		p += c;
		n -= c;
	}
}

uint32_t
arc4random(void)
{
	return (uint32_t)rand64();
}

/* ---- malloc(9) -------------------------------------------------------- */

extern const uint8_t iwm_fw_8000c_start[];
extern const uint8_t iwm_fw_8000c_end[];

static int
is_firmware_blob(const void *p)
{
	return (const uint8_t *)p >= iwm_fw_8000c_start &&
	    (const uint8_t *)p < iwm_fw_8000c_end;
}

void *
malloc(size_t size, int type, int flags)
{
	void *p;

	(void)type;
	p = kmalloc(size ? size : 1);
	if (p && (flags & M_ZERO))
		memset(p, 0, size);
	return p;
}

void *
mallocarray(size_t n, size_t size, int type, int flags)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return malloc(n * size, type, flags);
}

void
free(void *p, int type, size_t size)
{
	(void)type;
	(void)size;
	if (p == NULL || is_firmware_blob(p))
		return;
	kfree(p);
}

/* ---- time ------------------------------------------------------------- */

static uint64_t tsc_base;
static uint64_t tsc_per_us = 2000;	/* refined by calibration */

static void
tsc_calibrate(void)
{
	uint64_t t0, t1, k0, k1, start;

	tsc_base = rdtsc();
	/* Align to a 100 Hz tick edge, then count TSC over 10 ticks. */
	k0 = sched_ticks();
	start = rdtsc();
	while (sched_ticks() == k0) {
		if (rdtsc() - start > 20000000000ULL)
			return;		/* timer not running; keep default */
		__asm__ volatile("pause");
	}
	k0 = sched_ticks();
	t0 = rdtsc();
	while ((k1 = sched_ticks()) < k0 + 10)
		__asm__ volatile("pause");
	t1 = rdtsc();
	/* 10 ticks = 100 ms */
	if (t1 > t0)
		tsc_per_us = (t1 - t0) / ((k1 - k0) * 10000ULL);
	if (tsc_per_us == 0)
		tsc_per_us = 1;
}

static int tsc_ready;

static void
tsc_init(void)
{
	if (!tsc_ready) {
		tsc_ready = 1;
		tsc_calibrate();
		printf("iwm-compat: TSC %u MHz\n", (unsigned)tsc_per_us);
	}
}

uint64_t
iwm_compat_nsecuptime(void)
{
	return (rdtsc() - tsc_base) * 1000ULL / tsc_per_us;
}

void
getmicrouptime(struct timeval *tv)
{
	uint64_t us = iwm_compat_nsecuptime() / 1000ULL;

	tv->tv_sec = (int64_t)(us / 1000000ULL);
	tv->tv_usec = (int64_t)(us % 1000000ULL);
}

void
microuptime(struct timeval *tv)
{
	getmicrouptime(tv);
}

void
DELAY(unsigned int usec)
{
	uint64_t end = rdtsc() + (uint64_t)usec * tsc_per_us;

	while (rdtsc() < end)
		__asm__ volatile("pause");
}

/* ---- sleep/wakeup ----------------------------------------------------- */

struct sleeper {
	const volatile void	*ident;
	int			 woken;
	struct sleeper		*prev;
};

static struct sleeper	*sleepers;
static int		(*poll_fn)(void *);
static void		*poll_arg;
static int		 polling;

void
iwm_compat_set_poll(int (*fn)(void *), void *arg)
{
	tsc_init();
	poll_fn = fn;
	poll_arg = arg;
}

int
tsleep_nsec(const volatile void *ident, int prio, const char *wmesg,
    uint64_t nsecs)
{
	struct sleeper s;
	uint64_t start, deadline, last_yield;
	int rv = 0;

	(void)prio;
	(void)wmesg;
	s.ident = ident;
	s.woken = 0;
	s.prev = sleepers;
	sleepers = &s;

	start = iwm_compat_nsecuptime();
	deadline = (nsecs == INFSLP) ? UINT64_MAX : start + nsecs;
	last_yield = start;
	for (;;) {
		if (poll_fn && !polling) {
			polling = 1;
			poll_fn(poll_arg);
			polling = 0;
		}
		if (s.woken)
			break;
		uint64_t now = iwm_compat_nsecuptime();
		if (now >= deadline) {
			rv = EWOULDBLOCK;
			break;
		}
		/* Spin briefly for fast command replies, then share the CPU. */
		if (now - last_yield > 2000000ULL) {
			sched_yield();
			last_yield = iwm_compat_nsecuptime();
		} else
			DELAY(20);
	}
	sleepers = s.prev;
	return rv;
}

void
wakeup(const volatile void *ident)
{
	struct sleeper *s;

	for (s = sleepers; s != NULL; s = s->prev)
		if (s->ident == ident)
			s->woken = 1;
}

/* ---- refcnt ----------------------------------------------------------- */

void
refcnt_init(struct refcnt *r)
{
	r->r_refs = 1;
}

void
refcnt_take(struct refcnt *r)
{
	r->r_refs++;
}

int
refcnt_rele(struct refcnt *r)
{
	if (r->r_refs == 0)
		return 1;
	return --r->r_refs == 0;
}

void
refcnt_rele_wake(struct refcnt *r)
{
	if (refcnt_rele(r))
		wakeup(r);
}

void
refcnt_finalize(struct refcnt *r, const char *wmesg)
{
	(void)wmesg;
	/*
	 * Nothing runs concurrently with the caller, so references still held
	 * belong to tasks that were queued and have been deleted already.
	 */
	r->r_refs = 0;
}

/* ---- tasks ------------------------------------------------------------ */

static struct taskq systq_store = { "systq", NULL, NULL, NULL };
struct taskq *const systq = &systq_store;
static struct taskq *taskqs = &systq_store;

void
task_set(struct task *t, void (*fn)(void *), void *arg)
{
	t->t_func = fn;
	t->t_arg = arg;
	t->t_pending = 0;
	t->t_next = NULL;
}

int
task_add(struct taskq *tq, struct task *t)
{
	if (t->t_pending)
		return 0;
	t->t_pending = 1;
	t->t_next = NULL;
	if (tq->tq_tail)
		tq->tq_tail->t_next = t;
	else
		tq->tq_head = t;
	tq->tq_tail = t;
	return 1;
}

int
task_del(struct taskq *tq, struct task *t)
{
	struct task **pp, *prev = NULL;

	if (!t->t_pending)
		return 0;
	for (pp = &tq->tq_head; *pp; prev = *pp, pp = &(*pp)->t_next) {
		if (*pp == t) {
			*pp = t->t_next;
			if (tq->tq_tail == t)
				tq->tq_tail = prev;
			t->t_pending = 0;
			t->t_next = NULL;
			return 1;
		}
	}
	return 0;
}

struct taskq *
taskq_create(const char *name, unsigned int nthreads, int ipl,
    unsigned int flags)
{
	struct taskq *tq;

	(void)nthreads; (void)ipl; (void)flags;
	tq = malloc(sizeof(*tq), M_DEVBUF, M_ZERO);
	if (tq == NULL)
		return NULL;
	tq->tq_name = name;
	tq->tq_next = taskqs;
	taskqs = tq;
	return tq;
}

int
iwm_compat_run_tasks(void)
{
	struct taskq *tq;
	int ran = 0, budget = 16;

	for (tq = taskqs; tq != NULL; tq = tq->tq_next) {
		while (tq->tq_head != NULL && budget-- > 0) {
			struct task *t = tq->tq_head;
			tq->tq_head = t->t_next;
			if (tq->tq_head == NULL)
				tq->tq_tail = NULL;
			t->t_next = NULL;
			t->t_pending = 0;
			t->t_func(t->t_arg);
			ran++;
		}
	}
	return ran;
}

/* ---- timeouts --------------------------------------------------------- */

static struct timeout *timeouts;

void
timeout_set(struct timeout *to, void (*fn)(void *), void *arg)
{
	to->to_func = fn;
	to->to_arg = arg;
	to->to_pending = 0;
	to->to_next = NULL;
}

static int
timeout_add_nsec(struct timeout *to, uint64_t ns)
{
	int was = to->to_pending;

	if (!was) {
		to->to_next = timeouts;
		timeouts = to;
		to->to_pending = 1;
	}
	to->to_deadline = iwm_compat_nsecuptime() + ns;
	return !was;
}

int
timeout_add_msec(struct timeout *to, int ms)
{
	return timeout_add_nsec(to, MSEC_TO_NSEC(ms < 0 ? 0 : ms));
}

int
timeout_add_sec(struct timeout *to, int s)
{
	return timeout_add_nsec(to, SEC_TO_NSEC(s < 0 ? 0 : s));
}

int
timeout_add_usec(struct timeout *to, int us)
{
	return timeout_add_nsec(to, USEC_TO_NSEC(us < 0 ? 0 : us));
}

int
timeout_del(struct timeout *to)
{
	struct timeout **pp;

	if (!to->to_pending)
		return 0;
	for (pp = &timeouts; *pp; pp = &(*pp)->to_next) {
		if (*pp == to) {
			*pp = to->to_next;
			break;
		}
	}
	to->to_pending = 0;
	to->to_next = NULL;
	return 1;
}

int
iwm_compat_run_timeouts(void)
{
	uint64_t now = iwm_compat_nsecuptime();
	struct timeout *to;
	int ran = 0;

again:
	for (to = timeouts; to != NULL; to = to->to_next) {
		if (to->to_deadline <= now) {
			timeout_del(to);
			to->to_func(to->to_arg);
			if (++ran < 32)
				goto again;	/* list may have changed */
			break;
		}
	}
	return ran;
}

/* ---- DMA memory ------------------------------------------------------- */

uint64_t
iwm_compat_vtophys(const void *v)
{
	uint64_t va = (uint64_t)(uintptr_t)v;

	if (va >= PHYSICAL_BASE && va < KERNEL_VMA)
		return VIRT_TO_PHYS(va);
	return vmm_virt_to_phys(vmm_kernel_address_space(), va & ~0xFFFULL) +
	    (va & 0xFFFULL);
}

int
bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t map;

	(void)t; (void)maxsegsz; (void)boundary; (void)flags;
	map = malloc(sizeof(*map), M_DEVBUF, M_ZERO);
	if (map == NULL)
		return ENOMEM;
	map->_dm_size = size;
	map->_dm_segcnt = nsegments > ICDA_DMA_MAXSEGS ?
	    ICDA_DMA_MAXSEGS : nsegments;
	*mapp = map;
	return 0;
}

void
bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{
	(void)t;
	free(map, M_DEVBUF, sizeof(*map));
}

int
bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf,
    bus_size_t len, void *p, int flags)
{
	uint8_t *va = buf;
	bus_size_t left = len;
	int n = 0;

	(void)t; (void)p; (void)flags;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	if (len > map->_dm_size)
		return EINVAL;
	while (left > 0) {
		uint64_t pa = iwm_compat_vtophys(va);
		bus_size_t chunk = PAGE_SIZE - (pa & (PAGE_SIZE - 1));

		if (chunk > left)
			chunk = left;
		if (pa == 0 || pa + chunk > DMA_LIMIT + 1)
			return EINVAL;
		if (n > 0 && map->dm_segs[n - 1].ds_addr +
		    map->dm_segs[n - 1].ds_len == pa) {
			map->dm_segs[n - 1].ds_len += chunk;
		} else {
			if (n == map->_dm_segcnt)
				return EFBIG;
			map->dm_segs[n].ds_addr = pa;
			map->dm_segs[n].ds_len = chunk;
			n++;
		}
		va += chunk;
		left -= chunk;
	}
	map->dm_nsegs = n;
	map->dm_mapsize = len;
	return 0;
}

int
bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m,
    int flags)
{
	return bus_dmamap_load(t, map, m->m_data, (bus_size_t)m->m_len, NULL,
	    flags);
}

void
bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{
	(void)t;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
}

int
bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs,
    int flags)
{
	uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t pa;

	(void)t; (void)boundary; (void)nsegs; (void)flags;
	if (npages == 0)
		npages = 1;
	if (align < PAGE_SIZE)
		align = PAGE_SIZE;
	pa = pmm_alloc_contiguous_aligned_below(npages, align, DMA_LIMIT);
	if (pa == 0)
		return ENOMEM;
	segs[0].ds_addr = pa;
	segs[0].ds_len = size;
	segs[0]._ds_vaddr = PHYS_TO_VIRT(pa);
	segs[0]._ds_npages = npages;
	memset(segs[0]._ds_vaddr, 0, npages * PAGE_SIZE);
	*rsegs = 1;
	return 0;
}

void
bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	(void)t; (void)nsegs;
	if (segs[0]._ds_npages)
		pmm_free_range(segs[0].ds_addr, segs[0]._ds_npages);
	segs[0]._ds_npages = 0;
}

int
bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs,
    size_t size, caddr_t *kvap, int flags)
{
	(void)t; (void)nsegs; (void)size; (void)flags;
	*kvap = segs[0]._ds_vaddr;
	return 0;
}

void
bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
	(void)t; (void)kva; (void)size;
}

/* ---- mbufs ------------------------------------------------------------ */

/* Free lists: 4 KiB DMA pages, buffer headers and mbuf headers. */
struct pool_link {
	struct pool_link *next;
};

static struct pool_link *page_pool;
static struct pool_link *mbuf_pool;
static struct pool_link *bufhdr_pool;
static unsigned int page_pool_total;

#define PAGE_POOL_CHUNK	32

static int
page_pool_grow(void)
{
	uint64_t n = PAGE_POOL_CHUNK, pa = 0;

	while (n > 0 &&
	    (pa = pmm_alloc_contiguous_below(n, DMA_LIMIT)) == 0)
		n /= 2;
	if (pa == 0)
		return ENOMEM;
	for (uint64_t i = 0; i < n; i++) {
		struct pool_link *l = PHYS_TO_VIRT(pa + i * PAGE_SIZE);
		l->next = page_pool;
		page_pool = l;
	}
	page_pool_total += (unsigned int)n;
	return 0;
}

static struct icda_mbuf_buf *
mbuf_buf_alloc(size_t size)
{
	struct icda_mbuf_buf *b;

	if (bufhdr_pool != NULL) {
		b = (struct icda_mbuf_buf *)(void *)bufhdr_pool;
		bufhdr_pool = bufhdr_pool->next;
	} else if ((b = kmalloc(sizeof(*b))) == NULL)
		return NULL;

	if (size <= PAGE_SIZE) {
		if (page_pool == NULL && page_pool_grow() != 0)
			goto fail;
		b->buf = (uint8_t *)(void *)page_pool;
		page_pool = page_pool->next;
		b->size = PAGE_SIZE;
		b->npages = 1;
	} else {
		uint64_t np = (size + PAGE_SIZE - 1) / PAGE_SIZE;
		uint64_t pa = pmm_alloc_contiguous_below(np, DMA_LIMIT);
		if (pa == 0)
			goto fail;
		b->buf = PHYS_TO_VIRT(pa);
		b->size = np * PAGE_SIZE;
		b->npages = np;
	}
	b->refs = 1;
	return b;
fail:
	((struct pool_link *)(void *)b)->next = bufhdr_pool;
	bufhdr_pool = (struct pool_link *)(void *)b;
	return NULL;
}

static void
mbuf_buf_rele(struct icda_mbuf_buf *b)
{
	if (b == NULL || --b->refs > 0)
		return;
	if (b->npages == 1) {
		struct pool_link *l = (struct pool_link *)(void *)b->buf;
		l->next = page_pool;
		page_pool = l;
	} else
		pmm_free_range(VIRT_TO_PHYS(b->buf), b->npages);
	((struct pool_link *)(void *)b)->next = bufhdr_pool;
	bufhdr_pool = (struct pool_link *)(void *)b;
}

static struct mbuf *
mbuf_hdr_alloc(void)
{
	struct mbuf *m;

	if (mbuf_pool != NULL) {
		m = (struct mbuf *)(void *)mbuf_pool;
		mbuf_pool = mbuf_pool->next;
	} else if ((m = kmalloc(sizeof(*m))) == NULL)
		return NULL;
	memset(m, 0, sizeof(*m));
	return m;
}

static void
mbuf_set_buf(struct mbuf *m, struct icda_mbuf_buf *b)
{
	m->m_buf = b;
	m->m_ext.ext_buf = (caddr_t)b->buf;
	m->m_ext.ext_size = (u_int)b->size;
	m->m_data = (caddr_t)b->buf;
	m->m_flags |= M_EXT;
}

static struct mbuf *
mbuf_alloc(size_t size, int type)
{
	struct mbuf *m = mbuf_hdr_alloc();
	struct icda_mbuf_buf *b;

	if (m == NULL)
		return NULL;
	b = mbuf_buf_alloc(size);
	if (b == NULL) {
		((struct pool_link *)(void *)m)->next = mbuf_pool;
		mbuf_pool = (struct pool_link *)(void *)m;
		return NULL;
	}
	mbuf_set_buf(m, b);
	m->m_type = type;
	m->m_flags |= M_PKTHDR;
	return m;
}

struct mbuf *
m_gethdr(int how, int type)
{
	(void)how;
	return mbuf_alloc(PAGE_SIZE, type);
}

struct mbuf *
m_get(int how, int type)
{
	return m_gethdr(how, type);
}

struct mbuf *
m_get_lead(u_int lead, u_int len)
{
	struct mbuf *m = mbuf_alloc(lead + len, MT_DATA);

	if (m == NULL)
		return NULL;
	m->m_data += lead;
	m->m_len = m->m_pkthdr.len = (int)len;
	return m;
}

struct mbuf *
m_clget(struct mbuf *m, int how, u_int size)
{
	struct icda_mbuf_buf *b;

	if (m == NULL) {
		m = mbuf_alloc(size, MT_DATA);
		if (m != NULL)
			m->m_len = m->m_pkthdr.len = 0;
		return m;
	}
	(void)how;
	if (m->m_buf != NULL && m->m_buf->size >= size && m->m_buf->refs == 1) {
		m->m_data = (caddr_t)m->m_buf->buf;
		m->m_flags |= M_EXT;
		return m;
	}
	b = mbuf_buf_alloc(size);
	if (b == NULL) {
		m->m_flags &= ~M_EXT;
		return NULL;
	}
	mbuf_buf_rele(m->m_buf);
	mbuf_set_buf(m, b);
	return m;
}

struct mbuf *
m_free(struct mbuf *m)
{
	struct mbuf *n;

	if (m == NULL)
		return NULL;
	n = m->m_next;
	mbuf_buf_rele(m->m_buf);
	((struct pool_link *)(void *)m)->next = mbuf_pool;
	mbuf_pool = (struct pool_link *)(void *)m;
	return n;
}

void
m_freem(struct mbuf *m)
{
	while (m != NULL)
		m = m_free(m);
}

void
m_adj(struct mbuf *m, int req)
{
	if (m == NULL)
		return;
	if (req >= 0) {
		if (req > m->m_len)
			req = m->m_len;
		m->m_data += req;
		m->m_len -= req;
	} else {
		req = -req;
		if (req > m->m_len)
			req = m->m_len;
		m->m_len -= req;
	}
	m->m_pkthdr.len = m->m_len;
}

struct mbuf *
m_copym(struct mbuf *m, int off, int len, int how)
{
	struct mbuf *n;

	(void)how;
	if (m == NULL || off > m->m_len)
		return NULL;
	n = mbuf_hdr_alloc();
	if (n == NULL)
		return NULL;
	*n = *m;
	n->m_next = n->m_nextpkt = NULL;
	n->m_buf->refs++;
	n->m_data = m->m_data + off;
	n->m_len = (len == M_COPYALL || off + len > m->m_len) ?
	    m->m_len - off : len;
	n->m_pkthdr.len = n->m_len;
	return n;
}

struct mbuf *
m_dup_pkt(struct mbuf *m, unsigned int adj, int how)
{
	struct mbuf *n;

	(void)how;
	n = m_get_lead(64 + adj, (u_int)m->m_len);
	if (n == NULL)
		return NULL;
	memcpy(n->m_data, m->m_data, (size_t)m->m_len);
	n->m_pkthdr.ph_cookie = m->m_pkthdr.ph_cookie;
	n->m_flags |= m->m_flags & (M_BCAST | M_MCAST);
	return n;
}

struct mbuf *
m_pullup(struct mbuf *m, int len)
{
	if (m != NULL && m->m_len < len) {
		m_freem(m);
		return NULL;
	}
	return m;
}

struct mbuf *
m_prepend(struct mbuf *m, int plen, int how)
{
	struct mbuf *n;

	(void)how;
	if (m == NULL)
		return NULL;
	if (M_LEADINGSPACE(m) >= plen && m->m_buf->refs == 1) {
		m->m_data -= plen;
		m->m_len += plen;
		m->m_pkthdr.len = m->m_len;
		return m;
	}
	n = m_get_lead(128, (u_int)(plen + m->m_len));
	if (n == NULL) {
		m_freem(m);
		return NULL;
	}
	memcpy(n->m_data + plen, m->m_data, (size_t)m->m_len);
	n->m_pkthdr.ph_cookie = m->m_pkthdr.ph_cookie;
	n->m_flags |= m->m_flags & (M_BCAST | M_MCAST);
	m_freem(m);
	return n;
}

int
m_defrag(struct mbuf *m, int how)
{
	(void)m; (void)how;
	return 0;
}

void
m_copydata(struct mbuf *m, int off, int len, void *buf)
{
	memcpy(buf, m->m_data + off, (size_t)len);
}

void
m_align(struct mbuf *m, int len)
{
	size_t off = (m->m_buf->size - (size_t)len) & ~(size_t)7;

	m->m_data = (caddr_t)m->m_buf->buf + off;
}

/* ---- mbuf lists and queues -------------------------------------------- */

void
ml_init(struct mbuf_list *ml)
{
	ml->ml_head = ml->ml_tail = NULL;
	ml->ml_len = 0;
}

void
ml_enqueue(struct mbuf_list *ml, struct mbuf *m)
{
	m->m_nextpkt = NULL;
	if (ml->ml_tail)
		ml->ml_tail->m_nextpkt = m;
	else
		ml->ml_head = m;
	ml->ml_tail = m;
	ml->ml_len++;
}

struct mbuf *
ml_dequeue(struct mbuf_list *ml)
{
	struct mbuf *m = ml->ml_head;

	if (m != NULL) {
		ml->ml_head = m->m_nextpkt;
		if (ml->ml_head == NULL)
			ml->ml_tail = NULL;
		m->m_nextpkt = NULL;
		ml->ml_len--;
	}
	return m;
}

void
ml_purge(struct mbuf_list *ml)
{
	struct mbuf *m;

	while ((m = ml_dequeue(ml)) != NULL)
		m_freem(m);
}

void
mq_init(struct mbuf_queue *mq, u_int maxlen, int ipl)
{
	(void)ipl;
	ml_init(&mq->mq_list);
	mq->mq_maxlen = maxlen;
	mq->mq_drops = 0;
}

int
mq_enqueue(struct mbuf_queue *mq, struct mbuf *m)
{
	if (mq->mq_maxlen && mq_len(mq) >= mq->mq_maxlen) {
		mq->mq_drops++;
		m_freem(m);
		return ENOBUFS;
	}
	ml_enqueue(&mq->mq_list, m);
	return 0;
}

struct mbuf *
mq_dequeue(struct mbuf_queue *mq)
{
	return ml_dequeue(&mq->mq_list);
}

void
mq_purge(struct mbuf_queue *mq)
{
	ml_purge(&mq->mq_list);
}

int
ifq_enqueue(struct ifqueue *ifq, struct mbuf *m)
{
	if (ifq->ifq_maxlen && ifq_len(ifq) >= ifq->ifq_maxlen) {
		m_freem(m);
		return ENOBUFS;
	}
	ml_enqueue(&ifq->ifq_list, m);
	return 0;
}

struct mbuf *
ifq_dequeue(struct ifqueue *ifq)
{
	return ml_dequeue(&ifq->ifq_list);
}

void
ifq_purge(struct ifqueue *ifq)
{
	ml_purge(&ifq->ifq_list);
}

/* ---- interfaces ------------------------------------------------------- */

const uint8_t etherbroadcastaddr[ETHER_ADDR_LEN] =
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
const uint8_t etheranyaddr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

const char *
ether_sprintf(const uint8_t *a)
{
	static char buf[4][18];
	static int which;
	char *b = buf[which++ & 3];

	snprintf(b, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
	    a[0], a[1], a[2], a[3], a[4], a[5]);
	return b;
}

void
if_input(struct ifnet *ifp, struct mbuf_list *ml)
{
	struct mbuf *m;

	while ((m = ml_dequeue(ml)) != NULL) {
		ifp->if_ipackets++;
		ifp->if_ibytes += (uint64_t)m->m_len;
		if (ifp->if_input_icda)
			ifp->if_input_icda(ifp, m);
		else
			m_freem(m);
	}
}

void
if_start(struct ifnet *ifp)
{
	/* Deferred to the driver thread's loop. */
	ifp->if_start_pending = 1;
}

void
if_link_state_change(struct ifnet *ifp)
{
	if (ifp->if_link_icda)
		ifp->if_link_icda(ifp);
}

int
if_setlladdr(struct ifnet *ifp, const uint8_t *lladdr)
{
	struct arpcom *ac = (struct arpcom *)(void *)ifp;

	memcpy(ac->ac_enaddr, lladdr, ETHER_ADDR_LEN);
	return 0;
}

/* ---- PCI -------------------------------------------------------------- */

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	(void)pc;
	return pci_read_config32(tag, (uint16_t)reg);
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t val)
{
	(void)pc;
	pci_write_config32(tag, (uint16_t)reg, val);
}

/* ---- firmware --------------------------------------------------------- */

int
loadfirmware(const char *name, u_char **bufp, size_t *lenp)
{
	size_t len = (size_t)(iwm_fw_8000c_end - iwm_fw_8000c_start);

	if (strcmp(name, "iwm-8000C-36") != 0 || len == 0)
		return ENOENT;
	/* Read-only and never freed: free() ignores it. */
	*bufp = (u_char *)(uintptr_t)iwm_fw_8000c_start;
	*lenp = len;
	return 0;
}
