#include "form.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "http.h"

static int ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int form_kind(const dom_node_t *n) {
    const char *type;
    if (!n || n->type != N_ELEMENT) return FK_NONE;
    if (n->tag == T_TEXTAREA) return FK_TEXTAREA;
    if (n->tag == T_SELECT) return FK_SELECT;
    if (n->tag == T_BUTTON) {
        type = dom_attr(n, "type");
        if (ieq(type, "reset")) return FK_RESET;
        if (ieq(type, "button")) return FK_BUTTON;
        return FK_SUBMIT;
    }
    if (n->tag != T_INPUT) return FK_NONE;
    type = dom_attr(n, "type");
    if (!type || !*type) return FK_TEXT;
    if (ieq(type, "password")) return FK_PASSWORD;
    if (ieq(type, "checkbox")) return FK_CHECKBOX;
    if (ieq(type, "radio")) return FK_RADIO;
    if (ieq(type, "submit")) return FK_SUBMIT;
    if (ieq(type, "image")) return FK_IMAGE;
    if (ieq(type, "reset")) return FK_RESET;
    if (ieq(type, "button")) return FK_BUTTON;
    if (ieq(type, "hidden")) return FK_HIDDEN;
    if (ieq(type, "file")) return FK_FILE;
    return FK_TEXT;   /* text, search, email, url, tel, number, date, ... */
}

int form_disabled(const dom_node_t *n) {
    for (const dom_node_t *p = n; p && p->type == N_ELEMENT; p = p->parent) {
        if (dom_attr(p, "disabled") && (p == n || p->tag == T_FIELDSET)) return 1;
    }
    return 0;
}

int form_is_text(const dom_node_t *n) {
    int k = form_kind(n);
    return (k == FK_TEXT || k == FK_PASSWORD || k == FK_TEXTAREA) && !form_disabled(n);
}

int form_is_focusable(const dom_node_t *n) {
    int k = form_kind(n);
    return k != FK_NONE && k != FK_HIDDEN && !form_disabled(n);
}

/* ---- state -------------------------------------------------------------- */

static int ctl_reserve(form_ctl_t *c, size_t need) {
    char *v;
    size_t cap;
    if (need + 1 <= c->cap) return 0;
    cap = c->cap ? c->cap * 2 : 32;
    while (cap < need + 1) cap *= 2;
    v = (char *)realloc(c->value, cap);
    if (!v) return -1;
    c->value = v;
    c->cap = cap;
    return 0;
}

static void ctl_assign(form_ctl_t *c, const char *s, size_t n) {
    if (ctl_reserve(c, n) != 0) return;
    memcpy(c->value, s, n);
    c->value[n] = 0;
    c->len = n;
    c->cursor = c->anchor = n;
}

form_ctl_t *form_ctl(dom_node_t *n) {
    form_ctl_t *c;
    int k;
    if (!n || n->type != N_ELEMENT) return 0;
    if (n->ctl) return n->ctl;
    c = (form_ctl_t *)calloc(1, sizeof(form_ctl_t));
    if (!c) return 0;
    n->ctl = c;
    c->selected = -1;
    k = form_kind(n);
    if (k == FK_TEXTAREA) {
        size_t len = 0;
        for (dom_node_t *t = n->first; t; t = t->next) if (t->type == N_TEXT) len += t->text_len;
        if (ctl_reserve(c, len) == 0) {
            size_t o = 0;
            for (dom_node_t *t = n->first; t; t = t->next) {
                if (t->type != N_TEXT) continue;
                memcpy(c->value + o, t->text, t->text_len);
                o += t->text_len;
            }
            /* the parser keeps a leading newline; HTML drops it */
            if (o && c->value[0] == '\n') { memmove(c->value, c->value + 1, o - 1); o--; }
            c->value[o] = 0;
            c->len = o;
        }
    } else if (k == FK_SELECT) {
        int count = form_option_count(n);
        for (int i = 0; i < count; i++) {
            if (dom_attr(form_option(n, i), "selected")) c->selected = i;
        }
        if (c->selected < 0 && count > 0 && !dom_attr(n, "multiple")) c->selected = 0;
    } else {
        const char *v = dom_attr(n, "value");
        if (v) ctl_assign(c, v, strlen(v));
        if (k == FK_CHECKBOX || k == FK_RADIO) c->checked = dom_attr(n, "checked") != 0;
    }
    if (!c->value && ctl_reserve(c, 0) == 0) c->value[0] = 0;
    c->cursor = c->anchor = c->len;
    return c;
}

const char *form_value(dom_node_t *n) {
    form_ctl_t *c = form_ctl(n);
    if (!c) return "";
    if (form_kind(n) == FK_SELECT) {
        dom_node_t *o = form_option(n, c->selected);
        static char buf[512];
        const char *v;
        if (!o) return "";
        v = dom_attr(o, "value");
        if (v) return v;
        form_option_label(o, buf, sizeof buf);
        return buf;
    }
    return c->value ? c->value : "";
}

/* ---- editing ------------------------------------------------------------ */

static size_t utf8_prev(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t utf8_next_off(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int delete_selection(form_ctl_t *c) {
    size_t a = c->anchor < c->cursor ? c->anchor : c->cursor;
    size_t b = c->anchor < c->cursor ? c->cursor : c->anchor;
    if (a == b) return 0;
    memmove(c->value + a, c->value + b, c->len - b + 1);
    c->len -= b - a;
    c->cursor = c->anchor = a;
    return 1;
}

int form_insert(dom_node_t *n, const char *text, size_t len) {
    form_ctl_t *c = form_ctl(n);
    const char *ml;
    size_t max = (size_t)-1;
    int single = form_kind(n) != FK_TEXTAREA;
    if (!c || !form_is_text(n) || dom_attr(n, "readonly")) return 0;
    delete_selection(c);
    ml = dom_attr(n, "maxlength");
    if (ml && atoi(ml) > 0) max = (size_t)atoi(ml);
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (single && (ch == '\n' || ch == '\r')) continue;
        if (ch == '\r') continue;
        if (c->len >= max) break;
        if (ctl_reserve(c, c->len + 1) != 0) break;
        memmove(c->value + c->cursor + 1, c->value + c->cursor, c->len - c->cursor + 1);
        c->value[c->cursor++] = ch;
        c->len++;
    }
    c->anchor = c->cursor;
    return 1;
}

int form_backspace(dom_node_t *n) {
    form_ctl_t *c = form_ctl(n);
    size_t p;
    if (!c || !form_is_text(n) || dom_attr(n, "readonly")) return 0;
    if (delete_selection(c)) return 1;
    if (c->cursor == 0) return 0;
    p = utf8_prev(c->value, c->cursor);
    memmove(c->value + p, c->value + c->cursor, c->len - c->cursor + 1);
    c->len -= c->cursor - p;
    c->cursor = c->anchor = p;
    return 1;
}

int form_delete(dom_node_t *n) {
    form_ctl_t *c = form_ctl(n);
    size_t q;
    if (!c || !form_is_text(n) || dom_attr(n, "readonly")) return 0;
    if (delete_selection(c)) return 1;
    if (c->cursor >= c->len) return 0;
    q = utf8_next_off(c->value, c->len, c->cursor);
    memmove(c->value + c->cursor, c->value + q, c->len - q + 1);
    c->len -= q - c->cursor;
    c->anchor = c->cursor;
    return 1;
}

void form_move(dom_node_t *n, int delta, int extend) {
    form_ctl_t *c = form_ctl(n);
    if (!c) return;
    if (!extend && c->anchor != c->cursor) {
        size_t a = c->anchor < c->cursor ? c->anchor : c->cursor;
        size_t b = c->anchor < c->cursor ? c->cursor : c->anchor;
        c->cursor = c->anchor = delta < 0 ? a : b;
        return;
    }
    for (; delta < 0; delta++) c->cursor = utf8_prev(c->value, c->cursor);
    for (; delta > 0; delta--) c->cursor = utf8_next_off(c->value, c->len, c->cursor);
    if (!extend) c->anchor = c->cursor;
}

void form_home_end(dom_node_t *n, int end, int extend) {
    form_ctl_t *c = form_ctl(n);
    if (!c) return;
    if (form_kind(n) == FK_TEXTAREA) {
        /* within the current line */
        size_t p = c->cursor;
        if (end) while (p < c->len && c->value[p] != '\n') p++;
        else while (p > 0 && c->value[p - 1] != '\n') p--;
        c->cursor = p;
    } else {
        c->cursor = end ? c->len : 0;
    }
    if (!extend) c->anchor = c->cursor;
}

void form_select_all(dom_node_t *n) {
    form_ctl_t *c = form_ctl(n);
    if (!c) return;
    c->anchor = 0;
    c->cursor = c->len;
}

int form_set_value(dom_node_t *n, const char *value) {
    form_ctl_t *c = form_ctl(n);
    if (!c) return 0;
    if (form_kind(n) == FK_SELECT) {
        int count = form_option_count(n);
        for (int i = 0; i < count; i++) {
            dom_node_t *o = form_option(n, i);
            const char *v = dom_attr(o, "value");
            char label[256];
            if (!v) { form_option_label(o, label, sizeof label); v = label; }
            if (strcmp(v, value ? value : "") == 0) { c->selected = i; return 1; }
        }
        c->selected = -1;
        return 1;
    }
    ctl_assign(c, value ? value : "", value ? strlen(value) : 0);
    return 1;
}

size_t form_selection(dom_node_t *n, char *out, size_t cap) {
    form_ctl_t *c = form_ctl(n);
    size_t a, b, len;
    if (!c || cap == 0) return 0;
    a = c->anchor < c->cursor ? c->anchor : c->cursor;
    b = c->anchor < c->cursor ? c->cursor : c->anchor;
    if (a == b) { a = 0; b = c->len; }
    len = b - a < cap - 1 ? b - a : cap - 1;
    memcpy(out, c->value + a, len);
    out[len] = 0;
    return len;
}

/* ---- choices ------------------------------------------------------------ */

static void walk_options(dom_node_t *n, int *count, int want, dom_node_t **found) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT) continue;
        if (k->tag == T_OPTION) {
            if (*count == want) *found = k;
            (*count)++;
        } else if (k->tag == T_OPTGROUP) {
            walk_options(k, count, want, found);
        }
    }
}

int form_option_count(dom_node_t *select) {
    int count = 0;
    dom_node_t *f = 0;
    if (!select) return 0;
    walk_options(select, &count, -1, &f);
    return count;
}

dom_node_t *form_option(dom_node_t *select, int index) {
    int count = 0;
    dom_node_t *f = 0;
    if (!select || index < 0) return 0;
    walk_options(select, &count, index, &f);
    return f;
}

void form_choose(dom_node_t *select, int index) {
    form_ctl_t *c = form_ctl(select);
    if (c && index >= 0 && index < form_option_count(select)) c->selected = index;
}

size_t form_option_label(dom_node_t *option, char *out, size_t cap) {
    const char *label;
    if (!option || cap == 0) return 0;
    label = dom_attr(option, "label");
    if (label) {
        snprintf(out, cap, "%s", label);
        return strlen(out);
    }
    return dom_text(option, out, cap);
}

static dom_node_t *find_id(dom_node_t *n, const char *id) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        dom_node_t *r;
        if (k->type != N_ELEMENT) continue;
        if (k->id && strcmp(k->id, id) == 0) return k;
        r = find_id(k, id);
        if (r) return r;
    }
    return 0;
}

dom_node_t *form_owner(dom_node_t *doc_root, dom_node_t *n) {
    const char *fid = n ? dom_attr(n, "form") : 0;
    if (fid && doc_root) {
        dom_node_t *f = find_id(doc_root, fid);
        if (f && f->tag == T_FORM) return f;
    }
    for (dom_node_t *p = n ? n->parent : 0; p; p = p->parent) {
        if (p->type == N_ELEMENT && p->tag == T_FORM) return p;
    }
    return 0;
}

static void uncheck_group(dom_node_t *root, dom_node_t *n, dom_node_t *form, const char *name) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT) continue;
        if (form_kind(k) == FK_RADIO) {
            const char *kn = dom_attr(k, "name");
            if (kn && strcmp(kn, name) == 0 && form_owner(root, k) == form) form_ctl(k)->checked = 0;
        }
        uncheck_group(root, k, form, name);
    }
}

void form_toggle(dom_node_t *doc_root, dom_node_t *n) {
    form_ctl_t *c = form_ctl(n);
    int k = form_kind(n);
    if (!c || form_disabled(n)) return;
    if (k == FK_CHECKBOX) {
        c->checked = !c->checked;
    } else if (k == FK_RADIO) {
        const char *name = dom_attr(n, "name");
        if (name && *name) uncheck_group(doc_root, doc_root, form_owner(doc_root, n), name);
        c->checked = 1;
    }
}

/* ---- submission --------------------------------------------------------- */

typedef struct {
    char  *s;
    size_t len, cap;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char *ns;
        while (cap < b->len + n + 1) cap *= 2;
        ns = (char *)realloc(b->s, cap);
        if (!ns) return;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

/* application/x-www-form-urlencoded, newlines normalized to CRLF */
static void sb_enc(sbuf_t *b, const char *s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        char tmp[3];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '*' || ch == '-' || ch == '.' || ch == '_') {
            sb_add(b, (const char *)&ch, 1);
        } else if (ch == ' ') {
            sb_add(b, "+", 1);
        } else if (ch == '\n' && (i == 0 || s[i - 1] != '\r')) {
            sb_add(b, "%0D%0A", 6);
        } else {
            tmp[0] = '%'; tmp[1] = hex[ch >> 4]; tmp[2] = hex[ch & 15];
            sb_add(b, tmp, 3);
        }
    }
}

static void sb_pair(sbuf_t *b, const char *name, const char *suffix, const char *value) {
    if (b->len) sb_add(b, "&", 1);
    sb_enc(b, name, strlen(name));
    if (suffix) sb_add(b, suffix, strlen(suffix));
    sb_add(b, "=", 1);
    sb_enc(b, value, strlen(value));
}

static void collect(dom_node_t *root, dom_node_t *n, dom_node_t *form, dom_node_t *submitter, sbuf_t *b) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        const char *name;
        int kind;
        if (k->type != N_ELEMENT) continue;
        kind = form_kind(k);
        if (kind == FK_NONE || k->tag == T_TEMPLATE) {
            if (k->tag != T_TEMPLATE) collect(root, k, form, submitter, b);
            continue;
        }
        if (k->tag == T_SELECT) {
            /* options are not controls themselves */
        } else if (k->first) {
            collect(root, k, form, submitter, b);
        }
        name = dom_attr(k, "name");
        if (form_disabled(k) || form_owner(root, k) != form) continue;
        switch (kind) {
        case FK_SUBMIT:
            if (k == submitter && name && *name) sb_pair(b, name, 0, form_value(k));
            break;
        case FK_IMAGE:
            if (k == submitter) {
                sb_pair(b, name && *name ? name : "", name && *name ? ".x" : "x", "0");
                sb_pair(b, name && *name ? name : "", name && *name ? ".y" : "y", "0");
            }
            break;
        case FK_RESET:
        case FK_BUTTON:
            break;
        case FK_CHECKBOX:
        case FK_RADIO:
            if (name && *name && form_ctl(k)->checked) {
                const char *v = dom_attr(k, "value");
                sb_pair(b, name, 0, v ? v : "on");
            }
            break;
        case FK_SELECT:
            if (name && *name && form_ctl(k)->selected >= 0) sb_pair(b, name, 0, form_value(k));
            break;
        case FK_FILE:
            if (name && *name) sb_pair(b, name, 0, "");
            break;
        default:
            if (name && *name) sb_pair(b, name, 0, form_value(k));
            break;
        }
    }
}

int form_submission(dom_doc_t *doc, dom_node_t *form, dom_node_t *submitter,
                    char *url, size_t url_cap, int *is_post, char **body, size_t *body_len) {
    const char *action = 0, *method = 0;
    char target[URL_CAP];
    sbuf_t b = { 0, 0, 0 };
    *body = 0;
    *body_len = 0;
    *is_post = 0;
    if (!doc || !form) return -1;
    if (submitter) {
        action = dom_attr(submitter, "formaction");
        method = dom_attr(submitter, "formmethod");
    }
    if (!action) action = dom_attr(form, "action");
    if (!method) method = dom_attr(form, "method");
    if (url_resolve(doc->base_url, action && *action ? action : doc->base_url, target, sizeof target) != 0) return -1;
    collect(doc->root, doc->root, form, submitter, &b);
    if (method && ieq(method, "post")) {
        *is_post = 1;
        snprintf(url, url_cap, "%s", target);
        *body = b.s ? b.s : (char *)calloc(1, 1);
        *body_len = b.len;
        return 0;
    }
    {
        /* GET replaces the action's query; any fragment is dropped */
        char *q = strchr(target, '?'), *h = strchr(target, '#');
        if (h) *h = 0;
        if (q) *q = 0;
        snprintf(url, url_cap, "%s?%s", target, b.s ? b.s : "");
    }
    free(b.s);
    return 0;
}

static void reset_walk(dom_node_t *root, dom_node_t *n, dom_node_t *form) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT) continue;
        if (k->ctl && form_owner(root, k) == form) {
            form_ctl_t *c = k->ctl;
            k->ctl = 0;
            free(c->value);
            free(c);
        }
        reset_walk(root, k, form);
    }
}

void form_reset(dom_node_t *doc_root, dom_node_t *form) {
    reset_walk(doc_root, doc_root, form);
}

void form_release(dom_node_t *root) {
    for (dom_node_t *k = root ? root->first : 0; k; k = k->next) {
        if (k->ctl) {
            free(k->ctl->value);
            free(k->ctl);
            k->ctl = 0;
        }
        form_release(k);
    }
}
