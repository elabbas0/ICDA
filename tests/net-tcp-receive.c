#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/net/net.c"
#include "../kernel/net/sock.c"

uint64_t sched_ticks(void) { return 123; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void serial_write(const char *s) { (void)s; }
void sched_event_wake_key(const void *key) { (void)key; }
static uint32_t ack_sent;
int net_drv_send_frame(const void *frame, uint16_t len) {
    assert(len >= 54);
    ack_sent = rd32be((const uint8_t *)frame + 42);
    return 0;
}
static sock_t s;
static void start(uint32_t seq, uint32_t head) {
    sock_release(&s);
    s.type = SOCK_TCP; s.state = T_ESTABLISHED;
    s.rcv_nxt = seq; s.rhead = head;
    static uint8_t buf[TCP_RBUF]; s.rbuf = buf;
}
static void segment(uint32_t seq, const char *data, int fin) {
    uint8_t packet[100] = {0};
    wr32be(packet + 4, seq);
    packet[12] = 5 << 4; packet[13] = fin ? TCP_FLAG_FIN : 0;
    size_t n = strlen(data); memcpy(packet + 20, data, n);
    tcp_input(&s, packet, (uint16_t)(20 + n));
    assert(ack_sent == s.rcv_nxt);
}
static void expect(const char *data) {
    assert(s.rlen == strlen(data));
    for (unsigned i = 0; i < s.rlen; i++) assert(s.rbuf[(s.rhead + i) % TCP_RBUF] == data[i]);
}
int main(void) {
    start(100, 0);
    segment(104, "efgh", 0); assert(s.rcv_nxt == 100 && !s.rlen);
    segment(108, "ijkl", 0); segment(104, "efgh", 0); assert(s.rpending == 8);
    segment(100, "abcd", 0); expect("abcdefghijkl"); assert(s.rcv_nxt == 112 && !s.rmap);
    segment(108, "ijklmnop", 0); expect("abcdefghijklmnop"); /* overlapping retransmission */
    segment(100, "abcd", 0); expect("abcdefghijklmnop");
    start(0xfffffffcU, TCP_RBUF - 2);
    segment(0, "efgh", 1); assert(!s.peer_fin && s.fin_pending);
    segment(0xfffffffcU, "abcd", 0); expect("abcdefgh");
    assert(s.rcv_nxt == 5 && s.peer_fin && s.state == T_CLOSE_WAIT);
    start(400, 0);
    segment(404, "efghijkl", 0); segment(404, "efgh", 1);
    segment(400, "abcd", 0); expect("abcdefgh");
    assert(s.rcv_nxt == 409 && s.peer_fin && !s.rmap);
    start(500, 0);
    segment(500 + TCP_RBUF, "outside", 1); assert(!s.rmap && !s.rlen && !s.fin_pending);
    segment(503, "def", 0); segment(500, "abcdefghi", 0); expect("abcdefghi");
    start(900, 0);
    s.rlen = TCP_RBUF - 2;
    segment(900, "abcd", 1); assert(s.rcv_nxt == 902 && s.rlen == TCP_RBUF && !s.peer_fin);
    start(1000, 0); segment(1002, "cd", 0); assert(s.rmap);
    sock_release(&s); assert(!s.rmap && !s.rpending);
    puts("TCP reordering, cumulative ACKs, overlap, duplicate, sequence/ring wrap, FIN and receive-window checks passed");
}
