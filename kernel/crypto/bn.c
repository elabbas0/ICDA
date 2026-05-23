#include "bn.h"

void bn_zero(bn_t *a) {
    for (int i = 0; i < BN_MAX_WORDS; i++) a->d[i] = 0;
    a->len = 0;
}

int bn_from_bytes(bn_t *a, const uint8_t *bytes, int len) {
    bn_zero(a);
    if (len > BN_MAX_WORDS * 4) return -1;
    int idx = 0;
    for (int i = len - 4; i >= 0; i -= 4) {
        a->d[idx++] = ((uint32_t)bytes[i] << 24) | ((uint32_t)bytes[i+1] << 16) |
                      ((uint32_t)bytes[i+2] << 8) | (uint32_t)bytes[i+3];
    }
    int rem = len % 4;
    if (rem) {
        uint32_t v = 0;
        for (int j = 0; j < rem; j++) v = (v << 8) | bytes[j];
        a->d[idx++] = v;
    }
    a->len = idx;
    while (a->len > 0 && a->d[a->len - 1] == 0) a->len--;
    if (a->len == 0) a->len = 1;
    return 0;
}

void bn_from_uint32(bn_t *a, uint32_t v) {
    bn_zero(a);
    a->d[0] = v;
    a->len = 1;
}

int bn_is_zero(const bn_t *a) {
    for (int i = 0; i < a->len; i++) if (a->d[i] != 0) return 0;
    return 1;
}

int bn_cmp(const bn_t *a, const bn_t *b) {
    if (a->len != b->len) return a->len - b->len;
    for (int i = a->len - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) return a->d[i] > b->d[i] ? 1 : -1;
    }
    return 0;
}

void bn_add(bn_t *r, const bn_t *a, const bn_t *b) {
    bn_zero(r);
    uint64_t carry = 0;
    int max_len = a->len > b->len ? a->len : b->len;
    for (int i = 0; i < max_len || carry; i++) {
        uint64_t sum = carry;
        if (i < a->len) sum += a->d[i];
        if (i < b->len) sum += b->d[i];
        r->d[i] = (uint32_t)sum;
        carry = sum >> 32;
        r->len = i + 1;
    }
}

void bn_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    bn_zero(r);
    uint64_t borrow = 0;
    int max_len = a->len > b->len ? a->len : b->len;
    for (int i = 0; i < max_len; i++) {
        uint64_t va = i < a->len ? a->d[i] : 0;
        uint64_t vb = i < b->len ? b->d[i] : 0;
        uint64_t diff = va - vb - borrow;
        if (va < vb + borrow) diff += (1ULL << 32);
        r->d[i] = (uint32_t)(diff & 0xFFFFFFFFULL);
        borrow = (va < vb + borrow) ? 1 : 0;
        r->len = i + 1;
    }
    while (r->len > 0 && r->d[r->len - 1] == 0) r->len--;
    if (r->len == 0) r->len = 1;
}

void bn_mul(bn_t *r, const bn_t *a, const bn_t *b) {
    bn_zero(r);
    if (bn_is_zero(a) || bn_is_zero(b)) return;
    for (int i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->len || carry; j++) {
            if (i + j >= BN_MAX_WORDS) return;
            uint64_t sum = r->d[i + j] + carry;
            if (j < b->len) sum += (uint64_t)a->d[i] * b->d[j];
            r->d[i + j] = (uint32_t)(sum & 0xFFFFFFFFULL);
            carry = sum >> 32;
            if (i + j + 1 > r->len) r->len = i + j + 1;
        }
    }
    while (r->len > 0 && r->d[r->len - 1] == 0) r->len--;
    if (r->len == 0) r->len = 1;
}

static int bn_sub_mod_internal(uint64_t *a, const uint64_t *b, int words) {
    uint64_t borrow = 0;
    for (int i = 0; i < words; i++) {
        uint64_t diff = a[i] - b[i] - borrow;
        a[i] = diff & 0xFFFFFFFFULL;
        borrow = (b[i] + borrow > a[i] + (borrow ? 0 : 0)) ? 1 : (diff >> 63);
        if (i == 0) {
            borrow = (b[0] > a[0]) ? 1 : 0;
            a[0] = diff;
        } else {
            borrow = diff >> 63;
        }
    }
    return (int)borrow;
}

void bn_mod(bn_t *r, const bn_t *a, const bn_t *m) {
    bn_t tmp;
    bn_zero(&tmp);
    if (bn_is_zero(m) || bn_cmp(a, m) < 0) {
        for (int i = 0; i < a->len; i++) r->d[i] = a->d[i];
        r->len = a->len;
        if (r->len == 0) { r->len = 1; r->d[0] = 0; }
        return;
    }
    for (int i = 0; i < a->len; i++) tmp.d[i] = a->d[i];
    tmp.len = a->len;

    bn_t divisor;
    bn_zero(&divisor);
    for (int i = 0; i < m->len; i++) divisor.d[i] = m->d[i];
    divisor.len = m->len;

    while (bn_cmp(&tmp, &divisor) >= 0) {
        bn_t shifted;
        bn_zero(&shifted);
        int shift = tmp.len - divisor.len;
        if (shift > 0) {
            for (int i = 0; i < divisor.len; i++) shifted.d[i + shift] = divisor.d[i];
            shifted.len = divisor.len + shift;
            if (bn_cmp(&tmp, &shifted) < 0 && shift > 0) {
                bn_zero(&shifted);
                shift--;
                for (int i = 0; i < divisor.len; i++) shifted.d[i + shift] = divisor.d[i];
                shifted.len = divisor.len + shift;
            }
        } else {
            for (int i = 0; i < divisor.len; i++) shifted.d[i] = divisor.d[i];
            shifted.len = divisor.len;
        }
        bn_t result;
        bn_zero(&result);
        bn_sub(&result, &tmp, &shifted);
        for (int i = 0; i < result.len; i++) tmp.d[i] = result.d[i];
        tmp.len = result.len;
    }

    for (int i = 0; i < tmp.len; i++) r->d[i] = tmp.d[i];
    r->len = tmp.len;
    if (r->len == 0) { r->len = 1; r->d[0] = 0; }
}

void bn_mod_exp(bn_t *r, const bn_t *a, const bn_t *e, const bn_t *m) {
    bn_t result;
    bn_from_uint32(&result, 1);
    bn_t base;
    for (int i = 0; i < a->len; i++) base.d[i] = a->d[i];
    base.len = a->len;
    bn_t exp;
    for (int i = 0; i < e->len; i++) exp.d[i] = e->d[i];
    exp.len = e->len;

    while (!bn_is_zero(&exp)) {
        if (exp.d[0] & 1) {
            bn_t tmp;
            bn_mul(&tmp, &result, &base);
            bn_mod(&result, &tmp, m);
        }
        bn_t sq;
        bn_mul(&sq, &base, &base);
        bn_mod(&base, &sq, m);
        uint64_t borrow = 1;
        uint32_t *ed = exp.d;
        int ewords = exp.len;
        for (int i = 0; i < ewords && borrow; i++) {
            uint64_t v = (uint64_t)ed[i] - borrow;
            ed[i] = (uint32_t)(v & 0xFFFFFFFFULL);
            borrow = v >> 63;
        }
        while (exp.len > 0 && exp.d[exp.len - 1] == 0) exp.len--;
        if (exp.len == 0) exp.len = 1;
    }
    for (int i = 0; i < result.len; i++) r->d[i] = result.d[i];
    r->len = result.len;
}

void bn_sub_mod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m) {
    bn_t tmp;
    if (bn_cmp(a, b) >= 0) {
        bn_sub(&tmp, a, b);
    } else {
        bn_sub(&tmp, b, a);
        bn_sub(&tmp, m, &tmp);
    }
    for (int i = 0; i < tmp.len; i++) r->d[i] = tmp.d[i];
    r->len = tmp.len;
}

void bn_to_bytes(const bn_t *a, uint8_t *bytes, int *len) {
    int idx = 0;
    for (int i = a->len - 1; i >= 0; i--) {
        bytes[idx++] = (uint8_t)(a->d[i] >> 24);
        bytes[idx++] = (uint8_t)((a->d[i] >> 16) & 0xFF);
        bytes[idx++] = (uint8_t)((a->d[i] >> 8) & 0xFF);
        bytes[idx++] = (uint8_t)(a->d[i] & 0xFF);
    }
    *len = idx;
    while (*len > 0 && bytes[0] == 0) {
        for (int i = 0; i < *len - 1; i++) bytes[i] = bytes[i+1];
        (*len)--;
    }
}
