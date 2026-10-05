#ifndef SURFER_X509_H
#define SURFER_X509_H

#include <stdint.h>
#include <stddef.h>

enum { HASH_NONE = 0, HASH_SHA1, HASH_SHA256, HASH_SHA384, HASH_SHA512 };
enum { KEY_NONE = 0, KEY_RSA, KEY_EC_P256, KEY_EC_P384 };

typedef struct {
    const uint8_t *p;
    size_t len;
} span_t;

typedef struct {
    span_t   raw;           /* whole certificate */
    span_t   tbs;           /* signed part (full TLV) */
    span_t   issuer, subject;
    int      sig_hash;      /* HASH_* */
    int      sig_rsa;       /* 1 RSA PKCS#1 v1.5, 0 ECDSA */
    span_t   sig;
    int64_t  not_before, not_after;
    int      key_type;
    span_t   rsa_n, rsa_e;
    span_t   ec_point;
    int      is_ca;
    span_t   san;           /* SubjectAltName GeneralNames content */
} cert_t;

int  x509_parse(const uint8_t *der, size_t len, cert_t *out);
/* Verifies sig over hash with the key of signer. */
int  x509_verify_sig(const cert_t *signer, int hash, int rsa_pss, const uint8_t *data, size_t len,
                     const uint8_t *sig, size_t sig_len);
/* Verifies certs[0] for host, building a chain through certs[1..n-1] to a
 * root in the CA store.  Returns 0 on success; *why gets a reason. */
int  x509_verify_chain(const cert_t *certs, int n, const char *host, int64_t now, const char **why);
int  x509_load_roots(const char *pem_path);
void hash_data(int hash, const uint8_t *data, size_t len, uint8_t *out, size_t *out_len);

#endif
