#include "tls.h"
#include "net.h"
#include "crypto/sha256.h"
#include "crypto/hmac.h"
#include "crypto/aes.h"
#include "crypto/rsa.h"
#include "../drivers/net/e1000.h"
#include "../drivers/console/console.h"
#include "../memory/heap.h"

#define TLS_VERSION_MAJOR 3
#define TLS_VERSION_MINOR 3

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20
#define TLS_CONTENT_ALERT              21
#define TLS_CONTENT_HANDSHAKE          22
#define TLS_CONTENT_APPLICATION_DATA   23

#define TLS_HANDSHAKE_CLIENT_HELLO       1
#define TLS_HANDSHAKE_SERVER_HELLO       2
#define TLS_HANDSHAKE_CERTIFICATE       11
#define TLS_HANDSHAKE_SERVER_HELLO_DONE 14
#define TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE 16
#define TLS_HANDSHAKE_FINISHED          20

#define TLS_CIPHER_RSA_AES128_CBC_SHA    0x002F
#define TLS_CIPHER_RSA_AES128_CBC_SHA256 0x003C

#define TLS_ALERT_LEVEL_FATAL 2
#define TLS_ALERT_DESC_HANDSHAKE_FAILURE 40
#define TLS_ALERT_DESC_INSUFFICIENT_SECURITY 71
#define TLS_ALERT_DESC_ILLEGAL_PARAMETER 47
#define TLS_ALERT_DESC_DECODE_ERROR 50
#define TLS_ALERT_DESC_PROTOCOL_VERSION 70
#define TLS_ALERT_DESC_INTERNAL_ERROR 80
#define TLS_ALERT_DESC_UNRECOGNIZED_NAME 112
#define TLS_ALERT_DESC_BAD_CERTIFICATE 42
#define TLS_ALERT_DESC_UNSUPPORTED_CERTIFICATE 43
#define TLS_ALERT_DESC_CERTIFICATE_UNKNOWN 46
#define TLS_ALERT_DESC_DECRYPT_ERROR 51
#define TLS_ALERT_DESC_UNEXPECTED_MESSAGE 10
#define TLS_ALERT_DESC_RECORD_OVERFLOW 22

typedef enum {
    TLS_MAC_SHA1 = 1,
    TLS_MAC_SHA256 = 2,
} tls_mac_alg_t;

typedef struct {
    uint8_t type;
    uint16_t version;
    uint16_t length;
} __attribute__((packed)) tls_record_hdr_t;

typedef struct {
    uint8_t type;
    uint32_t length;  // 3 bytes
} __attribute__((packed)) tls_handshake_hdr_t;

struct tls_conn {
    uint8_t dst_mac[6];
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;

    uint32_t tcp_seq;
    uint32_t tcp_ack;

    uint8_t pre_master_secret[48];
    uint8_t master_secret[48];
    uint8_t client_random[32];
    uint8_t server_random[32];

    uint8_t client_write_mac_key[32];
    uint8_t server_write_mac_key[32];
    uint8_t client_write_key[16];
    uint8_t server_write_key[16];
    uint8_t client_write_iv[16];
    uint8_t server_write_iv[16];

    uint8_t client_enc_key[16];
    uint8_t server_enc_key[16];
    uint8_t client_enc_expanded[176];
    uint8_t server_enc_expanded[176];
    uint16_t cipher_suite;
    uint8_t mac_key_len;
    uint8_t mac_len;
    tls_mac_alg_t mac_alg;

    sha256_ctx_t handshake_hash;
    int handshake_done;

    uint64_t seq_out;
    uint64_t seq_in;

    uint8_t enc_client;
    uint8_t enc_server;

    uint8_t rx_buf[TLS_RECORD_CAP];
    uint32_t rx_len;
    uint32_t rx_offset;

    uint8_t tx_buf[TLS_CAP];
    uint32_t tx_len;
};

static uint16_t tls_next_src_port = 45000;

static void tls_log(const char *msg) {
    (void)msg;
}

static uint16_t tls_alloc_src_port(void) {
    uint16_t port = tls_next_src_port++;
    if (tls_next_src_port < 45000 || tls_next_src_port >= 60000) {
        tls_next_src_port = 45000;
    }
    return port;
}

static void tls_hmac(const uint8_t *key, uint32_t key_len,
                     tls_mac_alg_t alg,
                     const uint8_t *data, uint32_t data_len,
                     uint8_t *out) {
    if (alg == TLS_MAC_SHA1) {
        hmac_sha1(key, key_len, data, data_len, out);
    } else {
        hmac_sha256(key, key_len, data, data_len, out);
    }
}

static uint16_t r16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8) | p[1];
}

static uint32_t r24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void w24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v);
}

static uint32_t tls_prf(const uint8_t *secret, int secret_len,
                         const char *label,
                         const uint8_t *seed, int seed_len,
                         uint8_t *out, int out_len) {
    int label_len = 0;
    while (label[label_len]) label_len++;

    uint8_t combined[256];
    for (int i = 0; i < label_len; i++) combined[i] = (uint8_t)label[i];
    for (int i = 0; i < seed_len; i++) combined[label_len + i] = seed[i];
    int combined_len = label_len + seed_len;

    int generated = 0;
    uint8_t A[32];
    hmac_sha256(secret, secret_len, combined, combined_len, A);

    uint8_t to_hash[512];
    while (generated < out_len) {
        for (int i = 0; i < 32; i++) to_hash[i] = A[i];
        for (int i = 0; i < combined_len; i++) to_hash[32 + i] = combined[i];

        uint8_t hmac_out[32];
        hmac_sha256(secret, secret_len, to_hash, 32 + combined_len, hmac_out);

        int remain = out_len - generated;
        int copy = remain < 32 ? remain : 32;
        for (int i = 0; i < copy; i++) out[generated + i] = hmac_out[i];
        generated += copy;

        hmac_sha256(secret, secret_len, A, 32, A);
    }
    return 0;
}

static int tls_send_record(tls_conn_t *conn, uint8_t type, const uint8_t *data, uint16_t len) {
    uint8_t frame[TLS_CAP + 256];
    uint8_t *payload;
    uint16_t frame_len;

    if (conn->enc_client && type != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
        uint8_t mac_buf[32];
        uint8_t seq_buf[8];
        uint8_t plaintext[TLS_CAP + 64];
        uint8_t aad[13];
        int pt_len = len;

        for (int i = 0; i < 8; i++) seq_buf[i] = (uint8_t)(conn->seq_out >> (56 - i * 8));
        aad[0] = type;
        aad[1] = TLS_VERSION_MAJOR;
        aad[2] = TLS_VERSION_MINOR;
        w16(aad + 3, len);

        uint8_t hmac_in[8 + 1 + 2 + 2 + TLS_CAP];
        int hi = 0;
        for (int i = 0; i < 8; i++) hmac_in[hi++] = seq_buf[i];
        hmac_in[hi++] = aad[0];
        hmac_in[hi++] = aad[1];
        hmac_in[hi++] = aad[2];
        hmac_in[hi++] = (uint8_t)(len >> 8);
        hmac_in[hi++] = (uint8_t)(len & 0xFF);
        for (int i = 0; i < len; i++) hmac_in[hi++] = data[i];
        tls_hmac(conn->client_write_mac_key, conn->mac_key_len, conn->mac_alg, hmac_in, hi, mac_buf);

        for (int i = 0; i < len; i++) plaintext[i] = data[i];
        for (int i = 0; i < conn->mac_len; i++) plaintext[len + i] = mac_buf[i];
        pt_len = len + conn->mac_len;

        int pad_plus_one = AES_BLOCK_SIZE - (pt_len % AES_BLOCK_SIZE);
        if (pad_plus_one == 0) pad_plus_one = AES_BLOCK_SIZE;
        int pad_len = pad_plus_one - 1;
        for (int i = 0; i <= pad_len; i++) plaintext[pt_len + i] = (uint8_t)pad_len;
        pt_len += pad_len + 1;

        tls_record_hdr_t *rec = (tls_record_hdr_t *)frame;
        rec->type = type;
        rec->version = (TLS_VERSION_MAJOR << 8) | TLS_VERSION_MINOR;

        uint8_t *body = frame + sizeof(tls_record_hdr_t);
        uint8_t iv[AES_BLOCK_SIZE];
        uint32_t rseed = (uint32_t)(conn->seq_out + sched_ticks());
        for (int i = 0; i < AES_BLOCK_SIZE; i++) {
            iv[i] = (uint8_t)(rseed + i * 13);
            body[i] = iv[i];
        }
        aes128_cbc_encrypt(conn->client_enc_expanded, iv, plaintext, pt_len, body + 16);
        uint16_t enc_len = (uint16_t)(16 + pt_len);
        w16((uint8_t*)&rec->length, enc_len);

        frame_len = sizeof(tls_record_hdr_t) + enc_len;
        conn->seq_out++;
    } else {
        tls_record_hdr_t *rec = (tls_record_hdr_t *)frame;
        rec->type = type;
        rec->version = (TLS_VERSION_MAJOR << 8) | TLS_VERSION_MINOR;
        rec->length = 0;
        w16((uint8_t*)&rec->length, len);
        payload = frame + sizeof(tls_record_hdr_t);
        for (int i = 0; i < len; i++) payload[i] = data[i];
        frame_len = sizeof(tls_record_hdr_t) + len;
    }

    uint8_t eth_frame[NET_FRAME_CAP];
    eth_hdr_t *eth = (eth_hdr_t *)eth_frame;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(eth_frame + sizeof(eth_hdr_t));
    tcp_hdr_t *tcp = (tcp_hdr_t *)(eth_frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t *tcp_data = eth_frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t);
    uint16_t ip_len = (uint16_t)(sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t) + frame_len);
    uint16_t eth_frame_len = (uint16_t)(sizeof(eth_hdr_t) + ip_len);

    if (eth_frame_len > sizeof(eth_frame)) return -1;
    for (uint64_t i = 0; i < eth_frame_len; i++) eth_frame[i] = 0;

    build_eth(eth, conn->dst_mac, ETH_TYPE_IPV4);
    ip->ver_ihl = 0x45;
    ip->total_len_be = htons16(ip_len);
    ip->ident_be = htons16((uint16_t)conn->tcp_seq);
    ip->ttl = 64;
    ip->proto = IP_PROTO_TCP;
    ip->src_be = net_state.ip;
    ip->dst_be = conn->dst_ip;
    ip->checksum_be = htons16((uint16_t)ip_checksum(ip, sizeof(ipv4_hdr_t)));

    tcp->src_port_be = htons16(conn->src_port);
    tcp->dst_port_be = htons16(conn->dst_port);
    tcp->seq_be = htonl32(conn->tcp_seq);
    tcp->ack_be = htonl32(conn->tcp_ack);
    tcp->data_offset = (uint8_t)(sizeof(tcp_hdr_t) / 4U) << 4;
    tcp->flags = TCP_FLAG_ACK | TCP_FLAG_PSH;
    tcp->window_be = htons16(4096);
    tcp->urgent_be = 0;
    for (int i = 0; i < frame_len; i++) tcp_data[i] = frame[i];
    tcp->checksum_be = htons16(tcp_checksum(ip, tcp, tcp_data, frame_len));

    int rc = e1000_send_frame(eth_frame, eth_frame_len);
    if (rc == 0) conn->tcp_seq += frame_len;
    return rc;
}

static int tls_send_handshake(tls_conn_t *conn, uint8_t htype, const uint8_t *data, uint32_t len) {
    uint8_t buf[TLS_CAP];
    tls_handshake_hdr_t *hdr = (tls_handshake_hdr_t *)buf;
    hdr->type = htype;
    w24((uint8_t*)&hdr->length, len);
    for (uint32_t i = 0; i < len; i++) buf[4 + i] = data[i];
    uint32_t total = 4 + len;
    sha256_update(&conn->handshake_hash, buf, total);
    return tls_send_record(conn, TLS_CONTENT_HANDSHAKE, buf, (uint16_t)total);
}

static int tls_recv_frame(tls_conn_t *conn, uint64_t timeout_ticks) {
    uint8_t frame[NET_FRAME_CAP];
    uint16_t len = 0;
    uint64_t deadline = sched_ticks() + timeout_ticks;

    while (sched_ticks() < deadline) {
        int rc = e1000_recv_frame(frame, sizeof(frame), &len);
        if (rc < 0) return -1;
        if (rc == 0) { sched_sleep(1); continue; }

        tcp_packet_info_t pkt;
        if (!parse_tcp_packet(frame, len, conn->dst_ip, conn->dst_port, conn->src_port, &pkt)) continue;

        if (pkt.flags & 0x04U) return -2;
        if (pkt.payload_len && pkt.seq == conn->tcp_ack) {
            uint32_t copy = pkt.payload_len;
            if (conn->rx_len + copy > TLS_RECORD_CAP) copy = (uint32_t)(TLS_RECORD_CAP - conn->rx_len);
            for (uint32_t i = 0; i < copy; i++) conn->rx_buf[conn->rx_len + i] = pkt.payload[i];
            conn->rx_len += copy;
            conn->tcp_ack += pkt.payload_len;

            uint8_t ack_frame[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t)];
            eth_hdr_t *ae = (eth_hdr_t *)ack_frame;
            ipv4_hdr_t *ai = (ipv4_hdr_t *)(ack_frame + sizeof(eth_hdr_t));
            tcp_hdr_t *at = (tcp_hdr_t *)(ack_frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));

            for (int i = 0; i < (int)sizeof(ack_frame); i++) ack_frame[i] = 0;
            build_eth(ae, conn->dst_mac, ETH_TYPE_IPV4);
            ai->ver_ihl = 0x45;
            ai->total_len_be = htons16((uint16_t)(sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t)));
            ai->ident_be = htons16((uint16_t)conn->tcp_seq);
            ai->ttl = 64;
            ai->proto = IP_PROTO_TCP;
            ai->src_be = net_state.ip;
            ai->dst_be = conn->dst_ip;
            ai->checksum_be = htons16((uint16_t)ip_checksum(ai, sizeof(ipv4_hdr_t)));
            at->src_port_be = htons16(conn->src_port);
            at->dst_port_be = htons16(conn->dst_port);
            at->seq_be = htonl32(conn->tcp_seq);
            at->ack_be = htonl32(conn->tcp_ack);
            at->data_offset = (uint8_t)(sizeof(tcp_hdr_t) / 4U) << 4;
            at->flags = TCP_FLAG_ACK;
            at->window_be = htons16(4096);
            at->checksum_be = htons16(tcp_checksum(ai, at, 0, 0));
            e1000_send_frame(ack_frame, sizeof(ack_frame));
            return 1;
        }
        if (pkt.flags & TCP_FLAG_FIN) {
            conn->tcp_ack += 1U;
            return -3;
        }
    }
    return 0;
}

static int tls_decrypt_record(tls_conn_t *conn, uint8_t record_type, uint8_t *data, uint16_t len, uint8_t *out, uint16_t *out_len) {
    if (len < 16) return -1;
    uint8_t iv[AES_BLOCK_SIZE];
    for (int i = 0; i < 16; i++) iv[i] = data[i];
    uint8_t *ciphertext = data + 16;
    int ct_len = len - 16;

    if (ct_len % AES_BLOCK_SIZE != 0) return -1;

    uint8_t *plaintext = (uint8_t *)kmalloc(ct_len);
    if (!plaintext) return -1;
    aes128_cbc_decrypt(conn->server_enc_expanded, iv, ciphertext, ct_len, plaintext);

    int pad_len = plaintext[ct_len - 1];
    if (pad_len < 0 || pad_len >= ct_len) { kfree(plaintext); return -1; }
    for (int i = 0; i <= pad_len; i++) {
        if (plaintext[ct_len - 1 - i] != (uint8_t)pad_len) {
            kfree(plaintext);
            return -1;
        }
    }

    int content_len = ct_len - pad_len - 1 - conn->mac_len;
    if (content_len < 0) { kfree(plaintext); return -1; }

    uint8_t seq_buf[8];
    for (int i = 0; i < 8; i++) seq_buf[i] = (uint8_t)(conn->seq_in >> (56 - i * 8));

    uint8_t hmac_data[8 + 1 + 2 + 2 + TLS_CAP];
    int hi = 0;
    for (int i = 0; i < 8; i++) hmac_data[hi++] = seq_buf[i];
    hmac_data[hi++] = record_type;
    hmac_data[hi++] = TLS_VERSION_MAJOR;
    hmac_data[hi++] = TLS_VERSION_MINOR;
    hmac_data[hi++] = (uint8_t)(content_len >> 8);
    hmac_data[hi++] = (uint8_t)(content_len & 0xFF);
    for (int i = 0; i < content_len; i++) hmac_data[hi++] = plaintext[i];

    uint8_t computed_mac[32];
    tls_hmac(conn->server_write_mac_key, conn->mac_key_len, conn->mac_alg, hmac_data, hi, computed_mac);

    uint8_t *received_mac = plaintext + content_len;
    int mac_ok = 1;
    for (int i = 0; i < conn->mac_len; i++) {
        if (computed_mac[i] != received_mac[i]) mac_ok = 0;
    }

    if (!mac_ok) { kfree(plaintext); return -1; }

    for (int i = 0; i < content_len; i++) out[i] = plaintext[i];
    *out_len = (uint16_t)content_len;
    kfree(plaintext);
    conn->seq_in++;
    return 0;
}

static int tls_parse_record(tls_conn_t *conn, uint8_t *content_type, uint8_t *payload, uint16_t *payload_len) {
    while (conn->rx_offset + 5 <= conn->rx_len) {
        uint8_t *p = conn->rx_buf + conn->rx_offset;
        uint8_t rtype = p[0];
        uint16_t rlen = r16(p + 3);
        if (conn->rx_offset + 5 + rlen > conn->rx_len) break;

        uint8_t *rdata = p + 5;

        if (conn->enc_server && rtype != TLS_CONTENT_CHANGE_CIPHER_SPEC) {
            uint8_t decrypted[TLS_CAP];
            uint16_t dlen = 0;
            if (tls_decrypt_record(conn, rtype, rdata, rlen, decrypted, &dlen) == 0) {
                *content_type = rtype;
                for (int i = 0; i < dlen; i++) payload[i] = decrypted[i];
                *payload_len = dlen;
                conn->rx_offset = conn->rx_offset + 5 + rlen;
                return 1;
            }
            return -1;
        } else {
            *content_type = rtype;
            *payload_len = rlen;
            for (int i = 0; i < rlen; i++) payload[i] = rdata[i];
            conn->rx_offset = conn->rx_offset + 5 + rlen;
            return 1;
        }
    }
    return 0;
}

int tls_connect(tls_conn_t **conn_out, uint32_t ip, uint16_t port, const char *server_name) {
    tls_conn_t *conn = (tls_conn_t *)kmalloc(sizeof(tls_conn_t));
    if (!conn) return -1;
    for (uint64_t i = 0; i < sizeof(tls_conn_t); i++) ((uint8_t*)conn)[i] = 0;

    uint32_t arp_target = ip_same_subnet(net_state.ip, ip, net_state.netmask) ? ip : net_state.gateway;
    if (net_arp_resolve(arp_target, conn->dst_mac) != 0) {
        tls_log("arp failed");
        kfree(conn);
        return -2;
    }

    conn->dst_ip = ip;
    conn->dst_port = port;
    conn->src_port = tls_alloc_src_port();
    conn->tcp_seq = 0x12345679;

    tls_log("tcp connecting...");
    {
        int rc = tcp_connect(conn->dst_mac, conn->dst_ip, conn->dst_port, conn->src_port,
                             &conn->tcp_seq, &conn->tcp_ack);
        if (rc == -2) {
            tls_log("tcp refused");
            kfree(conn);
            return -4;
        }
        if (rc != 0) {
            tls_log("tcp connect timeout");
            kfree(conn);
            return -3;
        }
    }
    tls_log("tcp connected, starting handshake");

    sha256_init(&conn->handshake_hash);
    conn->cipher_suite = TLS_CIPHER_RSA_AES128_CBC_SHA256;
    conn->mac_alg = TLS_MAC_SHA256;
    conn->mac_key_len = 32;
    conn->mac_len = 32;

    uint8_t ch[TLS_CAP];
    int ch_len = 0;
    ch[ch_len++] = TLS_VERSION_MAJOR;
    ch[ch_len++] = TLS_VERSION_MINOR;

    uint32_t rand_seed = sched_ticks();
    for (int i = 0; i < 32; i++) {
        conn->client_random[i] = (uint8_t)(rand_seed + i * 17 + (sched_ticks() & 0xFF));
        ch[ch_len++] = conn->client_random[i];
    }
    ch[ch_len++] = 0;
    ch[ch_len++] = 0;
    ch[ch_len++] = 6;
    ch[ch_len++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256 >> 8);
    ch[ch_len++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA256 & 0xFF);
    ch[ch_len++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA >> 8);
    ch[ch_len++] = (uint8_t)(TLS_CIPHER_RSA_AES128_CBC_SHA & 0xFF);
    ch[ch_len++] = 0x00;
    ch[ch_len++] = 0xFF; /* TLS_EMPTY_RENEGOTIATION_INFO_SCSV */
    ch[ch_len++] = 1;
    ch[ch_len++] = 0;

    {
        uint16_t ext_len_off = (uint16_t)ch_len;
        uint16_t ext_total = 0;
        ch[ch_len++] = 0;
        ch[ch_len++] = 0;

        if (server_name && server_name[0]) {
            uint16_t host_len = 0;
            while (server_name[host_len] && host_len < 255) host_len++;
            if (host_len > 0) {
                ch[ch_len++] = 0x00;
                ch[ch_len++] = 0x00;
                ch[ch_len++] = (uint8_t)((5 + host_len) >> 8);
                ch[ch_len++] = (uint8_t)((5 + host_len) & 0xFF);
                ch[ch_len++] = (uint8_t)((3 + host_len) >> 8);
                ch[ch_len++] = (uint8_t)((3 + host_len) & 0xFF);
                ch[ch_len++] = 0x00;
                ch[ch_len++] = (uint8_t)(host_len >> 8);
                ch[ch_len++] = (uint8_t)(host_len & 0xFF);
                for (uint16_t i = 0; i < host_len; i++) ch[ch_len++] = (uint8_t)server_name[i];
            }
        }

        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x0d;
        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x0a;
        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x08;
        ch[ch_len++] = 0x04; ch[ch_len++] = 0x01; // rsa_pkcs1_sha256
        ch[ch_len++] = 0x05; ch[ch_len++] = 0x01; // rsa_pkcs1_sha384
        ch[ch_len++] = 0x06; ch[ch_len++] = 0x01; // rsa_pkcs1_sha512
        ch[ch_len++] = 0x02; ch[ch_len++] = 0x01; // rsa_pkcs1_sha1

        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x0a;
        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x08;
        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x06;
        ch[ch_len++] = 0x00; ch[ch_len++] = 0x17; // secp256r1
        ch[ch_len++] = 0x00; ch[ch_len++] = 0x18; // secp384r1
        ch[ch_len++] = 0x00; ch[ch_len++] = 0x19; // secp521r1

        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x0b;
        ch[ch_len++] = 0x00;
        ch[ch_len++] = 0x02;
        ch[ch_len++] = 0x01;
        ch[ch_len++] = 0x00; // uncompressed EC point format

        ext_total = (uint16_t)(ch_len - ext_len_off - 2);
        ch[ext_len_off] = (uint8_t)(ext_total >> 8);
        ch[ext_len_off + 1] = (uint8_t)(ext_total & 0xFF);
    }

    tls_log("sending client hello...");
    if (tls_send_handshake(conn, TLS_HANDSHAKE_CLIENT_HELLO, ch, ch_len) != 0) {
        kfree(conn);
        return -1;
    }

    uint8_t payload[TLS_CAP];
    uint16_t plen = 0;
    uint8_t ctype = 0;
    int server_done = 0;
    rsa_pubkey_t server_key;

    while (!server_done) {
        int rc = tls_recv_frame(conn, 1000);
        if (rc < 0) {
            if (rc == -3) { tls_log("server closed"); break; }
            tls_log("recv failed");
            kfree(conn);
            return -1;
        }
        if (rc == 0) continue;

        while (1) {
            int ret = tls_parse_record(conn, &ctype, payload, &plen);
            if (ret <= 0) break;

            if (ctype == TLS_CONTENT_HANDSHAKE) {
                uint32_t off = 0;
                while (off + 4 <= plen) {
                    uint8_t ht = payload[off];
                    uint32_t hs_len = r24(payload + off + 1);
                    uint8_t *hs_data = payload + off + 4;
                    if (off + 4 + hs_len > plen) {
                        tls_log("truncated handshake record");
                        kfree(conn);
                        return -1;
                    }

                    sha256_update(&conn->handshake_hash, payload + off, 4 + hs_len);

                    if (ht == TLS_HANDSHAKE_SERVER_HELLO) {
                        conn->server_random[0] = TLS_VERSION_MAJOR;
                        conn->server_random[1] = TLS_VERSION_MINOR;
                        for (int i = 0; i < 32 && i + 2 < (int)hs_len; i++) conn->server_random[i] = hs_data[2 + i];
                        if (hs_len >= 38) {
                            uint8_t sid_len = hs_data[34];
                            if ((uint32_t)(35 + sid_len + 2) <= hs_len) {
                                conn->cipher_suite = r16(hs_data + 35 + sid_len);
                                if (conn->cipher_suite == TLS_CIPHER_RSA_AES128_CBC_SHA) {
                                    conn->mac_alg = TLS_MAC_SHA1;
                                    conn->mac_key_len = 20;
                                    conn->mac_len = 20;
                                } else {
                                    conn->mac_alg = TLS_MAC_SHA256;
                                    conn->mac_key_len = 32;
                                    conn->mac_len = 32;
                                }
                            }
                        }
                        tls_log("got server hello");
                    } else if (ht == TLS_HANDSHAKE_CERTIFICATE) {
                        tls_log("got certificate");
                        int cert_list_len = r24(hs_data);
                        (void)cert_list_len;
                        int offset = 3;
                        if (offset + 3 <= (int)hs_len) {
                            int cert_len = r24(hs_data + offset);
                            offset += 3;
                            if (offset + cert_len <= (int)hs_len) {
                                if (rsa_pubkey_from_cert_der(hs_data + offset, cert_len, &server_key) != 0) {
                                    tls_log("cert parse failed");
                                    kfree(conn);
                                    return -1;
                                }
                                tls_log("rsa key extracted from cert");
                            }
                        }
                    } else if (ht == TLS_HANDSHAKE_SERVER_HELLO_DONE) {
                        tls_log("server hello done");
                        server_done = 1;
                    } else {
                        console_write("[tls] hs type=", CONSOLE_STYLE_MUTED);
                        console_write_dec64(ht, CONSOLE_STYLE_INFO);
                        console_write(" len=", CONSOLE_STYLE_MUTED);
                        console_write_dec64(hs_len, CONSOLE_STYLE_INFO);
                        console_write("\n", CONSOLE_STYLE_INFO);
                    }

                    off += 4 + hs_len;
                }
            } else if (ctype == TLS_CONTENT_ALERT) {
                int alert_level = payload[0];
                int alert_desc = payload[1];
                console_write("[tls] alert level=", CONSOLE_STYLE_WARN);
                console_write_dec64(alert_level, CONSOLE_STYLE_WARN);
                console_write(" desc=", CONSOLE_STYLE_WARN);
                console_write_dec64(alert_desc, CONSOLE_STYLE_WARN);
                console_write("\n", CONSOLE_STYLE_WARN);
                if (alert_level == TLS_ALERT_LEVEL_FATAL) {
                    kfree(conn);
                    return -1;
                }
            }
        }
    }

    if (!server_done) {
        tls_log("handshake incomplete");
        kfree(conn);
        return -1;
    }

    tls_log("sending client key exchange...");

    conn->pre_master_secret[0] = TLS_VERSION_MAJOR;
    conn->pre_master_secret[1] = TLS_VERSION_MINOR;
    uint32_t rseed = sched_ticks();
    for (int i = 2; i < 48; i++) {
        conn->pre_master_secret[i] = (uint8_t)(rseed + i * 31 + (sched_ticks() & 0xFF));
    }

    int mod_bytes = 0;
    bn_to_bytes(&server_key.n, conn->tx_buf, &mod_bytes);

    uint8_t encrypted_pms[512];
    int enc_len = 0;
    if (rsa_encrypt(&server_key, conn->pre_master_secret, 48, encrypted_pms, &enc_len) != 0) {
        tls_log("rsa encrypt failed");
        kfree(conn);
        return -1;
    }

    (void)enc_len;
    uint8_t cke[2 + 512];
    cke[0] = (uint8_t)(enc_len >> 8);
    cke[1] = (uint8_t)(enc_len & 0xFF);
    for (int i = 0; i < enc_len; i++) cke[2 + i] = encrypted_pms[i];

    tls_send_handshake(conn, TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE, cke, 2 + enc_len);

    uint8_t seed[64];
    for (int i = 0; i < 32; i++) seed[i] = conn->client_random[i];
    for (int i = 0; i < 32; i++) seed[32 + i] = conn->server_random[i];

    tls_prf(conn->pre_master_secret, 48, "master secret", seed, 64, conn->master_secret, 48);

    uint8_t key_seed[64];
    for (int i = 0; i < 32; i++) key_seed[i] = conn->server_random[i];
    for (int i = 0; i < 32; i++) key_seed[32 + i] = conn->client_random[i];

    uint8_t key_block[128];
    uint32_t key_block_len = (uint32_t)(conn->mac_key_len * 2 + 16 + 16 + 4 + 4);
    uint32_t kb = 0;
    tls_prf(conn->master_secret, 48, "key expansion", key_seed, 64, key_block, (int)key_block_len);

    for (uint32_t i = 0; i < conn->mac_key_len; i++) conn->client_write_mac_key[i] = key_block[kb++];
    for (uint32_t i = 0; i < conn->mac_key_len; i++) conn->server_write_mac_key[i] = key_block[kb++];
    for (int i = 0; i < 16; i++) conn->client_write_key[i] = key_block[kb++];
    for (int i = 0; i < 16; i++) conn->server_write_key[i] = key_block[kb++];
    for (int i = 0; i < 4; i++) {
        conn->client_write_iv[i] = key_block[kb++];
        conn->server_write_iv[i] = key_block[kb++];
    }
    for (int i = 0; i < 16; i++) {
        conn->client_write_iv[i + 4] = 0;
        conn->server_write_iv[i + 4] = 0;
    }

    aes128_expand_key(conn->client_write_key, conn->client_enc_expanded);
    aes128_expand_key(conn->server_write_key, conn->server_enc_expanded);

    conn->enc_client = 1;

    tls_log("sending change cipher spec...");
    uint8_t ccs = 1;
    tls_send_record(conn, TLS_CONTENT_CHANGE_CIPHER_SPEC, &ccs, 1);

    uint8_t finished_hash[32];
    uint8_t fin_verify[12];
    sha256_final(&conn->handshake_hash, finished_hash);
    tls_prf(conn->master_secret, 48, "client finished", finished_hash, 32, fin_verify, 12);

    tls_send_handshake(conn, TLS_HANDSHAKE_FINISHED, fin_verify, 12);
    tls_log("sent finished, waiting for server...");

    int server_finished_ok = 0;
    uint64_t fin_deadline = sched_ticks() + 500;
    while (sched_ticks() < fin_deadline && !server_finished_ok) {
        int rc = tls_recv_frame(conn, 200);
        if (rc < 0) break;
        if (rc == 0) { sched_sleep(1); continue; }

        uint8_t sp[TLS_CAP];
        uint16_t sp_len = 0;
        uint8_t sct = 0;
        while (1) {
            int ret = tls_parse_record(conn, &sct, sp, &sp_len);
            if (ret <= 0) break;
            if (sct == TLS_CONTENT_CHANGE_CIPHER_SPEC) {
                tls_log("server ccs received");
                conn->enc_server = 1;
            } else if (sct == TLS_CONTENT_HANDSHAKE) {
                uint32_t off = 0;
                while (off + 4 <= sp_len) {
                    uint8_t ht = sp[off];
                    uint32_t hs_len = r24(sp + off + 1);
                    if (off + 4 + hs_len > sp_len) break;
                    if (ht == TLS_HANDSHAKE_FINISHED) {
                        tls_log("handshake complete!");
                        server_finished_ok = 1;
                        break;
                    }
                    off += 4 + hs_len;
                }
                if (server_finished_ok) break;
            }
        }
    }

    if (!server_finished_ok) {
        tls_log("handshake failed (no server finished)");
        kfree(conn);
        return -1;
    }

    *conn_out = conn;
    return 0;
}

int tls_write(tls_conn_t *conn, const uint8_t *data, uint32_t len) {
    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > 16384) chunk = 16384;
        if (tls_send_record(conn, TLS_CONTENT_APPLICATION_DATA, data + offset, (uint16_t)chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }
    return 0;
}

int tls_read(tls_conn_t *conn, uint8_t *buf, uint32_t cap, uint32_t *out_len) {
    uint64_t deadline = sched_ticks() + 500;

    while (sched_ticks() < deadline) {
        uint8_t payload[TLS_CAP];
        uint16_t plen = 0;
        uint8_t ctype = 0;

        int rc = tls_recv_frame(conn, 50);
        if (rc < 0) return -1;

        while (1) {
            int ret = tls_parse_record(conn, &ctype, payload, &plen);
            if (ret <= 0) break;
            if (ctype == TLS_CONTENT_APPLICATION_DATA) {
                uint32_t copy = plen < cap ? plen : cap;
                for (uint32_t i = 0; i < copy; i++) buf[i] = payload[i];
                *out_len = copy;
                return 0;
            }
        }
        sched_sleep(1);
    }
    return -1;
}

void tls_close(tls_conn_t *conn) {
    if (!conn) return;
    send_tcp_packet(conn->dst_mac, conn->dst_ip, conn->src_port, conn->dst_port,
                    conn->tcp_seq, conn->tcp_ack, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    conn->tcp_seq++;
    kfree(conn);
}
