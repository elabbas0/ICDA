#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UACCESS_H
static int copy_from_user(void *d, const void *s, uint64_t n);
static int copy_to_user(void *d, const void *s, uint64_t n);
#include "../kernel/net/net.c"
#include "../kernel/net/sock.c"

static int copy_from_user(void *d, const void *s, uint64_t n) { memcpy(d,s,n); return 0; }
static int copy_to_user(void *d, const void *s, uint64_t n) { memcpy(d,s,n); return 0; }
uint64_t sched_ticks(void) { return 123; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void serial_write(const char *s) { (void)s; }
void sched_event_wake_key(const void *key) { (void)key; }
static unsigned char wire[2 * TCP_SBUF];
static size_t wire_len;
int net_drv_send_frame(const void *frame, uint16_t len) {
    const unsigned char *p = frame;
    assert(len >= 54);
    unsigned iplen = (p[16] << 8) | p[17];
    unsigned tcp_header = (p[46] >> 4) * 4;
    unsigned n = iplen - 20 - tcp_header;
    assert(wire_len + n <= sizeof(wire));
    memcpy(wire + wire_len, p + 34 + tcp_header, n); wire_len += n;
    return 0;
}
static sock_t s;
static void start(void) {
    static uint8_t sendbuf[TCP_SBUF], recvbuf[TCP_RBUF];
    sock_release(&s);
    s.type = SOCK_TCP; s.state = T_ESTABLISHED;
    s.snd_una = s.snd_nxt = 100; s.rcv_nxt = 500;
    s.snd_wnd = s.cwnd = TCP_SBUF;
    s.sbuf = sendbuf; s.rbuf = recvbuf; wire_len = 0;
}
static void ack(uint32_t seq) {
    uint8_t p[20] = {0};
    wr32be(p+4,s.rcv_nxt); wr32be(p+8,seq);
    p[12] = 5 << 4; p[13] = TCP_FLAG_ACK; p[14] = 255; p[15] = 255;
    tcp_input(&s,p,sizeof(p));
}
int main(void) {
    start();
    assert(op_send(&s,(const uint8_t *)"abcdefgh",8) == 8);
    ack(104); /* Peer has received only abcd; efgh must remain queued. */
    assert(s.slen == 4);
    assert(op_send(&s,(const uint8_t *)"ijkl",4) == 4);
    s.snd_nxt = s.snd_una; wire_len = 0; /* retransmit the unacknowledged bytes */
    tcp_output(&s);
    assert(wire_len == 8 && !memcmp(wire,"efghijkl",8));
    ack(112);
    assert(s.slen == 0);
    wire_len = 0;
    assert(op_send(&s,(const uint8_t *)"next",4) == 4 && wire_len == 4 && !memcmp(wire,"next",4));
    start();
    static unsigned char fill[TCP_SBUF-2];
    memset(fill,'q',sizeof(fill));
    assert(op_send(&s,fill,sizeof(fill)) == sizeof(fill));
    ack(s.snd_nxt); wire_len = 0;
    assert(op_send(&s,(const uint8_t *)"wraparound",10) == 10);
    ack(s.snd_una+3);
    assert(op_send(&s,(const uint8_t *)"more",4) == 4);
    s.snd_nxt = s.snd_una; wire_len = 0; tcp_output(&s);
    assert(wire_len == 11 && !memcmp(wire,"paroundmore",11));
    start(); s.snd_una = s.snd_nxt = 0xfffffffcU;
    assert(op_send(&s,(const uint8_t *)"abcdefgh",8) == 8);
    ack(0);
    assert(op_send(&s,(const uint8_t *)"ijkl",4) == 4);
    s.snd_nxt = s.snd_una; wire_len = 0; tcp_output(&s);
    assert(wire_len == 8 && !memcmp(wire,"efghijkl",8));
    puts("TCP send queue: partial ACK, appended writes, retransmission, full drain, buffer wrap and sequence wrap passed");
    return 0;
}
