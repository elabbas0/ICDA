/* Battery, AC adapter and power modes.
 *
 * Battery: without an AML interpreter the battery methods (_BST, _BIF) cannot
 * run, so the embedded controller is read directly with the register layout
 * those methods use.  Known layouts are matched on the DSDT's OEM / table id;
 * other machines report no battery rather than poking unknown ports.
 *
 *   Dell CBX3 (Latitude E7470 and relatives), EC data 0x930 / command 0x934:
 *     0x06  bit 0 AC online, bit 1 battery present
 *     0x03  battery select (write 1 for BAT0) before reading 0x10 ...
 *     0x10  state (bit 0 discharging, bit 1 charging, bit 2 critical)
 *     0x12  present rate, mA (signed: negative while discharging)
 *     0x14  voltage, mV        0x16  remaining capacity, mAh
 *     0x1E  last full charge, mAh   0x20  design capacity, mAh
 *
 * Power modes use hardware P-states (Intel Speed Shift, HWP): each CPU's
 * IA32_HWP_REQUEST gets the mode's performance range and energy/performance
 * preference.  CPUs apply a new mode on their next timer tick.
 *
 * /dev/battery  read: "present 1\nac 1\npercent 87\nstate discharging\n..."
 * /dev/power    read: "mode balanced\nsupported 1\n", write: "mode saver" */

#include "battery.h"
#include "../firmware/acpi.h"
#include "../cpu/smp.h"
#include "../cpu/tsc.h"
#include "../drivers/serial/serial.h"
#include "../proc/sched.h"

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* ---- embedded controller ---------------------------------------------------- */

static uint16_t ec_data, ec_cmd;
static int ec_layout;               /* 0 unknown, 1 Dell CBX3 */

#define EC_OBF 0x01
#define EC_IBF 0x02

static int ec_wait(uint8_t mask, uint8_t want) {
    for (int i = 0; i < 20000; i++) {          /* about 20 ms */
        if ((inb(ec_cmd) & mask) == want) return 0;
        udelay(1);
    }
    return -1;
}

static int ec_read(uint8_t addr, uint8_t *v) {
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_cmd, 0x80);
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_data, addr);
    if (ec_wait(EC_OBF, EC_OBF)) return -1;
    *v = inb(ec_data);
    return 0;
}

static int ec_write(uint8_t addr, uint8_t v) {
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_cmd, 0x81);
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_data, addr);
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_data, v);
    return ec_wait(EC_IBF, 0);
}

static int ec_read16(uint8_t addr, uint16_t *v) {
    uint8_t lo, hi;
    if (ec_read(addr, &lo) || ec_read((uint8_t)(addr + 1), &hi)) return -1;
    *v = (uint16_t)(lo | (hi << 8));
    return 0;
}

static int same(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- battery ------------------------------------------------------------------ */

typedef struct {
    int present, ac, percent, state;      /* state: 0 idle/full, 1 discharging, 2 charging */
    int rate_ma, remaining_mah, full_mah, minutes;
} battery_t;

static battery_t bat;
static uint64_t bat_read_tick;

static void battery_refresh(void) {
    uint8_t flags, st;
    uint16_t rate, rem, full;
    if (!ec_layout) return;
    if (bat_read_tick && sched_ticks() - bat_read_tick < 200) return;   /* every 2 s at most */
    bat_read_tick = sched_ticks();
    if (ec_read(0x06, &flags)) return;
    bat.ac = flags & 1;
    bat.present = (flags >> 1) & 1;
    if (!bat.present) return;
    if (ec_write(0x03, 1) || ec_read(0x10, &st) || ec_read16(0x12, &rate) ||
        ec_read16(0x16, &rem) || ec_read16(0x1E, &full)) return;
    bat.rate_ma = (int16_t)rate < 0 ? -(int)(int16_t)rate : (int)rate;
    bat.remaining_mah = rem;
    bat.full_mah = full;
    bat.percent = full ? (int)((uint32_t)rem * 100u / full) : 0;
    if (bat.percent > 100) bat.percent = 100;
    bat.state = (st & 1) ? 1 : (st & 2) ? 2 : 0;
    bat.minutes = -1;
    if (bat.rate_ma > 50) {
        if (bat.state == 1) bat.minutes = bat.remaining_mah * 60 / bat.rate_ma;
        else if (bat.state == 2 && full > rem) bat.minutes = (int)(full - rem) * 60 / bat.rate_ma;
    }
}

/* ---- power modes (HWP) ---------------------------------------------------------- */

#define MSR_PM_ENABLE        0x770
#define MSR_HWP_CAPABILITIES 0x771
#define MSR_HWP_REQUEST      0x774

static int hwp, hwp_epp;
static volatile int power_mode = POWER_BALANCED;
static volatile uint32_t mode_gen = 1;
static uint32_t applied_gen[SMP_MAX_CPUS];

static const char *const mode_names[] = { "saver", "balanced", "performance" };

static void hwp_apply(void) {
    uint64_t caps = rdmsr(MSR_HWP_CAPABILITIES);
    uint32_t highest = caps & 0xFF, guaranteed = (caps >> 8) & 0xFF, lowest = (caps >> 24) & 0xFF;
    uint32_t lo = lowest, hi = highest, epp = 0x80;
    if (power_mode == POWER_SAVER) { hi = guaranteed ? guaranteed : highest; epp = 0xC0; }
    else if (power_mode == POWER_PERFORMANCE) { lo = guaranteed ? guaranteed : lowest; epp = 0x00; }
    if (lo > hi) lo = hi;
    wrmsr(MSR_HWP_REQUEST, (uint64_t)lo | ((uint64_t)hi << 8) | (hwp_epp ? (uint64_t)epp << 24 : 0));
}

/* every CPU, from its timer interrupt */
void power_tick(void) {
    cpu_t *c;
    if (!hwp) return;
    c = this_cpu();
    if (applied_gen[c->index] == mode_gen) return;
    applied_gen[c->index] = mode_gen;
    if (!(rdmsr(MSR_PM_ENABLE) & 1)) wrmsr(MSR_PM_ENABLE, 1);
    hwp_apply();
}

/* ---- setup and device nodes ------------------------------------------------------- */

void power_mgmt_init(void) {
    /* Dell gives every table the same OEM ids; the DSDT itself is not in the root list */
    const struct acpi_sdt_header *dsdt = acpi_find_table("FACP");
    uint32_t a, b, c, d;
    if (dsdt && same((const char *)dsdt->oem_id, "DELL", 4) && same((const char *)dsdt->oem_table_id, "CBX3", 4)) {
        ec_data = 0x930;
        ec_cmd = 0x934;
        if (inb(ec_cmd) != 0xFF) ec_layout = 1;
    }
    cpuid(0, &a, &b, &c, &d);
    if (a >= 6) {
        cpuid(6, &a, &b, &c, &d);
        hwp = (a >> 7) & 1;
        hwp_epp = (a >> 10) & 1;
    }
    serial_write(ec_layout ? "[power] battery: Dell embedded controller\n" : "[power] battery: no known layout\n");
    serial_write(hwp ? "[power] modes: hardware P-states (HWP)\n" : "[power] modes: not supported (no HWP)\n");
}

static void put(char *buf, uint64_t cap, uint64_t *n, const char *s) {
    while (*s && *n + 1 < cap) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static void put_num(char *buf, uint64_t cap, uint64_t *n, int64_t v) {
    char t[24];
    int k = 0;
    if (v < 0) { put(buf, cap, n, "-"); v = -v; }
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) { char s[2] = { t[--k], 0 }; put(buf, cap, n, s); }
}

uint64_t battery_node_read(char *buf, uint64_t cap) {
    static const char *const states[] = { "idle", "discharging", "charging" };
    uint64_t n = 0;
    battery_refresh();
    put(buf, cap, &n, "present ");
    put_num(buf, cap, &n, ec_layout ? bat.present : 0);
    put(buf, cap, &n, "\nac ");
    put_num(buf, cap, &n, ec_layout ? bat.ac : 1);
    if (ec_layout && bat.present) {
        put(buf, cap, &n, "\npercent ");
        put_num(buf, cap, &n, bat.percent);
        put(buf, cap, &n, "\nstate ");
        put(buf, cap, &n, bat.state == 0 && bat.percent >= 95 ? "full" : states[bat.state]);
        put(buf, cap, &n, "\nrate_ma ");
        put_num(buf, cap, &n, bat.rate_ma);
        put(buf, cap, &n, "\nremaining_mah ");
        put_num(buf, cap, &n, bat.remaining_mah);
        put(buf, cap, &n, "\nfull_mah ");
        put_num(buf, cap, &n, bat.full_mah);
        put(buf, cap, &n, "\nminutes ");
        put_num(buf, cap, &n, bat.minutes);
    }
    put(buf, cap, &n, "\n");
    return n;
}

uint64_t power_node_read(char *buf, uint64_t cap) {
    uint64_t n = 0;
    put(buf, cap, &n, "mode ");
    put(buf, cap, &n, mode_names[power_mode]);
    put(buf, cap, &n, "\nsupported ");
    put_num(buf, cap, &n, hwp);
    put(buf, cap, &n, "\n");
    return n;
}

uint64_t power_node_write(const char *buf, uint64_t len) {
    for (int m = 0; m < 3; m++) {
        const char *name = mode_names[m];
        uint64_t k = 0;
        if (len < 5 || !same(buf, "mode ", 5)) break;
        while (name[k] && 5 + k < len && buf[5 + k] == name[k]) k++;
        if (!name[k] && (5 + k == len || buf[5 + k] == '\n' || buf[5 + k] == 0)) {
            power_mode = m;
            mode_gen++;
            return len;
        }
    }
    return (uint64_t)-1;
}
