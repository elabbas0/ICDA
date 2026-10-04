#include "mouse.h"
#include "../../cpu/isr.h"
#include "../../proc/sched.h"
#include "../../drivers/serial/serial.h"
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
#define MOUSE_CMD_SET_RATE       0xF3
#define MOUSE_CMD_GET_ID         0xF2

#define MOUSE_BUF_CAP 256

static mouse_event_t mouse_buf[MOUSE_BUF_CAP];
static uint32_t mouse_buf_head = 0;
static uint32_t mouse_buf_tail = 0;

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static uint8_t mouse_btn = 0;

static int screen_w = 1280;
static int screen_h = 720;

static uint8_t mouse_packet[4];
static int     mouse_packet_len = 3;
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

static void mouse_set_rate(uint8_t rate) {
    mouse_send(MOUSE_CMD_SET_RATE);
    (void)ps2_read();
    mouse_send(rate);
    (void)ps2_read();
}





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

    
    ps2_cmd(PS2_CMD_ENABLE_AUX);

    
    ps2_cmd(PS2_CMD_READ_CONFIG);
    ps2_wait_read();
    config = inb(PS2_DATA);
    config |= 0x02;    
    config &= ~0x20;   
    ps2_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_data_write(config);

    
    mouse_send(MOUSE_CMD_RESET);
    (void)ps2_read();   
    (void)ps2_read();   
    (void)ps2_read();   

    
    mouse_send(MOUSE_CMD_SET_DEFAULTS);
    (void)ps2_read();

    mouse_set_rate(200);
    mouse_set_rate(100);
    mouse_set_rate(80);
    mouse_send(MOUSE_CMD_GET_ID);
    (void)ps2_read();
    {
        uint8_t id = ps2_read();
        mouse_packet_len = (id == 3 || id == 4) ? 4 : 3;
    }
    mouse_set_rate(100);

    
    mouse_send(MOUSE_CMD_ENABLE_STREAM);
    (void)ps2_read();

    
    mouse_drain_output();
}

void mouse_set_screen(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w != screen_w || h != screen_h) {
        


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
    static uint64_t irq_cnt = 0;
    static uint64_t last_diag_tsc = 0;
    irq_cnt++;
    


    {
        uint32_t lo, hi;
        uint64_t now;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        now = ((uint64_t)hi << 32) | lo;
        if (now - last_diag_tsc > 5000000) {
            last_diag_tsc = now;
            serial_write("mouse: irq cnt=");
            
            {
                char buf[21]; int n = 0; uint64_t v = irq_cnt;
                if (v == 0) { buf[n++] = '0'; }
                else { while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; } }
                for (int i = n - 1; i >= 0; i--) serial_write_char(buf[i]);
            }
            serial_write(" qdepth=");
            {
                uint32_t depth = (mouse_buf_head + MOUSE_BUF_CAP - mouse_buf_tail) % MOUSE_BUF_CAP;
                char buf[21]; int n = 0; uint64_t v = depth;
                if (v == 0) { buf[n++] = '0'; }
                else { while (v && n < 20) { buf[n++] = (char)('0' + v % 10); v /= 10; } }
                for (int i = n - 1; i >= 0; i--) serial_write_char(buf[i]);
            }
            serial_write("\n");
        }
    }
    





    for (int guard = 0; guard < 64; guard++) {
        uint8_t status = inb(PS2_STATUS);
        


        if (!(status & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }
        uint8_t byte = inb(PS2_DATA);

        
        if (mouse_packet_idx == 0 && !(byte & 0x08)) {
            mouse_packet_idx = 0;
            continue;
        }

        mouse_packet[mouse_packet_idx++] = byte;

        if (mouse_packet_idx == mouse_packet_len) {
            mouse_packet_idx = 0;

            uint8_t flags = mouse_packet[0];
            int32_t dx = (int32_t)(int8_t)mouse_packet[1];
            int32_t dy = (int32_t)(int8_t)mouse_packet[2];
            int32_t dz = 0;
            if (mouse_packet_len == 4) {
                int32_t z = mouse_packet[3] & 0x0F;
                dz = (z & 0x08) ? z - 16 : z;
            }


            
            dy = -dy;

            
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
            int overflow = (next == mouse_buf_tail);
            if (overflow) {
                
                mouse_buf_tail = (mouse_buf_tail + 1) % MOUSE_BUF_CAP;
                static uint64_t last_warn = 0;
                uint64_t now = 0;
                {
                    uint32_t lo, hi;
                    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
                    now = ((uint64_t)hi << 32) | lo;
                }
                if (now - last_warn > 10000000) {
                    last_warn = now;
                    serial_write("mouse: overflow, keeping newest\n");
                }
            }
            mouse_buf[mouse_buf_head].abs_x   = mouse_x;
            mouse_buf[mouse_buf_head].abs_y   = mouse_y;
            mouse_buf[mouse_buf_head].dx      = dx;
            mouse_buf[mouse_buf_head].dy      = dy;
            mouse_buf[mouse_buf_head].buttons = mouse_btn;
            mouse_buf[mouse_buf_head].dz      = (int8_t)dz;
            mouse_buf_head = next;
            


            sched_wake_input_waiters();
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
