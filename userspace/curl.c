#include "icda_sys.h"

#include <stdint.h>

#define CURL_REQUEST_PATH "/home/.curl.request"
#define CURL_TEMP_PATH "/home/.curl.temp"
#define CURL_REQ_CAP 384
#define CURL_URL_CAP 256
#define CURL_HOST_CAP 128
#define CURL_OUT_CAP 128
#define CURL_PATH_CAP 256
#define CURL_CONSOLE_BUF 512

static char curl_req[CURL_REQ_CAP];
static char curl_url[CURL_URL_CAP];
static char curl_host[CURL_HOST_CAP];
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

static int try_parse_ipv4(const char *host, uint32_t *ip_out) {
    const char *cursor = host;
    uint32_t octets[4];
    uint64_t used = 0;

    if (!host || !*host || !ip_out) return -1;
    if (parse_u8(cursor, &octets[0], &used) != 0) return -1;
    cursor += used;
    if (*cursor++ != '.') return -1;
    if (parse_u8(cursor, &octets[1], &used) != 0) return -1;
    cursor += used;
    if (*cursor++ != '.') return -1;
    if (parse_u8(cursor, &octets[2], &used) != 0) return -1;
    cursor += used;
    if (*cursor++ != '.') return -1;
    if (parse_u8(cursor, &octets[3], &used) != 0) return -1;
    cursor += used;
    if (*cursor != 0) return -1;
    *ip_out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

static int parse_http_url(uint32_t *ip_out, uint16_t *port_out, int *https_out) {
    const char *host;
    uint64_t host_len = 0;
    uint64_t path_len = 0;
    uint16_t port = 80;
    uint32_t ip;
    int use_https = 0;

    if (!str_len(curl_url) || !ip_out || !port_out || !https_out) return -1;
    if ((curl_url[0] == 'h') && (curl_url[1] == 't') && (curl_url[2] == 't') && (curl_url[3] == 'p') &&
        (curl_url[4] == ':') && (curl_url[5] == '/') && (curl_url[6] == '/')) {
        host = curl_url + 7;
    } else if ((curl_url[0] == 'h') && (curl_url[1] == 't') && (curl_url[2] == 't') && (curl_url[3] == 'p') &&
               (curl_url[4] == 's') && (curl_url[5] == ':') && (curl_url[6] == '/') && (curl_url[7] == '/')) {
        host = curl_url + 8;
        port = 443;
        use_https = 1;
    } else {
        return -1;
    }

    while (host[host_len] && host[host_len] != ':' && host[host_len] != '/') {
        if (host_len + 1 >= sizeof(curl_host)) return -1;
        curl_host[host_len] = host[host_len];
        host_len++;
    }
    if (host_len == 0) return -1;
    curl_host[host_len] = 0;
    host += host_len;

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
        while (host[path_len] && path_len + 1 < sizeof(curl_path)) {
            curl_path[path_len] = host[path_len];
            path_len++;
        }
        curl_path[path_len] = 0;
    }

    if (try_parse_ipv4(curl_host, &ip) != 0) {
        uint32_t resolved = 0;
        long rc = (long)icda_dns_resolve(curl_host, &resolved);
        if (rc < 0) return (int)rc;
        ip = resolved;
    }
    *ip_out = ip;
    *port_out = port;
    *https_out = use_https;
    return 0;
}

uint64_t curl_main(void) {
    long req_len;
    uint32_t ip = 0;
    uint16_t port = 80;
    uint64_t bytes = 0;
    int use_https = 0;
    int parse_rc = 0;

    req_len = (long)icda_read_file(CURL_REQUEST_PATH, curl_req, sizeof(curl_req) - 1);
    if (req_len <= 0) {
        icda_write("curl: no request\n");
        icda_exit(1);
    }
    curl_req[req_len] = 0;
    split_request();

    if (!curl_url[0]) {
        icda_write("curl: bad request\n");
        icda_exit(1);
    }
    parse_rc = parse_http_url(&ip, &port, &use_https);
    if (parse_rc == -9) {
        icda_write("curl: dns timed out while resolving host\n");
        icda_exit(1);
    }
    if (parse_rc == -10) {
        icda_write("curl: dns failed to resolve host\n");
        icda_exit(1);
    }
    if (parse_rc != 0) {
        icda_write("curl: usage: curl <http://host[:port]/path> <out-path>\n");
        icda_exit(1);
    }
    {
        const char *target = curl_out[0] ? curl_out : CURL_TEMP_PATH;
        long rc;
        if (use_https) {
            rc = (long)icda_https_get_ipv4(ip, port, curl_host, curl_path, target, &bytes);
        } else {
            rc = (long)icda_http_get_ipv4(ip, port, curl_host, curl_path, target, &bytes);
        }
        if (rc < 0) {
            long err = -rc;
            if (err >= 2000 && err < 3000) {
                icda_write("curl: HTTP ");
                write_uint((uint64_t)(err - 2000));
                icda_write("\n");
            } else if (err == 2) {
                icda_write("curl: ARP timeout\n");
            } else if (err == 3) {
                icda_write("curl: TCP timeout\n");
            } else if (err == 4) {
                icda_write("curl: TCP refused\n");
            } else if (err == 5) {
                icda_write("curl: bad HTTP response\n");
            } else if (err == 6) {
                icda_write("curl: response too large\n");
            } else if (err == 7) {
                icda_write("curl: failed writing file\n");
            } else if (err == 8) {
                icda_write("curl: bad URL path\n");
            } else if (err == 9) {
                icda_write("curl: DNS timeout\n");
            } else if (err == 10) {
                icda_write("curl: DNS parse failure\n");
            } else if (err == 11) {
                icda_write("curl: TLS handshake failed\n");
            } else if (err == 12) {
                icda_write("curl: TLS recv failed\n");
            } else {
                icda_write("curl: download failed err=");
                write_uint((uint64_t)err);
                icda_write("\n");
            }
            icda_exit(1);
        }
    }

    if (!curl_out[0]) {
        char console_buf[CURL_CONSOLE_BUF + 1];
        uint64_t offset = 0;
        while (offset < bytes) {
            uint64_t chunk = icda_read_file_at(CURL_TEMP_PATH, offset, console_buf, CURL_CONSOLE_BUF);
            if (chunk <= 0) break;
            console_buf[chunk] = 0;
            icda_write(console_buf);
            offset += chunk;
        }
    } else {
        icda_write("downloaded ");
        write_uint(bytes);
        icda_write(" bytes to ");
        icda_write(curl_out);
        icda_write("\n");
    }
    icda_exit(0);
    return 0;
}
