#include "http.h"
#include "net.h"
#include "tls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define USER_AGENT "Mozilla/5.0 (ICDA; x86_64) Surfer/0.1"

/* ---- URLs --------------------------------------------------------------- */

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ieq_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (lower((unsigned char)*s++) != lower((unsigned char)*prefix++)) return 0;
    }
    return 1;
}

static void copy_cap(char *dst, const char *src, size_t n, size_t cap) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int url_parse(const char *text, url_t *out) {
    const char *p = text, *host, *host_end, *path;
    memset(out, 0, sizeof(*out));
    while (*p == ' ') p++;
    if (ieq_prefix(p, "https://")) {
        out->tls = 1;
        out->port = 443;
        p += 8;
    } else if (ieq_prefix(p, "http://")) {
        out->port = 80;
        p += 7;
    } else if (strstr(p, "://")) {
        return -1;
    } else {
        out->tls = 1;
        out->port = 443;
    }
    host = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') p++;
    host_end = p;
    if (host_end == host || (size_t)(host_end - host) >= HOST_CAP) return -1;
    copy_cap(out->host, host, (size_t)(host_end - host), HOST_CAP);
    for (char *c = out->host; *c; c++) *c = (char)lower((unsigned char)*c);
    if (*p == ':') {
        unsigned v = 0;
        p++;
        while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        if (!v || v > 65535) return -1;
        out->port = (uint16_t)v;
    }
    path = p;
    while (*p && *p != '#') p++;
    if (p == path || *path != '/') {
        out->path[0] = '/';
        copy_cap(out->path + 1, path, (size_t)(p - path), URL_CAP - 1);
    } else {
        copy_cap(out->path, path, (size_t)(p - path), URL_CAP);
    }
    for (char *c = out->path; *c; c++) {
        if (*c == ' ') *c = '+';
    }
    return 0;
}

void url_format(const url_t *u, char *out, size_t cap) {
    int default_port = u->tls ? 443 : 80;
    if (u->port == default_port) snprintf(out, cap, "%s://%s%s", u->tls ? "https" : "http", u->host, u->path);
    else snprintf(out, cap, "%s://%s:%u%s", u->tls ? "https" : "http", u->host, u->port, u->path);
}

/* Removes "." and ".." segments from an absolute path in place. */
static void normalize_path(char *path) {
    char out[URL_CAP];
    size_t o = 0;
    const char *p = path;
    const char *query = strpbrk(path, "?#");
    size_t plen = query ? (size_t)(query - path) : strlen(path);
    while ((size_t)(p - path) < plen) {
        const char *seg = p + 1;
        const char *end = seg;
        while ((size_t)(end - path) < plen && *end != '/') end++;
        if (end - seg == 1 && seg[0] == '.') {
        } else if (end - seg == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
        } else if (o + (size_t)(end - p) < sizeof(out)) {
            memcpy(out + o, p, (size_t)(end - p));
            o += (size_t)(end - p);
        }
        p = end;
        if ((size_t)(p - path) >= plen) break;
    }
    if (o == 0 || (plen > 0 && path[plen - 1] == '/' && out[o - 1] != '/')) out[o++] = '/';
    if (query) {
        size_t q = strlen(query);
        if (o + q < sizeof(out)) {
            memcpy(out + o, query, q);
            o += q;
        }
    }
    out[o] = 0;
    strcpy(path, out);
}

int url_resolve(const char *base, const char *ref, char *out, size_t cap) {
    url_t b;
    char tmp[URL_CAP];
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    if (ieq_prefix(ref, "http://") || ieq_prefix(ref, "https://")) {
        snprintf(out, cap, "%s", ref);
        return 0;
    }
    if (url_parse(base, &b) != 0) return -1;
    if (ref[0] == '/' && ref[1] == '/') {
        snprintf(out, cap, "%s:%s", b.tls ? "https" : "http", ref);
        return 0;
    }
    if (ref[0] == '#' || ref[0] == 0) {
        url_format(&b, out, cap);
        return 0;
    }
    if (ref[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", ref);
    } else if (ref[0] == '?') {
        char *q = strchr(b.path, '?');
        if (q) *q = 0;
        snprintf(tmp, sizeof(tmp), "%s%s", b.path, ref);
    } else {
        char *q = strchr(b.path, '?');
        char *slash;
        if (q) *q = 0;
        slash = strrchr(b.path, '/');
        if (slash) slash[1] = 0;
        snprintf(tmp, sizeof(tmp), "%s%s", b.path, ref);
    }
    {
        char *hash = strchr(tmp, '#');
        if (hash) *hash = 0;
    }
    normalize_path(tmp);
    snprintf(b.path, sizeof(b.path), "%s", tmp);
    url_format(&b, out, cap);
    return 0;
}

/* ---- buffers ------------------------------------------------------------ */

static int grow(uint8_t **buf, size_t *cap, size_t need) {
    size_t n = *cap ? *cap : 4096;
    uint8_t *next;
    if (need <= *cap) return 0;
    while (n < need) n *= 2;
    next = (uint8_t *)realloc(*buf, n);
    if (!next) return -1;
    *buf = next;
    *cap = n;
    return 0;
}

static int append(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t n) {
    if (grow(buf, cap, *len + n + 1) != 0) return -1;
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

/* ---- requests ----------------------------------------------------------- */

static void fail(http_req_t *r, const char *msg) {
    r->state = HTTP_ERROR;
    snprintf(r->error, sizeof(r->error), "%s", msg);
}

static int conn_send(http_req_t *r, const char *data, size_t len) {
    if (r->tls) return tls_write((tls_conn_t *)r->tls, (const uint8_t *)data, (uint32_t)len);
    return net_send_all(r->sock, (const uint8_t *)data, (uint32_t)len);
}

/* >0 bytes, 0 end, -11 nothing yet, other negative error */
static long conn_recv(http_req_t *r, uint8_t *buf, size_t cap, int timeout_ms) {
    if (r->tls) return tls_read((tls_conn_t *)r->tls, buf, (uint32_t)cap, timeout_ms);
    {
        long n = net_recv(r->sock, buf, (uint32_t)cap);
        if (n == NET_EAGAIN && timeout_ms > 0) {
            net_wait(r->sock, NET_POLL_IN, timeout_ms);
            n = net_recv(r->sock, buf, (uint32_t)cap);
        }
        return n;
    }
}

/* ---- cookies -------------------------------------------------------------
 * A small in-memory jar (RFC 6265 subset): host-only and domain cookies,
 * path scoping, Secure, and removal through Max-Age<=0 or an empty value
 * with an expiry.  Session-lifetime only. */

#define COOKIE_CAP 384

typedef struct {
    char *name, *value, *domain, *path;
    int   secure, host_only;
} cookie_t;

static cookie_t jar[COOKIE_CAP];
static int      njar;


static int ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

static char *dupn(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return 0;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int domain_match(const char *host, const char *domain) {
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return ieq_n(host, domain, hl);
    return hl > dl && host[hl - dl - 1] == '.' && ieq_n(host + hl - dl, domain, dl);
}

static void cookie_drop(int i) {
    free(jar[i].name); free(jar[i].value); free(jar[i].domain); free(jar[i].path);
    jar[i] = jar[--njar];
}

static void cookie_store(const url_t *u, const char *v, size_t vlen) {
    const char *end = v + vlen, *semi = memchr(v, ';', vlen), *eq;
    const char *nv_end = semi ? semi : end;
    char domain[HOST_CAP], path[URL_CAP];
    int secure = 0, host_only = 1, remove = 0;
    eq = memchr(v, '=', (size_t)(nv_end - v));
    if (!eq || eq == v) return;
    snprintf(domain, sizeof domain, "%s", u->host);
    {
        /* default path: the request path up to its last slash */
        const char *q = strchr(u->path, '?');
        size_t pl = q ? (size_t)(q - u->path) : strlen(u->path);
        while (pl > 1 && u->path[pl - 1] != '/') pl--;
        if (pl > 1) pl--;
        snprintf(path, sizeof path, "%.*s", (int)(pl ? pl : 1), pl ? u->path : "/");
    }
    for (const char *a = semi; a && a < end;) {
        const char *ae, *ak, *av;
        size_t kl, vl;
        a++;
        while (a < end && *a == ' ') a++;
        ae = memchr(a, ';', (size_t)(end - a));
        if (!ae) ae = end;
        ak = a;
        av = memchr(a, '=', (size_t)(ae - a));
        kl = (size_t)((av ? av : ae) - ak);
        while (kl && ak[kl - 1] == ' ') kl--;
        if (av) { av++; while (av < ae && *av == ' ') av++; }
        vl = av ? (size_t)(ae - av) : 0;
        while (vl && av[vl - 1] == ' ') vl--;
        if (kl == 6 && ieq_n(ak, "domain", 6) && vl) {
            const char *d = av;
            if (*d == '.') { d++; vl--; }
            if (vl < sizeof domain) {
                snprintf(domain, sizeof domain, "%.*s", (int)vl, d);
                if (!domain_match(u->host, domain)) return;   /* not ours to set */
                host_only = 0;
            }
        } else if (kl == 4 && ieq_n(ak, "path", 4) && vl && av[0] == '/') {
            snprintf(path, sizeof path, "%.*s", (int)vl, av);
        } else if (kl == 6 && ieq_n(ak, "secure", 6)) {
            secure = 1;
        } else if (kl == 7 && ieq_n(ak, "max-age", 7) && vl) {
            if (atol(av) <= 0) remove = 1;
        } else if (kl == 7 && ieq_n(ak, "expires", 7) && vl >= 4) {
            /* "Thu, 01 Jan 1970 ..." and other past years: treat as delete */
            const char *y = av;
            for (size_t i = 0; i + 4 <= vl; i++) {
                if (av[i] >= '0' && av[i] <= '9' && av[i + 1] >= '0' && av[i + 1] <= '9' &&
                    av[i + 2] >= '0' && av[i + 2] <= '9' && av[i + 3] >= '0' && av[i + 3] <= '9') { y = av + i; break; }
            }
            if (y != av && atoi(y) < 2000) remove = 1;
        }
        a = ae;
    }
    for (int i = 0; i < njar; i++) {
        if (strlen(jar[i].name) == (size_t)(eq - v) && memcmp(jar[i].name, v, (size_t)(eq - v)) == 0 &&
            strcmp(jar[i].domain, domain) == 0 && strcmp(jar[i].path, path) == 0) {
            cookie_drop(i);
            break;
        }
    }
    if (remove) return;
    if (njar >= COOKIE_CAP) cookie_drop(0);
    jar[njar].name = dupn(v, (size_t)(eq - v));
    jar[njar].value = dupn(eq + 1, (size_t)(nv_end - eq - 1));
    jar[njar].domain = dupn(domain, strlen(domain));
    jar[njar].path = dupn(path, strlen(path));
    jar[njar].secure = secure;
    jar[njar].host_only = host_only;
    if (!jar[njar].name || !jar[njar].value || !jar[njar].domain || !jar[njar].path) return;
    njar++;
}

/* "Cookie: a=b; c=d\r\n" for url, or "" (malloc'd). */
static char *cookie_header(const url_t *u) {
    size_t cap = 16, len = 0;
    char *out;
    for (int i = 0; i < njar; i++) cap += strlen(jar[i].name) + strlen(jar[i].value) + 3;
    out = (char *)malloc(cap + 16);
    if (!out) return 0;
    out[0] = 0;
    for (int i = 0; i < njar; i++) {
        cookie_t *c = &jar[i];
        size_t pl = strlen(c->path);
        if (c->secure && !u->tls) continue;
        if (c->host_only ? !(strlen(u->host) == strlen(c->domain) && ieq_n(u->host, c->domain, strlen(c->domain)))
                         : !domain_match(u->host, c->domain)) continue;
        if (strncmp(u->path, c->path, pl) != 0) continue;
        if (pl > 1 && c->path[pl - 1] != '/' && u->path[pl] && u->path[pl] != '/' && u->path[pl] != '?') continue;
        len += (size_t)sprintf(out + len, "%s%s=%s", len ? "; " : "Cookie: ", c->name, c->value);
    }
    if (len) sprintf(out + len, "\r\n");
    return out;
}

http_req_t *http_open(const char *url, const char *method, const char *extra_headers) {
    return http_open_body(url, method, extra_headers, 0, 0);
}

http_req_t *http_open_body(const char *url, const char *method, const char *extra_headers,
                           const void *body, size_t body_len) {
    http_req_t *r = (http_req_t *)calloc(1, sizeof(http_req_t));
    uint32_t ip;
    char *req, *cookies;
    size_t req_cap;
    int n;
    if (!r) return 0;
    r->content_length = -1;
    if (url_parse(url, &r->url) != 0) {
        fail(r, "That address is not a valid URL");
        return r;
    }
    if (net_resolve(r->url.host, &ip) != 0) {
        fail(r, "The host name did not resolve");
        return r;
    }
    if (r->url.tls) {
        tls_conn_t *t = 0;
        int rc = tls_connect(&t, ip, r->url.port, r->url.host);
        if (rc != 0) {
            if (rc == -4) fail(r, "The connection was refused");
            else if (rc == -3) fail(r, "The connection timed out");
            else {
                const char *why = tls_last_error();
                if (why && *why) snprintf(r->error, sizeof(r->error), "Not secure: %s", why);
                else snprintf(r->error, sizeof(r->error), "The secure connection failed (%d)", rc);
                r->state = HTTP_ERROR;
            }
            return r;
        }
        r->tls = t;
        r->sock = tls_socket(t);
    } else {
        r->sock = net_connect(ip, r->url.port, 1000);
        if (r->sock < 0) {
            fail(r, r->sock == -111 ? "The connection was refused" : "The connection timed out");
            return r;
        }
    }
    cookies = cookie_header(&r->url);
    req_cap = URL_CAP + 1024 + (cookies ? strlen(cookies) : 0) + (extra_headers ? strlen(extra_headers) : 0) + body_len;
    req = (char *)malloc(req_cap);
    if (!req) {
        free(cookies);
        fail(r, "Out of memory");
        return r;
    }
    n = snprintf(req, req_cap,
                 "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
                 "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
                 "Accept-Language: en-US,en;q=0.8\r\nAccept-Encoding: identity\r\n"
                 "Connection: close\r\n%s%s",
                 method ? method : "GET", r->url.path, r->url.host, cookies ? cookies : "",
                 extra_headers ? extra_headers : "");
    free(cookies);
    if (body) n += snprintf(req + n, req_cap - (size_t)n, "Content-Length: %u\r\n", (unsigned)body_len);
    n += snprintf(req + n, req_cap - (size_t)n, "\r\n");
    if (body && body_len && (size_t)n + body_len < req_cap) {
        memcpy(req + n, body, body_len);
        n += (int)body_len;
    }
    if (conn_send(r, req, (size_t)n) != 0) fail(r, "The request could not be sent");
    free(req);
    return r;
}

static void parse_headers(http_req_t *r, const char *head, size_t len) {
    const char *p = head, *end = head + len;
    const char *line_end = memchr(p, '\n', len);
    if (!line_end) return;
    {
        const char *sp = memchr(p, ' ', (size_t)(line_end - p));
        if (sp) r->status = atoi(sp + 1);
    }
    p = line_end + 1;
    while (p < end) {
        const char *colon, *v;
        size_t vlen;
        line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            v = colon + 1;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            vlen = (size_t)(line_end - v);
            while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;
            if (ieq_prefix(p, "content-length:")) r->content_length = atol(v);
            else if (ieq_prefix(p, "transfer-encoding:") && vlen >= 7 && ieq_prefix(v + vlen - 7, "chunked")) r->chunked = 1;
            else if (ieq_prefix(p, "content-type:")) copy_cap(r->content_type, v, vlen, sizeof(r->content_type));
            else if (ieq_prefix(p, "location:")) copy_cap(r->location, v, vlen, sizeof(r->location));
            else if (ieq_prefix(p, "set-cookie:")) cookie_store(&r->url, v, vlen);
        }
        p = line_end + 1;
    }
}

/* Moves decodable bytes from r->in into r->body; returns 1 when complete. */
static int decode_body(http_req_t *r) {
    size_t pos = 0;
    if (!r->chunked) {
        if (r->in_len) {
            if (append(&r->body, &r->body_len, &r->body_cap, r->in, r->in_len) != 0) return -1;
            r->in_len = 0;
        }
        return r->content_length >= 0 && (int64_t)r->body_len >= r->content_length;
    }
    for (;;) {
        if (r->chunk_phase == 0) {
            uint8_t *nl = memchr(r->in + pos, '\n', r->in_len - pos);
            int64_t size = 0;
            if (!nl) break;
            for (uint8_t *c = r->in + pos; c < nl; c++) {
                int d = (*c >= '0' && *c <= '9') ? *c - '0' : (lower(*c) >= 'a' && lower(*c) <= 'f') ? lower(*c) - 'a' + 10 : -1;
                if (d < 0) break;
                size = size * 16 + d;
            }
            pos = (size_t)(nl - r->in) + 1;
            if (size == 0) {
                r->chunk_phase = 3;
                break;
            }
            r->chunk_left = size;
            r->chunk_phase = 1;
        } else if (r->chunk_phase == 1) {
            size_t avail = r->in_len - pos;
            size_t take = avail < (size_t)r->chunk_left ? avail : (size_t)r->chunk_left;
            if (!take) break;
            if (append(&r->body, &r->body_len, &r->body_cap, r->in + pos, take) != 0) return -1;
            pos += take;
            r->chunk_left -= (int64_t)take;
            if (r->chunk_left == 0) r->chunk_phase = 2;
        } else if (r->chunk_phase == 2) {
            uint8_t *nl = memchr(r->in + pos, '\n', r->in_len - pos);
            if (!nl) break;
            pos = (size_t)(nl - r->in) + 1;
            r->chunk_phase = 0;
        } else {
            break;
        }
    }
    memmove(r->in, r->in + pos, r->in_len - pos);
    r->in_len -= pos;
    return r->chunk_phase == 3;
}

int http_poll(http_req_t *r, int timeout_ms) {
    uint8_t buf[16384];
    if (r->state != HTTP_PENDING) return r->state;
    for (int round = 0; round < 64; round++) {
        long n = conn_recv(r, buf, sizeof(buf), round == 0 ? timeout_ms : 0);
        if (n == NET_EAGAIN || n == TLS_WOULD_BLOCK) return HTTP_PENDING;
        if (n < 0) {
            if (r->headers_done && r->content_length < 0 && !r->chunked) {
                r->state = HTTP_DONE;
                return HTTP_DONE;
            }
            fail(r, "The connection dropped");
            return HTTP_ERROR;
        }
        if (n == 0) {
            if (!r->headers_done) {
                fail(r, "The server closed the connection without a reply");
                return HTTP_ERROR;
            }
            r->state = HTTP_DONE;
            return HTTP_DONE;
        }
        if (append(&r->in, &r->in_len, &r->in_cap, buf, (size_t)n) != 0) {
            fail(r, "Out of memory");
            return HTTP_ERROR;
        }
        if (!r->headers_done) {
            uint8_t *end = 0;
            for (size_t i = 3; i < r->in_len; i++) {
                if (r->in[i] == '\n' && r->in[i - 1] == '\r' && r->in[i - 2] == '\n' && r->in[i - 3] == '\r') {
                    end = r->in + i + 1;
                    break;
                }
            }
            if (!end) continue;
            parse_headers(r, (const char *)r->in, (size_t)(end - r->in));
            r->headers_done = 1;
            memmove(r->in, end, r->in_len - (size_t)(end - r->in));
            r->in_len -= (size_t)(end - r->in);
            if (r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200)) {
                r->state = HTTP_DONE;
                return HTTP_DONE;
            }
        }
        {
            int rc = decode_body(r);
            if (rc < 0) {
                fail(r, "Out of memory");
                return HTTP_ERROR;
            }
            if (rc == 1) {
                r->state = HTTP_DONE;
                return HTTP_DONE;
            }
        }
    }
    return HTTP_PENDING;
}

int http_socket(http_req_t *r) {
    return r ? r->sock : -1;
}

void http_free(http_req_t *r) {
    if (!r) return;
    if (r->tls) tls_close((tls_conn_t *)r->tls);
    else if (r->sock > 0) net_close(r->sock);
    free(r->in);
    free(r->body);
    free(r);
}

http_req_t *http_request(const char *method, const char *url, const char *content_type,
                         const void *body, size_t body_len, char *final_url, size_t final_cap) {
    char current[URL_CAP], headers[160];
    int post = method && strcmp(method, "GET") != 0;
    headers[0] = 0;
    if (post && content_type) snprintf(headers, sizeof headers, "Content-Type: %s\r\n", content_type);
    snprintf(current, sizeof(current), "%s", url);
    for (int hop = 0, retried = 0; hop < 8; hop++) {
        http_req_t *r = post ? http_open_body(current, method, headers, body ? body : "", body_len)
                             : http_open(current, "GET", 0);
        uint64_t deadline = icda_ticks() + 3000;
        if (!r) return 0;
        while (r->state == HTTP_PENDING && icda_ticks() < deadline) http_poll(r, 200);
        if (r->state == HTTP_PENDING) fail(r, "The page took too long to load");
        if (r->state == HTTP_ERROR && !r->headers_done && !retried && !post) {
            /* one retry: flaky paths often succeed on a fresh connection */
            retried = 1;
            hop--;
            http_free(r);
            continue;
        }
        if (r->state == HTTP_DONE && r->status >= 300 && r->status < 400 && r->location[0]) {
            char next[URL_CAP];
            if (url_resolve(current, r->location, next, sizeof(next)) == 0) {
                /* 301/302/303 turn a POST into a GET; 307/308 repeat it */
                if (r->status != 307 && r->status != 308) post = 0;
                http_free(r);
                snprintf(current, sizeof(current), "%s", next);
                continue;
            }
        }
        {
            url_t u;
            if (url_parse(current, &u) == 0) url_format(&u, final_url, final_cap);
        }
        return r;
    }
    return 0;
}

http_req_t *http_get(const char *url, char *final_url, size_t final_cap) {
    return http_request("GET", url, 0, 0, 0, final_url, final_cap);
}
