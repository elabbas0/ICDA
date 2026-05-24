#include "rsa.h"
#include "../proc/sched.h"
static int der_read_tag(const uint8_t *der, int der_len, int *pos, uint8_t *tag, int *len) {
    if (*pos >= der_len) return -1;
    *tag = der[(*pos)++];
    if (*pos >= der_len) return -1;
    int l = der[(*pos)++];
    if (l & 0x80) {
        int num_bytes = l & 0x7F;
        if (num_bytes > 4) return -1;
        l = 0;
        for (int i = 0; i < num_bytes; i++) {
            if (*pos >= der_len) return -1;
            l = (l << 8) | der[(*pos)++];
        }
    }
    *len = l;
    return 0;
}

static int der_skip_constructed(const uint8_t *der, int der_len, int *pos) {
    uint8_t tag;
    int len;
    while (*pos < der_len) {
        if (der_read_tag(der, der_len, pos, &tag, &len) != 0) return -1;
        if ((tag & 0x20) && len > 0) {
            int end = *pos + len;
            while (*pos < end) {
                if (der_skip_constructed(der, der_len, pos) != 0) return -1;
            }
        } else {
            *pos += len;
        }
    }
    return 0;
}

static int der_find_in_sequence(const uint8_t *der, int der_len, int *pos, uint8_t target_tag, const uint8_t **out_data, int *out_len) {
    uint8_t tag;
    int len;
    int start = *pos;
    if (der_read_tag(der, der_len, pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    int end = *pos + len;
    while (*pos < end) {
        if (*pos >= der_len) return -1;
        if (der[*pos] == target_tag) {
            int saved = *pos;
            uint8_t t;
            int l;
            if (der_read_tag(der, der_len, pos, &t, &l) != 0) return -1;
            if (t == target_tag) {
                *out_data = der + *pos;
                *out_len = l;
                *pos = end;
                return 0;
            }
            *pos = saved;
        }
        uint8_t t;
        int l;
        if (der_read_tag(der, der_len, pos, &t, &l) != 0) return -1;
        *pos += l;
    }
    return -1;
}

int rsa_pubkey_from_cert_der(const uint8_t *der, int der_len, rsa_pubkey_t *key) {
    int pos = 0;
    uint8_t tag;
    int len;

    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    int cert_end = pos + len;

    if (pos >= cert_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    int tbs_end = pos + len;

    if (pos < tbs_end && der[pos] == 0xA0) {
        if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
        pos += len;
    }

    if (pos >= tbs_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x02) return -1;
    pos += len;

    if (pos >= tbs_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    pos += len;

    if (pos >= tbs_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    pos += len;

    if (pos >= tbs_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    pos += len;

    if (pos >= tbs_end) return -1;
    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    pos += len;

    while (pos < tbs_end) {
        uint8_t next = der[pos];
        if (next == 0xA1 || next == 0xA2 || next == 0xA3) {
            if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
            pos += len;
        } else {
            break;
        }
    }

    if (pos > tbs_end) return -1;
    if (pos >= tbs_end) return -1;
    return rsa_pubkey_from_der(der + pos, tbs_end - pos, key);
}

int rsa_pubkey_from_der(const uint8_t *der, int der_len, rsa_pubkey_t *key) {
    int pos = 0;
    uint8_t tag;
    int len;

    if (der_read_tag(der, der_len, &pos, &tag, &len) != 0) return -1;
    if (tag != 0x30) return -1;
    int seq_end = pos + len;

    if (pos + 2 > seq_end) return -1;
    if (der[pos] == 0x30) {
        int alg_id_end;
        uint8_t alg_tag;
        int alg_len;
        if (der_read_tag(der, der_len, &pos, &alg_tag, &alg_len) != 0) return -1;
        pos += alg_len;
    } else if (der[pos] == 0x02) {
    } else {
        return -1;
    }

    if (pos >= seq_end) return -1;
    uint8_t bs_tag;
    int bs_len;
    if (der_read_tag(der, der_len, &pos, &bs_tag, &bs_len) != 0) return -1;
    if (bs_tag != 0x03) return -1;
    if (pos >= seq_end) return -1;
    int unused_bits = der[pos++];
    (void)unused_bits;
    int bitstring_start = pos;
    int inner_pos = 0;
    uint8_t inner_tag;
    int inner_len;
    if (der_read_tag(der + bitstring_start, bs_len - 1, &inner_pos, &inner_tag, &inner_len) != 0) return -1;
    if (inner_tag != 0x30) return -1;
    int rsa_seq_start = bitstring_start + inner_pos;
    int rsa_seq_end = rsa_seq_start + inner_len;

    int rpos = rsa_seq_start;
    uint8_t rtag;
    int rlen;
    if (der_read_tag(der, der_len, &rpos, &rtag, &rlen) != 0) return -1;
    if (rtag != 0x02) return -1;
    int n_start = rpos;
    int n_len = rlen;
    rpos += rlen;

    if (der_read_tag(der, der_len, &rpos, &rtag, &rlen) != 0) return -1;
    if (rtag != 0x02) return -1;
    int e_start = rpos;
    int e_len = rlen;

    while (n_len > 1 && der[n_start] == 0x00) {
        n_start++;
        n_len--;
    }
    while (e_len > 1 && der[e_start] == 0x00) {
        e_start++;
        e_len--;
    }

    if (bn_from_bytes(&key->n, der + n_start, n_len) != 0) return -1;
    if (bn_from_bytes(&key->e, der + e_start, e_len) != 0) return -1;

    return 0;
}

static int pkcs1_v15_pad(const uint8_t *in, int in_len, uint8_t *out, int out_len, int type) {
    if (in_len + 11 > out_len) return -1;
    out[0] = 0x00;
    out[1] = (uint8_t)type;
    int pad_len = out_len - in_len - 3;
    for (int i = 0; i < pad_len; i++) {
        if (type == 0x01) {
            out[2 + i] = 0xFF;
        } else {
            uint8_t v = 0;
            uint32_t seed = 0xA5A50000U ^ (uint32_t)(sched_ticks() + i * 73U);
            while (v == 0) {
                seed = seed * 1664525U + 1013904223U;
                v = (uint8_t)((seed >> 16) & 0xFFU);
            }
            out[2 + i] = v;
        }
    }
    out[2 + pad_len] = 0x00;
    for (int i = 0; i < in_len; i++) out[2 + pad_len + 1 + i] = in[i];
    return 0;
}

int rsa_pkcs1_v15_encode(const uint8_t *in, int in_len, uint8_t *out, int out_len) {
    return pkcs1_v15_pad(in, in_len, out, out_len, 0x02);
}

int rsa_encrypt(const rsa_pubkey_t *key, const uint8_t *in, int in_len, uint8_t *out, int *out_len) {
    uint8_t padded[512];
    uint8_t tmp_out[512];
    int mod_bytes = 0;
    bn_to_bytes(&key->n, padded, &mod_bytes);

    if (in_len + 11 > mod_bytes) return -1;
    if (rsa_pkcs1_v15_encode(in, in_len, padded, mod_bytes) != 0) return -1;

    bn_t m, c;
    bn_from_bytes(&m, padded, mod_bytes);
    bn_mod_exp(&c, &m, &key->e, &key->n);
    bn_to_bytes(&c, tmp_out, out_len);
    if (*out_len > mod_bytes) return -1;
    for (int i = 0; i < mod_bytes - *out_len; i++) out[i] = 0;
    for (int i = 0; i < *out_len; i++) out[mod_bytes - *out_len + i] = tmp_out[i];
    *out_len = mod_bytes;
    return 0;
}
