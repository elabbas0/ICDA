#ifndef TLS_H
#define TLS_H

#include <stdint.h>

#define TLS_CAP 16384
#define TLS_RECORD_CAP (TLS_CAP + 256)

typedef struct tls_conn tls_conn_t;

int tls_connect(tls_conn_t **conn, uint32_t ip, uint16_t port, const char *server_name);
int tls_write(tls_conn_t *conn, const uint8_t *data, uint32_t len);
int tls_read(tls_conn_t *conn, uint8_t *buf, uint32_t cap, uint32_t *out_len);
void tls_close(tls_conn_t *conn);

#endif
