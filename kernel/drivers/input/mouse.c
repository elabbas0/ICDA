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
    ps2_read(); /* ACK 0xFA */
    ps2_read(); /* self-test 0xAA */
    ps2_read(); /* mouse ID 0x00 */

    /* Set defaults */
    mouse_send(MOUSE_CMD_SET_DEFAULTS);
    ps2_read(); /* ACK */

    /* Enable data reporting (stream mode) */
    mouse_send(MOUSE_CMD_ENABLE_STREAM);
    ps2_read(); /* ACK */

    mouse_packet_idx = 0;
}

void mouse_set_screen(int w, int h) {
    screen_w = w;
    screen_h = h;
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
}

void mouse_irq(struct registers *regs) {
    (void)regs;
    uint8_t byte = inb(PS2_DATA);

    /* First byte must have bit 3 set; resync if not */
    if (mouse_packet_idx == 0 && !(byte & 0x08)) {
        return;
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

int mouse_read_event(mouse_event_t *out) {
    if (!out || mouse_buf_tail == mouse_buf_head) return -1;
    *out = mouse_buf[mouse_buf_tail];
    mouse_buf_tail = (mouse_buf_tail + 1) % MOUSE_BUF_CAP;
    return 0;
}

int32_t mouse_abs_x(void) { return mouse_x; }
int32_t mouse_abs_y(void) { return mouse_y; }
uint8_t mouse_buttons(void) { return mouse_btn; }
