#include <stdio.h>
#include <string.h>
#include "http.h"
#include "dom.h"
#include <stdlib.h>
static int counts[3];
static void walk(dom_node_t *n, int depth, int maxdepth) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        if (c->type == N_ELEMENT) counts[0]++; else if (c->type == N_TEXT) counts[1]++;
        if (depth < maxdepth && c->type == N_ELEMENT) printf("%*s<%s%s%s>\n", depth * 2, "", c->name, c->id ? " #" : "", c->id ? c->id : "");
        walk(c, depth + 1, maxdepth);
    }
}
int main(int argc, char **argv) {
    char final[URL_CAP];
    http_req_t *r = http_get(argv[1], final, sizeof final);
    if (!r || r->state != HTTP_DONE) { printf("fetch failed\n"); return 1; }
    dom_doc_t *d = html_parse((const char *)r->body, r->body_len, final);
    walk(d->root, 0, argc > 2 ? atoi(argv[2]) : 4);
    printf("title: %s\nelements %d, text nodes %d, arena %lu KB, html %lu KB\n", d->title, counts[0], counts[1],
           (unsigned long)(d->arena.used_total / 1024), (unsigned long)(r->body_len / 1024));
    dom_free(d);
    return 0;
}
