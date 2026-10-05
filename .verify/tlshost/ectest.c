#include <stdio.h>
#include <string.h>
#include "ecdsa.h"
#include "sha512.h"
#include "sha256.h"
static size_t rd(const char *p, unsigned char *b, size_t cap) { FILE *f = fopen(p, "rb"); size_t n = fread(b, 1, cap, f); fclose(f); return n; }
int main(int argc, char **argv) {
    unsigned char pub[256], sig[256], msg[64], h[64];
    int curve = argv[1][0] == '2' ? EC_P256 : EC_P384;
    size_t pl = rd(argv[2], pub, sizeof pub), sl = rd(argv[3], sig, sizeof sig), ml = rd(argv[4], msg, sizeof msg);
    size_t keylen = curve == EC_P256 ? 65 : 97, hl;
    if (curve == EC_P256) { sha256_hash(msg, ml, h); hl = 32; } else { sha384_hash(msg, ml, h); hl = 48; }
    printf("%s valid: %s\n", argv[1], ecdsa_verify(curve, pub + pl - keylen, keylen, h, hl, sig, sl) == 0 ? "OK" : "FAIL");
    h[0] ^= 1;
    printf("%s tampered rejected: %s\n", argv[1], ecdsa_verify(curve, pub + pl - keylen, keylen, h, hl, sig, sl) != 0 ? "OK" : "FAIL");
    return 0;
}
