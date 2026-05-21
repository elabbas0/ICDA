#include "icda_sys.h"

#include <stdint.h>

#define CURL_REQUEST_PATH "/home/.curl.request"
#define CURL_REQ_CAP 384
#define CURL_URL_CAP 256
#define CURL_OUT_CAP 128
#define CURL_PATH_CAP 256

static char curl_req[CURL_REQ_CAP];
static char curl_url[CURL_URL_CAP];
static char curl_out[CURL_OUT_CAP];
static char curl_path[CURL_PATH_CAP];

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void write_uint(uint64_t v) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0) {
        icda_write("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    icda_write(&buf[i]);
}

static void split_request(void) {
    uint64_t i = 0;
    uint64_t out_i = 0;

    curl_url[0] = 0;
    curl_out[0] = 0;
    while (curl_req[i] && curl_req[i] != '\n' && i + 1 < sizeof(curl_url)) {
        curl_url[i] = curl_req[i];
        i++;
    }
    curl_url[i] = 0;
    if (curl_req[i] == '\n') i++;
    while (curl_req[i] && out_i + 1 < sizeof(curl_out)) {
        curl_out[out_i++] = curl_req[i++];
    }
    curl_out[out_i] = 0;
}

static int parse_u8(const char *text, uint32_t *value_out, uint64_t *consumed_out) {
    uint32_t value = 0;
    uint64_t i = 0;
    if (!text || text[0] < '0' || text[0] > '9' || !value_out || !consumed_out) return -1;
    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10U + (uint32_t)(text[i] - '0');
        if (value > 255U) return -1;
        i++;
    }
    *value_out = value;
    *consumed_out = i;
    return 0;
}

static int parse_http_url(uint32_t *ip_out, uint16_t *port_out) {
    const char *host;
    uint32_t octets[4];
    uint64_t used = 0;
    uint64_t host_len = 0;
    uint16_t port = 80;
    uint32_t ip;

    if (!str_len(curl_url) || !ip_out || !port_out) return -1;
    if (!((curl_url[0] == 'h') && (curl_url[1] == 't') && (curl_url[2] == 't') && (curl_url[3] == 'p') &&
          (curl_url[4] == ':') && (curl_url[5] == '/') && (curl_url[6] == '/'))) {
        return -1;
    }

    host = curl_url + 7;
    if (parse_u8(host, &octets[0], &used) != 0) return -1;
    host += used;
    if (*host++ != '.') return -1;
    if (parse_u8(host, &octets[1], &used) != 0) return -1;
    host += used;
    if (*host++ != '.') return -1;
    if (parse_u8(host, &octets[2], &used) != 0) return -1;
    host += used;
    if (*host++ != '.') return -1;
    if (parse_u8(host, &octets[3], &used) != 0) return -1;
    host += used;

    if (*host == ':') {
        uint32_t port_value = 0;
        host++;
        if (*host < '0' || *host > '9') return -1;
        while (*host >= '0' && *host <= '9') {
            port_value = port_value * 10U + (uint32_t)(*host - '0');
            if (port_value > 65535U) return -1;
            host++;
        }
        port = (uint16_t)port_value;
    }

    if (*host == 0) {
        copy_text(curl_path, "/", sizeof(curl_path));
    } else {
        if (*host != '/') return -1;
        while (host[host_len] && host_len + 1 < sizeof(curl_path)) {
            curl_path[host_len] = host[host_len];
            host_len++;
        }
        curl_path[host_len] = 0;
    }

    ip = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    *ip_out = ip;
    *port_out = port;
    return 0;
}

uint64_t curl_main(void) {
    long req_len;
    uint32_t ip = 0;
    uint16_t port = 80;
    uint64_t bytes = 0;

    req_len = (long)icda_read_file(CURL_REQUEST_PATH, curl_req, sizeof(curl_req) - 1);
    if (req_len <= 0) {
        icda_write("curl: no request\n");
        icda_exit(1);
    }
    curl_req[req_len] = 0;
    split_request();

    if (!curl_url[0] || !curl_out[0]) {
        icda_write("curl: bad request\n");
        icda_exit(1);
    }
    if (parse_http_url(&ip, &port) != 0) {
        icda_write("curl: only plain http://IPv4[:port]/path is supported right now\n");
        icda_exit(1);
    }
    {
        long rc = (long)icda_http_get_ipv4(ip, port, curl_path, curl_out, &bytes);
        if (rc < 0) {
            icda_write("curl: download failed err=");
            write_uint((uint64_t)(-rc));
            icda_write("\n");
            icda_exit(1);
        }
    }

    icda_write("downloaded ");
    write_uint(bytes);
    icda_write(" bytes to ");
    icda_write(curl_out);
    icda_write("\n");
    icda_exit(0);
    return 0;
}
