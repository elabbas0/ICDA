#ifndef SURFER_HTTP_H
#define SURFER_HTTP_H

#include <stdint.h>
#include <stddef.h>

/* HTTP/1.1 client over plain TCP or TLS.  A request is a small state machine:
 * http_open() connects and sends, http_poll() consumes whatever arrived, so
 * a caller can keep its UI responsive between polls. */

#define URL_CAP   2048
#define HOST_CAP  256

typedef struct {
    int      tls;
    char     host[HOST_CAP];
    uint16_t port;
    char     path[URL_CAP];      /* path + query, never empty */
} url_t;

int  url_parse(const char *text, url_t *out);
/* Resolves ref against base (absolute URLs, //host, /path, relative, ?q, #f). */
int  url_resolve(const char *base, const char *ref, char *out, size_t cap);
void url_format(const url_t *u, char *out, size_t cap);

enum { HTTP_PENDING = 0, HTTP_DONE = 1, HTTP_ERROR = -1 };

typedef struct http_req http_req_t;

struct http_req {
    url_t    url;
    int      sock;
    void    *tls;
    int      state;
    int      status;
    char     error[96];
    char     content_type[96];
    char     location[URL_CAP];
    int64_t  content_length;
    int      chunked;
    /* raw bytes not yet parsed (headers or chunk framing) */
    uint8_t *in;
    size_t   in_len, in_cap;
    int      headers_done;
    int64_t  chunk_left;
    int      chunk_phase;
    /* decoded body */
    uint8_t *body;
    size_t   body_len, body_cap;
};

http_req_t *http_open(const char *url, const char *method, const char *extra_headers);
http_req_t *http_open_body(const char *url, const char *method, const char *extra_headers,
                           const void *body, size_t body_len);
int         http_poll(http_req_t *r, int timeout_ms);
int         http_socket(http_req_t *r);
void        http_free(http_req_t *r);

/* Blocking fetch that follows up to 8 redirects; final_url gets the last URL. */
http_req_t *http_get(const char *url, char *final_url, size_t final_cap);
/* Blocking request with redirects; POST bodies use content_type. */
http_req_t *http_request(const char *method, const char *url, const char *content_type,
                         const void *body, size_t body_len, char *final_url, size_t final_cap);

#endif
