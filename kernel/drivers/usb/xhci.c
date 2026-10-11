/* USB: the xHCI host controller, device enumeration, HID keyboards and mice,
 * and mass storage (flash drives, external disks).
 *
 * One kernel thread ("usb") owns the controller.  It wakes every tick, takes
 * the controller's events (command completions, transfers, port changes),
 * hands keyboard and mouse reports to the input drivers and attaches devices
 * plugged in since the last look.  Mass storage reads and writes run on the
 * caller's thread and wait for their own transfer events; everything runs
 * under the kernel lock, so the two never interleave.
 *
 * Only devices on the controller's own ports are supported (no hubs yet).
 * Keyboards and mice use the boot protocol; QEMU's tablet (absolute pointer)
 * is recognised by its report size.  Storage uses SCSI over bulk-only
 * transport: TEST UNIT READY, READ CAPACITY, READ(10), WRITE(10). */

#include "xhci.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../storage/block.h"
#include "../storage/partition.h"
#include "../input/keyboard.h"
#include "../input/mouse.h"
#include "../../fs/volumes.h"
#include "../../memory/vmm.h"
#include "../../memory/pmm.h"
#include "../../cpu/tsc.h"
#include "../../proc/sched.h"

/* ---- registers ------------------------------------------------------------- */

#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(n)    (0x400 + 0x10 * (n))

#define CMD_RUN         (1u << 0)
#define CMD_HCRST       (1u << 1)
#define STS_HCH         (1u << 0)
#define STS_CNR         (1u << 11)

#define PORT_CCS        (1u << 0)
#define PORT_PED        (1u << 1)
#define PORT_PR         (1u << 4)
#define PORT_PP         (1u << 9)
#define PORT_SPEED(p)   (((p) >> 10) & 0xF)
#define PORT_CHANGES    (0x7Fu << 17)      /* CSC PEC WRC OCC PRC PLC CEC: write 1 to clear */
#define PORT_PRC        (1u << 21)

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND  33
#define TRB_EV_PORT     34

#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_ISP         (1u << 2)

#define CC_SUCCESS      1
#define CC_SHORT        13

#define RING_TRBS       256
#define MAX_DEV         8
#define MAX_KBD_KEYS    6

typedef struct { uint32_t d[4]; } trb_t;

typedef struct {
    trb_t   *trb;
    uint64_t phys;
    uint32_t idx;
    uint32_t cycle;
} ring_t;

/* ---- controller state ------------------------------------------------------ */

static volatile uint8_t *cap, *op, *rt;
static volatile uint32_t *db;
static uint32_t nports, nslots, ctx_size;
static uint64_t *dcbaa;
static ring_t cmd_ring, ev_ring;
static uint64_t erst_phys;
static int xhci_ok;

static struct {
    volatile int done;
    uint32_t cc, slot;
} cmd_result;

enum { DEV_NONE, DEV_KBD, DEV_MOUSE, DEV_TABLET, DEV_STORAGE, DEV_HUB, DEV_OTHER };

typedef struct {
    int      used, kind, port, slot, speed;
    uint64_t out_ctx_phys, in_ctx_phys;
    uint8_t *in_ctx;
    ring_t   ep0;
    ring_t   ep_in, ep_out;        /* interrupt IN (HID) / bulk IN and OUT (storage) */
    int      dci_in, dci_out;
    uint32_t mps_in, mps_out;
    uint8_t *buf;                  /* a page for control data and HID reports */
    uint64_t buf_phys;
    uint8_t  iface;
    /* where it hangs: a root port (parent -1) or a hub's port */
    int      parent, hub_port, depth, hub_nports;
    uint32_t route;
    /* transfer completion, set by the event handler */
    volatile int xfer_done[32];
    uint32_t xfer_cc[32], xfer_left[32];
    /* HID */
    uint8_t  keys[MAX_KBD_KEYS], mods;
    /* storage */
    uint32_t tag, block_size;
    uint64_t blocks;
    uint8_t *bounce;
    uint64_t bounce_phys;
    block_device_t bdev;
    char     name[8];
} usb_dev_t;

static usb_dev_t devs[MAX_DEV];

static inline uint32_t rd32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }
static inline void wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(base + off) = (uint32_t)v;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static void log_hex(const char *what, uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = hex[(v >> (28 - 4 * i)) & 15];
    b[8] = 0;
    serial_write(what);
    serial_write(b);
    serial_write("\n");
}

static void zero(void *p, uint64_t n) { for (uint64_t i = 0; i < n; i++) ((uint8_t *)p)[i] = 0; }

static void *page(uint64_t *phys) {
    uint64_t p = pmm_alloc();
    if (!p) return 0;
    zero(PHYS_TO_VIRT(p), 4096);
    *phys = p;
    return PHYS_TO_VIRT(p);
}

static int wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint64_t us) {
    uint64_t t0 = tsc_us();
    while ((rd32(base, off) & mask) != want) {
        if (tsc_us() - t0 > us) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

/* ---- rings ------------------------------------------------------------------- */

static int ring_init(ring_t *r) {
    r->trb = (trb_t *)page(&r->phys);
    if (!r->trb) return -1;
    r->idx = 0;
    r->cycle = 1;
    /* the last TRB links back to the start and flips the cycle */
    r->trb[RING_TRBS - 1].d[0] = (uint32_t)r->phys;
    r->trb[RING_TRBS - 1].d[1] = (uint32_t)(r->phys >> 32);
    r->trb[RING_TRBS - 1].d[3] = (TRB_LINK << 10) | (1u << 1);   /* toggle cycle */
    return 0;
}

static uint64_t ring_push(ring_t *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    trb_t *t = &r->trb[r->idx];
    uint64_t at = r->phys + (uint64_t)r->idx * sizeof(trb_t);
    t->d[0] = d0;
    t->d[1] = d1;
    t->d[2] = d2;
    __asm__ volatile("" ::: "memory");
    t->d[3] = (d3 & ~1u) | r->cycle;
    if (++r->idx == RING_TRBS - 1) {
        trb_t *link = &r->trb[RING_TRBS - 1];
        link->d[3] = (link->d[3] & ~1u) | r->cycle;
        r->idx = 0;
        r->cycle ^= 1;
    }
    return at;
}

/* ---- events ------------------------------------------------------------------ */

static usb_dev_t *dev_of_slot(uint32_t slot) {
    for (int i = 0; i < MAX_DEV; i++) if (devs[i].used && devs[i].slot == (int)slot) return &devs[i];
    return 0;
}

static void hid_report(usb_dev_t *d, uint32_t len);
static void hid_queue(usb_dev_t *d);
static volatile int port_change;

static void events(void) {
    int any = 0;
    if (!xhci_ok) return;
    for (;;) {
        trb_t *t = &ev_ring.trb[ev_ring.idx];
        uint32_t type;
        if ((t->d[3] & 1u) != ev_ring.cycle) break;
        type = (t->d[3] >> 10) & 0x3F;
        if (type == TRB_EV_COMMAND) {
            cmd_result.cc = t->d[2] >> 24;
            cmd_result.slot = t->d[3] >> 24;
            cmd_result.done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            usb_dev_t *d = dev_of_slot(t->d[3] >> 24);
            uint32_t ep = (t->d[3] >> 16) & 0x1F;
            if (d && ep < 32) {
                d->xfer_cc[ep] = t->d[2] >> 24;
                d->xfer_left[ep] = t->d[2] & 0xFFFFFF;
                d->xfer_done[ep] = 1;
                if ((d->kind == DEV_KBD || d->kind == DEV_MOUSE || d->kind == DEV_TABLET) && (int)ep == d->dci_in) {
                    if (d->xfer_cc[ep] == CC_SUCCESS || d->xfer_cc[ep] == CC_SHORT)
                        hid_report(d, d->mps_in - d->xfer_left[ep]);
                    hid_queue(d);
                }
            }
        } else if (type == TRB_EV_PORT) {
            port_change = 1;
        }
        any = 1;
        if (++ev_ring.idx == RING_TRBS) {
            ev_ring.idx = 0;
            ev_ring.cycle ^= 1;
        }
    }
    if (any) wr64(rt, 0x20 + 0x18, (ev_ring.phys + (uint64_t)ev_ring.idx * sizeof(trb_t)) | (1u << 3));
}

static int command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *slot_out) {
    uint64_t t0 = tsc_us();
    cmd_result.done = 0;
    ring_push(&cmd_ring, d0, d1, d2, d3);
    db[0] = 0;
    while (!cmd_result.done) {
        events();
        if (tsc_us() - t0 > 500000) {
            serial_write("[usb] command timed out\n");
            return -1;
        }
    }
    if (slot_out) *slot_out = cmd_result.slot;
    return cmd_result.cc == CC_SUCCESS ? 0 : -(int)cmd_result.cc;
}

static int wait_xfer(usb_dev_t *d, int dci, uint64_t us) {
    uint64_t t0 = tsc_us();
    while (!d->xfer_done[dci]) {
        events();
        if (tsc_us() - t0 > us) return -1;
    }
    return d->xfer_cc[dci] == CC_SUCCESS || d->xfer_cc[dci] == CC_SHORT ? 0 : -(int)d->xfer_cc[dci];
}

/* ---- contexts and control transfers ----------------------------------------- */

static uint32_t *ctx(usb_dev_t *d, int index) {    /* input context: 0 control, 1 slot, 2.. endpoints */
    return (uint32_t *)(d->in_ctx + (uint64_t)index * ctx_size);
}

static int control(usb_dev_t *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, uint16_t len) {
    int in = (reqtype & 0x80) != 0;
    uint32_t trt = len == 0 ? 0 : in ? 3 : 2;
    d->xfer_done[1] = 0;
    ring_push(&d->ep0, (uint32_t)reqtype | ((uint32_t)req << 8) | ((uint32_t)value << 16),
              (uint32_t)index | ((uint32_t)len << 16), 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len) ring_push(&d->ep0, (uint32_t)d->buf_phys, (uint32_t)(d->buf_phys >> 32), len,
                       (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(&d->ep0, 0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | ((len && in) ? 0 : (1u << 16)));
    db[d->slot] = 1;
    return wait_xfer(d, 1, 1000000);
}

/* xHCI endpoint interval: 125 us units as a power of two */
static uint32_t ep_interval(int speed, uint8_t binterval) {
    uint32_t frames, i = 0;
    if (speed >= 3) return binterval ? (uint32_t)(binterval - 1) : 0;   /* high / super speed: already 2^(n-1) */
    frames = binterval ? binterval : 1;                                  /* full / low speed: ms */
    while ((1u << i) < frames * 8) i++;
    return i < 3 ? 3 : i;
}

static int add_endpoint(usb_dev_t *d, ring_t *r, int dci, uint32_t type, uint32_t mps, uint32_t interval) {
    uint32_t *ep = ctx(d, 1 + dci);
    if (ring_init(r)) return -1;
    zero(ep, ctx_size);
    ep[0] = interval << 16;
    ep[1] = (3u << 1) | (type << 3) | (mps << 16);
    ep[2] = (uint32_t)r->phys | 1;
    ep[3] = (uint32_t)(r->phys >> 32);
    ep[4] = (type == 7 ? mps : 1024) | ((type == 7 ? mps : 0) << 16);   /* average TRB length, ESIT payload */
    ctx(d, 0)[1] |= 1u << dci;
    if ((uint32_t)dci > (ctx(d, 1)[0] >> 27)) ctx(d, 1)[0] = (ctx(d, 1)[0] & ~(0x1Fu << 27)) | ((uint32_t)dci << 27);
    return 0;
}

/* ---- HID ------------------------------------------------------------------------- */

/* USB usage (0x04..0x65) -> PS/2 set-1 make code; 0x100 marks an E0 prefix */
static const uint16_t usage_to_set1[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22,
    [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31,
    [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16,
    [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08,
    [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D,
    [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29,
    [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41,
    [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0x137, [0x47] = 0x46,
    [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149, [0x4C] = 0x153, [0x4D] = 0x14F, [0x4E] = 0x151,
    [0x4F] = 0x14D, [0x50] = 0x14B, [0x51] = 0x150, [0x52] = 0x148,
    [0x53] = 0x45, [0x54] = 0x135, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x11C,
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47,
    [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = 0x15D,
};
/* modifier bits 0..7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint16_t mod_to_set1[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

static void key(uint16_t code, int up) {
    if (!code) return;
    if (code & 0x100) keyboard_inject_scancode(0xE0);
    keyboard_inject_scancode((uint8_t)((code & 0xFF) | (up ? 0x80 : 0)));
}

static void hid_report(usb_dev_t *d, uint32_t len) {
    const uint8_t *r = d->buf + 2048;
    if (d->kind == DEV_KBD && len >= 8) {
        uint8_t mods = r[0];
        if (r[2] == 1) return;                         /* phantom state: too many keys */
        for (int b = 0; b < 8; b++)
            if (((mods ^ d->mods) >> b) & 1) key(mod_to_set1[b], !((mods >> b) & 1));
        for (int i = 0; i < MAX_KBD_KEYS; i++) {       /* released */
            int still = 0;
            if (!d->keys[i]) continue;
            for (int j = 0; j < MAX_KBD_KEYS; j++) if (r[2 + j] == d->keys[i]) still = 1;
            if (!still && d->keys[i] < 0x66) key(usage_to_set1[d->keys[i]], 1);
        }
        for (int j = 0; j < MAX_KBD_KEYS; j++) {       /* pressed */
            int was = 0;
            if (r[2 + j] < 4) continue;
            for (int i = 0; i < MAX_KBD_KEYS; i++) if (d->keys[i] == r[2 + j]) was = 1;
            if (!was && r[2 + j] < 0x66) key(usage_to_set1[r[2 + j]], 0);
        }
        d->mods = mods;
        for (int i = 0; i < MAX_KBD_KEYS; i++) d->keys[i] = r[2 + i];
    } else if (d->kind == DEV_MOUSE && len >= 3) {
        mouse_inject((int8_t)r[1], (int8_t)r[2], len >= 4 ? -(int8_t)r[3] : 0, r[0]);
    } else if (d->kind == DEV_TABLET && len >= 5) {
        mouse_inject_abs(r[1] | (r[2] << 8), r[3] | (r[4] << 8), len >= 6 ? -(int8_t)r[5] : 0, r[0]);
    }
}

static void hid_queue(usb_dev_t *d) {
    d->xfer_done[d->dci_in] = 0;
    ring_push(&d->ep_in, (uint32_t)(d->buf_phys + 2048), (uint32_t)((d->buf_phys + 2048) >> 32), d->mps_in,
              (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    db[d->slot] = (uint32_t)d->dci_in;
}

/* ---- mass storage (bulk-only transport) ------------------------------------------ */

static int bulk(usb_dev_t *d, int in, uint64_t phys, uint32_t len) {
    ring_t *r = in ? &d->ep_in : &d->ep_out;
    int dci = in ? d->dci_in : d->dci_out;
    d->xfer_done[dci] = 0;
    ring_push(r, (uint32_t)phys, (uint32_t)(phys >> 32), len, (TRB_NORMAL << 10) | TRB_IOC | (in ? TRB_ISP : 0));
    db[d->slot] = (uint32_t)dci;
    return wait_xfer(d, dci, 5000000);
}

/* one SCSI command: CBW, data, CSW; the data in or from d->bounce */
static int scsi(usb_dev_t *d, const uint8_t *cb, int cblen, int in, uint32_t len) {
    uint8_t *cbw = d->buf, *csw = d->buf + 64;
    zero(cbw, 31);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;        /* "USBC" */
    d->tag++;
    cbw[4] = (uint8_t)d->tag; cbw[5] = (uint8_t)(d->tag >> 8); cbw[6] = (uint8_t)(d->tag >> 16); cbw[7] = (uint8_t)(d->tag >> 24);
    cbw[8] = (uint8_t)len; cbw[9] = (uint8_t)(len >> 8); cbw[10] = (uint8_t)(len >> 16); cbw[11] = (uint8_t)(len >> 24);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = (uint8_t)cblen;
    for (int i = 0; i < cblen; i++) cbw[15 + i] = cb[i];
    if (bulk(d, 0, d->buf_phys, 31)) return -1;
    if (len && bulk(d, in, d->bounce_phys, len)) return -1;
    if (bulk(d, 1, d->buf_phys + 64, 13)) return -1;
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53) return -1;
    return csw[12] == 0 ? 0 : -1;
}

#define BOUNCE_PAGES 16      /* 64 KiB per command */

static int storage_io(void *context, uint64_t lba, uint32_t count, void *buffer, int write) {
    usb_dev_t *d = (usb_dev_t *)context;
    uint8_t *p = (uint8_t *)buffer;
    uint32_t per = (BOUNCE_PAGES * 4096) / d->block_size;
    if (!d->used || d->kind != DEV_STORAGE) return -1;
    while (count) {
        uint32_t n = count < per ? count : per, bytes = n * d->block_size;
        uint8_t cb[10] = { write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                           (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        if (write) for (uint32_t i = 0; i < bytes; i++) d->bounce[i] = p[i];
        if (scsi(d, cb, 10, !write, bytes)) return -1;
        if (!write) for (uint32_t i = 0; i < bytes; i++) p[i] = d->bounce[i];
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

static int storage_read(void *c, uint64_t lba, uint32_t count, void *buf) { return storage_io(c, lba, count, buf, 0); }
static int storage_write(void *c, uint64_t lba, uint32_t count, const void *buf) {
    return storage_io(c, lba, count, (void *)buf, 1);
}

static int storage_start(usb_dev_t *d) {
    static int nstorage;
    uint8_t tur[6] = { 0 }, cap10[10] = { 0x25 };
    uint32_t before = partition_count();
    d->bounce_phys = pmm_alloc_contiguous(BOUNCE_PAGES);
    if (!d->bounce_phys) return -1;
    d->bounce = (uint8_t *)PHYS_TO_VIRT(d->bounce_phys);
    for (int i = 0; i < 20 && scsi(d, tur, 6, 0, 0); i++) sched_sleep(10);   /* media spinning up */
    if (scsi(d, cap10, 10, 1, 8)) return -1;
    d->blocks = (((uint64_t)d->bounce[0] << 24) | (d->bounce[1] << 16) | (d->bounce[2] << 8) | d->bounce[3]) + 1;
    d->block_size = ((uint32_t)d->bounce[4] << 24) | (d->bounce[5] << 16) | (d->bounce[6] << 8) | d->bounce[7];
    if (d->block_size != 512 && d->block_size != 4096) return -1;
    d->name[0] = 'u'; d->name[1] = 's'; d->name[2] = 'b'; d->name[3] = (char)('0' + nstorage++); d->name[4] = 0;
    d->bdev.name = d->name;
    d->bdev.context = d;
    d->bdev.sector_size = d->block_size;
    d->bdev.sector_count = d->blocks;
    d->bdev.read = storage_read;
    d->bdev.write = storage_write;
    if (block_register(&d->bdev) != 0) return -1;
    log_hex("[usb] storage, blocks ", (uint32_t)d->blocks);
    /* its partitions, mounted as /volumes/usb-N; the ones already known keep their mounts */
    partition_scan_all();
    for (uint32_t i = before; i < partition_count(); i++) {
        static int nmount;
        char path[32] = "/volumes/usb-";
        int k = 13;
        path[k++] = (char)('0' + nmount % 10);
        path[k] = 0;
        if (volumes_mount_partition(i, path) == 0) {
            nmount++;
            serial_write("[usb] mounted ");
            serial_write(path);
            serial_write("\n");
        }
    }
    return 0;
}

/* ---- enumeration ------------------------------------------------------------------- */

static int port_reset(int port) {
    uint32_t sc = rd32(op, OP_PORTSC(port));
    if (PORT_SPEED(sc) >= 4) return 0;             /* USB 3 ports enable themselves */
    wr32(op, OP_PORTSC(port), (sc & PORT_PP) | PORT_PR);
    if (wait_bits(op, OP_PORTSC(port), PORT_PRC, PORT_PRC, 200000)) return -1;
    sc = rd32(op, OP_PORTSC(port));
    wr32(op, OP_PORTSC(port), (sc & PORT_PP) | (sc & PORT_CHANGES));
    sched_sleep(2);
    return (rd32(op, OP_PORTSC(port)) & PORT_PED) ? 0 : -1;
}

static void hub_start(usb_dev_t *h);

/* sets up the device at a root port (parent -1) or at port hub_port of hub parent */
static void attach_dev(int port, int parent, int hub_port, int speed) {
    usb_dev_t *d = 0;
    uint32_t slot, mps0, total;
    uint8_t *cfg;
    int found_dci = 0;
    for (int i = 0; i < MAX_DEV; i++) if (!devs[i].used) { d = &devs[i]; break; }
    if (!d) return;
    zero(d, sizeof(*d));
    d->speed = speed;
    d->port = port;
    d->parent = parent;
    d->hub_port = hub_port;
    if (parent >= 0) {
        d->depth = devs[parent].depth + 1;
        d->route = devs[parent].route | ((uint32_t)(hub_port > 15 ? 15 : hub_port) << (4 * devs[parent].depth));
    }
    if (command(0, 0, 0, TRB_ENABLE_SLOT << 10, &slot)) return;
    d->slot = (int)slot;
    d->used = 1;
    d->buf = (uint8_t *)page(&d->buf_phys);
    d->in_ctx = (uint8_t *)page(&d->in_ctx_phys);
    if (!d->buf || !d->in_ctx || !page(&d->out_ctx_phys) || ring_init(&d->ep0)) goto fail;
    dcbaa[slot] = d->out_ctx_phys;

    /* address it: slot and control endpoint */
    mps0 = d->speed == 4 ? 512 : d->speed == 3 ? 64 : 8;
    ctx(d, 0)[1] = 3;                                      /* add slot and EP0 */
    ctx(d, 1)[0] = d->route | ((uint32_t)d->speed << 20) | (1u << 27);
    ctx(d, 1)[1] = (uint32_t)(port + 1) << 16;
    if (parent >= 0 && speed <= 2 && devs[parent].speed == 3)      /* a slow device behind a fast hub: its transaction translator */
        ctx(d, 1)[2] = (uint32_t)devs[parent].slot | ((uint32_t)hub_port << 8);
    ctx(d, 2)[1] = (3u << 1) | (4u << 3) | (mps0 << 16);   /* control */
    ctx(d, 2)[2] = (uint32_t)d->ep0.phys | 1;
    ctx(d, 2)[3] = (uint32_t)(d->ep0.phys >> 32);
    ctx(d, 2)[4] = 8;
    if (command((uint32_t)d->in_ctx_phys, (uint32_t)(d->in_ctx_phys >> 32), 0, (TRB_ADDRESS_DEV << 10) | (slot << 24), 0))
        goto fail;

    /* device descriptor: the first 8 bytes give the real EP0 packet size */
    if (control(d, 0x80, 6, 0x0100, 0, 8)) goto fail;
    if (d->speed <= 2 && d->buf[7] && d->buf[7] != mps0) {
        mps0 = d->buf[7];
        zero(ctx(d, 0), ctx_size);
        ctx(d, 0)[1] = 2;                                  /* evaluate EP0 */
        ctx(d, 2)[1] = (ctx(d, 2)[1] & 0xFFFF) | (mps0 << 16);
        if (command((uint32_t)d->in_ctx_phys, (uint32_t)(d->in_ctx_phys >> 32), 0, (TRB_EVAL_CTX << 10) | (slot << 24), 0))
            goto fail;
    }

    /* configuration: interfaces and endpoints */
    if (control(d, 0x80, 6, 0x0200, 0, 9)) goto fail;
    total = d->buf[2] | (d->buf[3] << 8);
    if (total > 1024) total = 1024;
    if (control(d, 0x80, 6, 0x0200, 0, (uint16_t)total)) goto fail;
    cfg = d->buf;
    {
        uint8_t config_value = cfg[5];
        uint32_t off = 0;
        int cls = -1, sub = -1, proto = -1;
        zero(ctx(d, 0), ctx_size);
        ctx(d, 0)[1] = 1;                                  /* the slot context, for context entries */
        while (off + 2 <= total && cfg[off] >= 2) {
            uint8_t len = cfg[off], type = cfg[off + 1];
            if (type == 4 && d->kind == DEV_NONE) {        /* interface */
                cls = cfg[off + 5]; sub = cfg[off + 6]; proto = cfg[off + 7];
                d->iface = cfg[off + 2];
                /* boot keyboards and mice say so (subclass 1); other HID
                 * pointers (QEMU's tablet) are told apart by report size */
                if (cls == 3 && proto == 1) d->kind = DEV_KBD;
                else if (cls == 3 && sub == 1 && proto == 2) d->kind = DEV_MOUSE;
                else if (cls == 3) d->kind = DEV_TABLET;            /* report size decides below */
                else if (cls == 8 && sub == 6 && proto == 0x50) d->kind = DEV_STORAGE;
                else if (cls == 9) d->kind = DEV_HUB;
            } else if (type == 5 && d->kind != DEV_NONE && !found_dci) {   /* endpoint of the chosen interface */
                uint8_t addr = cfg[off + 2], attr = cfg[off + 3] & 3, interval = cfg[off + 6];
                uint32_t mps = (cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF;
                int dci = (addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0);
                if (d->kind != DEV_STORAGE && attr == 3 && (addr & 0x80)) {
                    if (add_endpoint(d, &d->ep_in, dci, 7, mps, ep_interval(d->speed, interval))) goto fail;
                    d->dci_in = dci;
                    d->mps_in = mps > 64 ? 64 : mps;
                    found_dci = 1;
                } else if (d->kind == DEV_STORAGE && attr == 2) {
                    if (addr & 0x80) {
                        if (add_endpoint(d, &d->ep_in, dci, 6, mps, 0)) goto fail;
                        d->dci_in = dci;
                        d->mps_in = mps;
                    } else {
                        if (add_endpoint(d, &d->ep_out, dci, 2, mps, 0)) goto fail;
                        d->dci_out = dci;
                        d->mps_out = mps;
                    }
                    if (d->dci_in && d->dci_out) found_dci = 1;
                }
            }
            off += len;
        }
        if (d->kind == DEV_TABLET && d->mps_in < 6) d->kind = DEV_MOUSE;    /* a mouse without the boot subclass */
        if (d->kind == DEV_HUB) {                          /* ports are polled: no endpoint needed */
            if (control(d, 0x00, 9, config_value, 0, 0)) goto fail;
            hub_start(d);
            return;
        }
        if (!found_dci) {
            serial_write("[usb] device not supported (no keyboard, mouse or storage interface)\n");
            log_hex("[usb]   class/subclass/protocol, kind ", ((uint32_t)(cls & 0xFF) << 24) | ((uint32_t)(sub & 0xFF) << 16) |
                    ((uint32_t)(proto & 0xFF) << 8) | (uint32_t)d->kind);
            log_hex("[usb]   speed / config bytes ", ((uint32_t)d->speed << 16) | total);
            d->kind = DEV_OTHER;
            return;                                       /* keeps its slot; ignored */
        }
        if (command((uint32_t)d->in_ctx_phys, (uint32_t)(d->in_ctx_phys >> 32), 0, (TRB_CONFIG_EP << 10) | (slot << 24), 0))
            goto fail;
        if (control(d, 0x00, 9, config_value, 0, 0)) goto fail;
    }

    if (d->kind == DEV_KBD || d->kind == DEV_MOUSE) {
        (void)control(d, 0x21, 0x0B, 0, d->iface, 0);      /* SET_PROTOCOL: boot */
        (void)control(d, 0x21, 0x0A, 0, d->iface, 0);      /* SET_IDLE: report changes only */
    }
    if (d->kind == DEV_STORAGE) {
        if (storage_start(d)) {
            serial_write("[usb] storage did not answer\n");
            d->kind = DEV_OTHER;
        }
        return;
    }
    serial_write(d->kind == DEV_KBD ? "[usb] keyboard\n" : d->kind == DEV_MOUSE ? "[usb] mouse\n" : "[usb] tablet\n");
    hid_queue(d);
    return;
fail:
    serial_write("[usb] could not set up a device\n");
    d->used = 0;
}

static void attach(int port) {
    if (port_reset(port)) return;
    attach_dev(port, -1, 0, (int)PORT_SPEED(rd32(op, OP_PORTSC(port))));
}

/* a device and everything behind it (a hub's devices) */
static void detach_dev(int i) {
    for (int c = 0; c < MAX_DEV; c++) if (devs[c].used && devs[c].parent == i) detach_dev(c);
    devs[i].kind = DEV_NONE;
    devs[i].used = 0;
    serial_write("[usb] device removed\n");
}

static void detach(int port) {
    for (int i = 0; i < MAX_DEV; i++) {
        /* storage stays registered (the block layer cannot forget a device):
         * its reads now fail.  The slot is not reused. */
        if (devs[i].used && devs[i].port == port && devs[i].parent < 0) detach_dev(i);
    }
}

/* ---- hubs (USB 2: ports polled every half second) ---------------------------------- */

#define HUB_PORT_POWER   8
#define HUB_PORT_RESET   4
#define HUB_C_CONNECTION 16
#define HUB_C_RESET      20

static int hub_status(usb_dev_t *h, int p, uint16_t *status, uint16_t *change) {
    if (control(h, 0xA3, 0, 0, (uint16_t)p, 4)) return -1;
    *status = (uint16_t)(h->buf[0] | (h->buf[1] << 8));
    *change = (uint16_t)(h->buf[2] | (h->buf[3] << 8));
    return 0;
}

static void hub_start(usb_dev_t *h) {
    int hi = (int)(h - devs);
    if (h->depth >= 4 || control(h, 0xA0, 6, 0x2900, 0, 9)) {      /* hub descriptor */
        h->kind = DEV_OTHER;
        return;
    }
    h->hub_nports = h->buf[2] > 15 ? 15 : h->buf[2];
    /* tell the controller it is a hub, with how many ports */
    zero(ctx(h, 0), ctx_size);
    ctx(h, 0)[1] = 1;
    ctx(h, 1)[0] |= 1u << 26;
    ctx(h, 1)[1] = (ctx(h, 1)[1] & 0x00FFFFFFu) | ((uint32_t)h->hub_nports << 24);
    (void)command((uint32_t)h->in_ctx_phys, (uint32_t)(h->in_ctx_phys >> 32), 0,
                  (TRB_CONFIG_EP << 10) | ((uint32_t)h->slot << 24), 0);
    for (int p = 1; p <= h->hub_nports; p++) (void)control(h, 0x23, 3, HUB_PORT_POWER, (uint16_t)p, 0);
    sched_sleep(h->buf[5] * 2 / 10 + 10);          /* power-on to power-good, then some */
    h->kind = DEV_HUB;
    log_hex("[usb] hub, ports ", (uint32_t)h->hub_nports);
    (void)hi;
}

static int child_at(int hub, int p) {
    for (int i = 0; i < MAX_DEV; i++) if (devs[i].used && devs[i].parent == hub && devs[i].hub_port == p) return i;
    return -1;
}

static void hub_scan(int hi) {
    usb_dev_t *h = &devs[hi];
    for (int p = 1; p <= h->hub_nports; p++) {
        uint16_t st, ch;
        int c;
        if (hub_status(h, p, &st, &ch)) return;
        if (ch & 1) (void)control(h, 0x23, 1, HUB_C_CONNECTION, (uint16_t)p, 0);
        c = child_at(hi, p);
        if ((st & 1) && c < 0) {
            uint64_t t0 = tsc_us();
            int speed;
            (void)control(h, 0x23, 3, HUB_PORT_RESET, (uint16_t)p, 0);
            do {
                sched_sleep(2);
                if (hub_status(h, p, &st, &ch)) return;
            } while (!(ch & (1u << 4)) && tsc_us() - t0 < 500000);
            (void)control(h, 0x23, 1, HUB_C_RESET, (uint16_t)p, 0);
            if (!(st & 2)) continue;                         /* not enabled */
            speed = (st & (1u << 9)) ? 2 : (st & (1u << 10)) ? 3 : 1;
            sched_sleep(2);
            attach_dev(h->port, hi, p, speed);
        } else if (!(st & 1) && c >= 0) {
            detach_dev(c);
        }
    }
}

static int attached(int port) {
    for (int i = 0; i < MAX_DEV; i++) if (devs[i].used && devs[i].port == port && devs[i].parent < 0) return 1;
    return 0;
}

static void scan_ports(void) {
    for (uint32_t p = 0; p < nports; p++) {
        uint32_t sc = rd32(op, OP_PORTSC(p));
        if (sc & PORT_CHANGES) wr32(op, OP_PORTSC(p), (sc & PORT_PP) | (sc & PORT_CHANGES));
        if ((sc & PORT_CCS) && !attached((int)p)) attach((int)p);
        else if (!(sc & PORT_CCS) && attached((int)p)) detach((int)p);
    }
}

/* ---- controller bring-up ----------------------------------------------------------- */

static void bios_handoff(void) {
    uint32_t x = (rd32(cap, CAP_HCCPARAMS1) >> 16) * 4;
    while (x) {
        uint32_t v = rd32(cap, x);
        if ((v & 0xFF) == 1) {                          /* USB legacy support */
            wr32(cap, x, v | (1u << 24));               /* OS owned */
            (void)wait_bits(cap, x, 1u << 16, 0, 1000000);
            wr32(cap, x + 4, (rd32(cap, x + 4) & ~0x1F) | 0xE0000000u);   /* SMIs off, status cleared */
            return;
        }
        x = ((v >> 8) & 0xFF) ? x + ((v >> 8) & 0xFF) * 4 : 0;
    }
}

static int controller_start(const pci_device_t *pci) {
    uint32_t bar0 = pci_read_config32(pci, 0x10), bar1 = pci_read_config32(pci, 0x14);
    uint64_t base = (uint64_t)(bar0 & ~0xFu) | (((bar0 & 6) == 4) ? (uint64_t)bar1 << 32 : 0);
    uint32_t hcs1, hcs2, scratch;
    uint64_t p;
    uint32_t *erst;
    if (pci_enable_memory_busmaster(pci) != 0) return -1;
    cap = (volatile uint8_t *)vmm_map_physical(base, 0x10000, VMM_FLAGS_KERNEL_RW | PTE_NO_CACHE);
    if (!cap) return -1;
    op = cap + (rd32(cap, CAP_CAPLENGTH) & 0xFF);
    rt = cap + (rd32(cap, CAP_RTSOFF) & ~0x1Fu);
    db = (volatile uint32_t *)(cap + (rd32(cap, CAP_DBOFF) & ~0x3u));
    hcs1 = rd32(cap, CAP_HCSPARAMS1);
    hcs2 = rd32(cap, CAP_HCSPARAMS2);
    nslots = hcs1 & 0xFF;
    nports = (hcs1 >> 24) & 0xFF;
    ctx_size = (rd32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;
    if (nslots > 32) nslots = 32;

    bios_handoff();
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) & ~CMD_RUN);
    if (wait_bits(op, OP_USBSTS, STS_HCH, STS_HCH, 100000)) return -1;
    wr32(op, OP_USBCMD, CMD_HCRST);
    if (wait_bits(op, OP_USBCMD, CMD_HCRST, 0, 1000000) || wait_bits(op, OP_USBSTS, STS_CNR, 0, 1000000)) return -1;

    wr32(op, OP_CONFIG, nslots);
    dcbaa = (uint64_t *)page(&p);
    if (!dcbaa) return -1;
    wr64(op, OP_DCBAAP, p);
    /* scratchpad pages the controller keeps for itself */
    scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        uint64_t arr_phys, *arr = (uint64_t *)page(&arr_phys);
        if (!arr) return -1;
        for (uint32_t i = 0; i < scratch && i < 512; i++) if (!page(&arr[i])) return -1;
        dcbaa[0] = arr_phys;
    }
    if (ring_init(&cmd_ring)) return -1;
    wr64(op, OP_CRCR, cmd_ring.phys | 1);
    /* one event ring segment, interrupter 0 (polled) */
    ev_ring.trb = (trb_t *)page(&ev_ring.phys);
    erst = (uint32_t *)page(&erst_phys);
    if (!ev_ring.trb || !erst) return -1;
    ev_ring.idx = 0;
    ev_ring.cycle = 1;
    erst[0] = (uint32_t)ev_ring.phys;
    erst[1] = (uint32_t)(ev_ring.phys >> 32);
    erst[2] = RING_TRBS;
    wr32(rt, 0x20 + 0x08, 1);
    wr64(rt, 0x20 + 0x18, ev_ring.phys);
    wr64(rt, 0x20 + 0x10, erst_phys);
    wr32(op, OP_USBCMD, CMD_RUN);
    if (wait_bits(op, OP_USBSTS, STS_HCH, 0, 100000)) return -1;
    xhci_ok = 1;
    log_hex("[usb] xhci running, ports ", nports);
    return 0;
}

static void usb_thread(void) {
    const pci_device_t *pci = 0;
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d && d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) { pci = d; break; }
    }
    if (!pci || controller_start(pci) != 0) {
        serial_write(pci ? "[usb] xhci did not start\n" : "[usb] no xhci controller\n");
        for (;;) sched_sleep(100000);
    }
    sched_sleep(20);                                  /* ports finish connecting after power-up */
    scan_ports();
    for (uint64_t n = 0;; n++) {
        events();
        if (port_change || n % 50 == 0) {             /* plugged in or out: at once, else every 0.5 s */
            port_change = 0;
            scan_ports();
        }
        if (n % 50 == 25)                             /* hubs' ports (no change events from them) */
            for (int i = 0; i < MAX_DEV; i++) if (devs[i].used && devs[i].kind == DEV_HUB) hub_scan(i);
        sched_sleep(1);
    }
}

void usb_init(void) {
    (void)proc_create_kernel(usb_thread);
}
