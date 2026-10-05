#ifndef SURFER_TLS_H
#define SURFER_TLS_H

#include <stdint.h>

#define TLS_CAP 16384
#define TLS_RECORD_CAP (2 * TLS_CAP + 1024)
#define TLS_WOULD_BLOCK (-11)

typedef struct tls_conn tls_conn_t;

int  tls_connect(tls_conn_t **conn, uint32_t ip, uint16_t port, const char *server_name);
int  tls_write(tls_conn_t *conn, const uint8_t *data, uint32_t len);
long tls_read(tls_conn_t *conn, uint8_t *buf, uint32_t cap, int timeout_ms);
int  tls_socket(tls_conn_t *conn);
void tls_close(tls_conn_t *conn);
const char *tls_last_error(void);
extern int tls_debug;

#endif
