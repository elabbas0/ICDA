#include "mouse.h"
#include "../../cpu/isr.h"
#include <stdint.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02

#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_SEND_TO_AUX  0xD4

#define MOUSE_CMD_RESET          0xFF
#define MOUSE_CMD_ENABLE_STREAM  0xF4
#define MOUSE_CMD_SET_DEFAULTS   0xF6

#define MOUSE_BUF_CAP 64

static mouse_event_t mouse_buf[MOUSE_BUF_CAP];
static uint32_t mouse_buf_head = 0;
static uint32_t mouse_buf_tail = 0;

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static uint8_t mouse_btn = 0;

static int screen_w = 1280;
static int screen_h = 720;

static uint8_t mouse_packet[3];
static int     mouse_packet_idx = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void ps2_wait_write(void) {
    uint32_t timeout = 100000;
    while ((inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL) && timeout--);
}
static void ps2_wait_read(void) {
    uint32_t timeout = 100000;
    while (!(inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) && timeout--);
}
static uint8_t ps2_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}
static void ps2_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}
static void ps2_data_write(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}
static void mouse_send(uint8_t cmd) {
    ps2_cmd(PS2_CMD_SEND_TO_AUX);
    ps2_data_write(cmd);
}

/* Discard any bytes still queued after init (e.g. a late ACK on real
 * hardware). Otherwise a stray byte gets consumed as the first byte of
 * the next real packet (0xFA passes the bit-3 sync check), shifting
 * every packet by one byte. */
static void mouse_drain_output(void) {
    uint32_t timeout = 2000000;
    while ((inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) && timeout--) {
        inb(PS2_DATA);
    }
    mouse_packet_idx = 0;
}

void mouse_init(void) {
    uint8_t config;

    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    /* Enable auxiliary PS/2 port */
    ps2_cmd(PS2_CMD_ENABLE_AUX);

    /* Enable IRQ12 in PS/2 controller config byte */
    ps2_cmd(PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    config = inb(PS2_DATA);
    config |= 0x02;    /* enable IRQ12 */
    config &= ~0x20;   /* clear mouse clock disable bit */
    ps2_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_data_write(config);

    /* Reset mouse */
    mouse_send(MOUSE_CMD_RESET);
    (void)ps2_read();   /* ACK 0xFA */
    (void)ps2_read();   /* self-test 0xAA */
    (void)ps2_read();   /* device id 0x00 */

    /* Set defaults */
    mouse_send(MOUSE_CMD_SET_DEFAULTS);
    (void)ps2_read();

    /* Enable data reporting (stream mode) */
    mouse_send(MOUSE_CMD_ENABLE_STREAM);
    (void)ps2_read();

    /* Discard any bytes that arrived late (see mouse_drain_output). */
    mouse_drain_output();
}

void mouse_set_screen(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w != screen_w || h != screen_h) {
        /* Resolution change: recenter so the pointer lands where the
         * window manager initialized it instead of teleporting on the
         * first event. */
        screen_w = w;
        screen_h = h;
        mouse_x = w / 2;
        mouse_y = h / 2;
        return;
    }
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
}

void mouse_irq(struct registers *regs) {
    (void)regs;
    /* Drain every byte currently in the output buffer. One IRQ12 can
     * cover several bytes (the PIC may coalesce edges under load), and
     * some IRQs are spurious (e.g. an edge latched during init while the
     * interrupt was still masked). Reading the data port when nothing is
     * waiting returns stale data on real hardware and the last byte on
     * QEMU, so always check the status port first. */
    for (int guard = 0; guard < 64; guard++) {
        uint8_t status = inb(PS2_STATUS);
        /* Consume only mouse data: OBF is set for both devices, and bit
         * 5 (MOUSE_OBF) selects the aux channel. Reading keyboard bytes
         * here would steal them from the keyboard driver. */
        if (!(status & PS2_STATUS_OUTPUT_FULL) || !(status & 0x20)) {
            break;
        }
        uint8_t byte = inb(PS2_DATA);

        /* First byte must have bit 3 set; resync if not */
        if (mouse_packet_idx == 0 && !(byte & 0x08)) {
            mouse_packet_idx = 0;
            continue;
        }

        mouse_packet[mouse_packet_idx++] = byte;

        if (mouse_packet_idx == 3) {
            mouse_packet_idx = 0;

            uint8_t flags = mouse_packet[0];
            int32_t dx = (int32_t)(int8_t)mouse_packet[1];
            int32_t dy = (int32_t)(int8_t)mouse_packet[2];


            /* Y axis is inverted for screen coords */
            dy = -dy;

            /* Ignore if overflow bits set */
            if (flags & 0x40) dx = 0;
            if (flags & 0x80) dy = 0;

            mouse_x += dx;
            mouse_y += dy;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (screen_w > 0 && mouse_x >= screen_w) mouse_x = screen_w - 1;
            if (screen_h > 0 && mouse_y >= screen_h) mouse_y = screen_h - 1;

            mouse_btn = flags & 0x07;

            uint32_t next = (mouse_buf_head + 1) % MOUSE_BUF_CAP;
            if (next != mouse_buf_tail) {
                mouse_buf[mouse_buf_head].abs_x   = mouse_x;
                mouse_buf[mouse_buf_head].abs_y   = mouse_y;
                mouse_buf[mouse_buf_head].dx      = dx;
                mouse_buf[mouse_buf_head].dy      = dy;
                mouse_buf[mouse_buf_head].buttons = mouse_btn;
                mouse_buf_head = next;
            }
        }
    }
}

int mouse_read_event(mouse_event_t *out) {
    if (!out || mouse_buf_tail == mouse_buf_head) return -1;
    *out = mouse_buf[mouse_buf_tail];
    mouse_buf_tail = (mouse_buf_tail + 1) % MOUSE_BUF_CAP;
    return 0;
}

int32_t mouse_abs_x(void) { return mouse_x; }
int32_t mouse_abs_y(void) { return mouse_y; }
uint8_t mouse_buttons(void) { return mouse_btn; }
