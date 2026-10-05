#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http.h"
#include "dom.h"
#include "css.h"
static css_sheet_t *sheets[64]; static int nsheets;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void gather(dom_node_t *n, const char *base) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEMENT) continue;
        if (c->tag == T_STYLE && c->first && nsheets < 64) {
            sheets[nsheets] = css_sheet_new(); css_parse(sheets[nsheets++], c->first->text, c->first->text_len, 1, 1024);
        } else if (c->tag == T_LINK && dom_attr(c, "rel") && strstr(dom_attr(c, "rel"), "stylesheet") && dom_attr(c, "href") && nsheets < 64) {
            char url[URL_CAP], fin[URL_CAP];
            url_resolve(base, dom_attr(c, "href"), url, sizeof url);
            http_req_t *r = http_get(url, fin, sizeof fin);
            if (r && r->state == HTTP_DONE && r->body) { sheets[nsheets] = css_sheet_new(); css_parse(sheets[nsheets++], (char *)r->body, r->body_len, 1, 1024); printf("css %s %lu bytes\n", url, (unsigned long)r->body_len); }
            http_free(r);
        }
        gather(c, base);
    }
}
static int shown, hidden, styled;
static void stats(dom_node_t *n) { for (dom_node_t *c = n->first; c; c = c->next) if (c->type == N_ELEMENT) { if (c->style) { styled++; if (c->style->display == D_NONE) hidden++; else shown++; } stats(c); } }
static void show(const char *what, dom_node_t *n) {
    if (!n || !n->style) { printf("%s: none\n", what); return; }
    css_style_t *s = n->style;
    printf("%s <%s> display=%d font=%.1fpx w%d %s color=#%06x bg=#%08x margin=%.0f/%.0f/%.0f/%.0f(u%d) width=%.0f(u%d)\n", what, n->name, s->display, s->font_size, s->font_weight,
           s->font_family ? s->font_family : "-", s->color & 0xFFFFFF, s->bg_color, s->margin[0].v, s->margin[1].v, s->margin[2].v, s->margin[3].v, s->margin[1].unit, s->width.v, s->width.unit);
}
int main(int argc, char **argv) {
    char final[URL_CAP];
    (void)argc;
    http_req_t *r = http_get(argv[1], final, sizeof final);
    if (!r || r->state != HTTP_DONE) { printf("fetch failed\n"); return 1; }
    double t0 = now_ms();
    dom_doc_t *d = html_parse((const char *)r->body, r->body_len, final);
    double t1 = now_ms();
    gather(d->root, d->base_url);
    double t2 = now_ms();
    { int tr = 0, tu = 0; for (int i = 0; i < nsheets; i++) { int r, u; extern void css_sheet_stats(css_sheet_t *, int *, int *); css_sheet_stats(sheets[i], &r, &u); tr += r; tu += u; } printf("rules %d universal %d\n", tr, tu); }
    t2 = now_ms();
    css_cascade(d, sheets, nsheets, 1024, 768);
    double t3 = now_ms();
    stats(d->root);
    printf("parse %.1f ms, css parse %.1f ms (%d sheets, excl. fetch), cascade %.1f ms; elements styled %d shown %d hidden %d; arena %lu KB\n",
           t1 - t0, 0.0, nsheets, t3 - t2, styled, shown, hidden, (unsigned long)(d->arena.used_total / 1024));
    show("body", d->body); show("h1", dom_find(d->body, T_H1)); show("p", dom_find(d->body, T_P)); show("a", dom_find(d->body, T_A));
    return 0;
}
