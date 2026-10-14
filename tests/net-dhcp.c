#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/net/net.c"
#include "../kernel/net/sock.c"

static uint64_t clock_ticks, deliver_at;
static uint8_t pending[NET_FRAME_CAP], last_reply[NET_FRAME_CAP], discover_xid[4];
static uint16_t pending_len, last_reply_len;
static int transmissions, delay_offer = 30, reject_request;
static const uint8_t client_mac[6] = {2,0,0,0,0,1};
static const uint32_t phone_ip = 0x010a14ac, client_ip = 0x020a14ac;
uint64_t sched_ticks(void) { return clock_ticks; }
void sched_sleep(uint64_t ticks) { clock_ticks += ticks; }
void sched_event_wake_key(const void *key) { (void)key; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void serial_write(const char *s) { (void)s; }
int net_drv_mac(uint8_t out[6]) { memcpy(out,client_mac,6); return 0; }

static const uint8_t *option(const uint8_t *body, size_t size, unsigned tag, unsigned *length) {
    for (size_t at=240; at<size;) {
        unsigned t=body[at++];
        if(t==255) break;
        if(!t) continue;
        if(at==size) return 0;
        unsigned n=body[at++];
        if(n>size-at) return 0;
        if(t==tag) { *length=n; return body+at; }
        at+=n;
    }
    return 0;
}
static void reply_packet(uint8_t type, const uint8_t *request) {
    memset(pending,0,sizeof(pending));
    eth_hdr_t *eth=(eth_hdr_t *)pending;
    eth->type_be=htons16(ETH_TYPE_IPV4);
    ipv4_hdr_t *ip=(ipv4_hdr_t *)(eth+1);
    ip->ver_ihl=0x45; ip->proto=17; ip->src_be=phone_ip; ip->dst_be=0xffffffff;
    ip->total_len_be=htons16(20+8+300);
    udp_hdr_t *udp=(udp_hdr_t *)(ip+1);
    udp->src_port_be=htons16(67); udp->dst_port_be=htons16(68); udp->len_be=htons16(308);
    uint8_t *b=(uint8_t *)(udp+1);
    b[0]=2; b[1]=1; b[2]=6;
    memcpy(b+4,request+4,4); memcpy(b+28,client_mac,6);
    dhcp_write32(b+16,client_ip); dhcp_write32(b+236,0x63538263);
    unsigned at=240;
    b[at++]=53; b[at++]=1; b[at++]=type;
    b[at++]=54; b[at++]=4; dhcp_write32(b+at,phone_ip); at+=4;
    b[at++]=1; b[at++]=4; dhcp_write32(b+at,0xf0ffffff); at+=4;
    b[at++]=3; b[at++]=4; dhcp_write32(b+at,phone_ip); at+=4;
    b[at++]=6; b[at++]=4; dhcp_write32(b+at,phone_ip); at+=4;
    b[at]=255;
    pending_len=14+20+8+300;
    memcpy(last_reply,pending,pending_len); last_reply_len=pending_len;
    deliver_at=clock_ticks+(type==2 ? delay_offer : 10);
}
int net_drv_send_frame(const void *frame, uint16_t len) {
    const ipv4_hdr_t *ip=(const ipv4_hdr_t *)((const uint8_t *)frame+14);
    const udp_hdr_t *udp=(const udp_hdr_t *)(ip+1);
    if(ip->proto!=17 || ntohs16(udp->dst_port_be)!=67) return 0;
    const uint8_t *body=(const uint8_t *)(udp+1);
    unsigned size=ntohs16(udp->len_be)-8, n=0;
    assert(len>=14+20+8+300 && size==300);
    assert(ip->src_be==0 && body[10]==0x80 && body[11]==0);
    const uint8_t *id=option(body,size,61,&n);
    assert(id && n==7 && id[0]==1 && !memcmp(id+1,client_mac,6));
    const uint8_t *prl=option(body,size,55,&n);
    assert(prl && n==3 && prl[0]==1 && prl[1]==3 && prl[2]==6);
    const uint8_t *type=option(body,size,53,&n); assert(type && n==1);
    transmissions++;
    if(*type==1) {memcpy(discover_xid,body+4,4); reply_packet(2,body);}
    else if(*type==3) {
        /* A strict hotspot drops REQUEST when it does not match its OFFER. */
        if(memcmp(discover_xid,body+4,4)) return 0;
        const uint8_t *address=option(body,size,50,&n);
        assert(address && n==4 && dhcp_read32(address)==client_ip);
        const uint8_t *server=option(body,size,54,&n);
        assert(server && n==4 && dhcp_read32(server)==phone_ip);
        reply_packet(reject_request ? 6 : 5,body);
    }
    return 0;
}
int net_drv_recv_frame(void *data, uint16_t cap, uint16_t *len) {
    if(!pending_len || clock_ticks<deliver_at) return 0;
    assert(cap>=pending_len);
    memcpy(data,pending,pending_len); *len=pending_len; pending_len=0; return 1;
}
static void acquire(int sockets_open, int delay) {
    memset(&net_state,0,sizeof(net_state)); memset(socks,0,sizeof(socks));
    legacy_len=0; transmissions=0; pending_len=0; delay_offer=delay;
    if(sockets_open) {socks[0].state=T_CLOSED; socks[0].type=SOCK_UDP;}
    assert(net_reconfigure()==0 && net_state.ready && transmissions==2);
    assert(net_state.ip==client_ip && net_state.gateway==phone_ip && net_state.dns==phone_ip);
    assert(net_state.netmask==0xf0ffffff);
}
int main(void) {
    acquire(0,30);
    acquire(1,30); /* background application retains a socket while reconnecting */
    acquire(1,300); /* busy hotspot takes longer than the old two-second window */
    dhcp_reply_t parsed;
    uint8_t *body=last_reply+14+20+8;
    uint32_t xid=dhcp_read32(body+4);
    assert(dhcp_parse_reply(last_reply,last_reply_len,xid,&parsed));
    assert(!dhcp_parse_reply(last_reply,last_reply_len,xid+1,&parsed));
    body[28]^=1; assert(!dhcp_parse_reply(last_reply,last_reply_len,xid,&parsed)); body[28]^=1;
    assert(!dhcp_parse_reply(last_reply,10,xid,&parsed));
    assert(!dhcp_parse_reply(last_reply,last_reply_len-1,xid,&parsed));
    body[241]=250; assert(!dhcp_parse_reply(last_reply,last_reply_len,xid,&parsed));
    reject_request=1; net_state.ip=0;
    assert(net_reconfigure()<0 && !net_state.ready);
    assert(strstr(net_dhcp_error(),"rejected"));
    puts("Strict hotspot DHCP, stable transaction IDs, open-socket reconnects, delayed replies and malformed packet checks passed");
    return 0;
}
