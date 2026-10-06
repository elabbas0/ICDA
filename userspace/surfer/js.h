#ifndef SURFER_JS_H
#define SURFER_JS_H

#include <stddef.h>
#include <stdint.h>
#include "dom.h"

/* Surfer's JavaScript: QuickJS plus a DOM over Surfer's own tree.  The
 * browser supplies the host callbacks; the page runs scripts, timers and
 * network callbacks from js_tick() and reports what changed. */

typedef struct {
    void *ctx;
    /* Blocking fetch for scripts and modules.  *body is malloc'd. */
    int   (*fetch)(void *ctx, const char *url, char **body, size_t *len, char *final_url, size_t cap);
    void  (*navigate)(void *ctx, const char *url, int replace);
    void  (*set_url)(void *ctx, const char *url);          /* history.pushState/replaceState */
    void  (*history_go)(void *ctx, int delta);
    void  (*focus)(void *ctx, dom_node_t *n);               /* 0 blurs */
    dom_node_t *(*active)(void *ctx);
    void  (*scroll_to)(void *ctx, float x, float y);
    float (*scroll_y)(void *ctx);
    void  (*viewport)(void *ctx, int *w, int *h);
    void  (*status)(void *ctx, const char *msg);
    /* Up-to-date layout box of n in document coordinates (lays out first if
     * the DOM changed); 0 if n has no box. */
    int   (*box)(void *ctx, dom_node_t *n, float *x, float *y, float *w, float *h);
    void  (*log)(void *ctx, const char *line);
    /* form.submit(): submit through the browser (GET or POST navigation) */
    void  (*submit)(void *ctx, dom_node_t *form, dom_node_t *submitter);
} js_host_t;

typedef struct js_page js_page_t;

/* What js_tick / js_take_dirty report. */
enum {
    JS_DIRTY_DOM   = 1,     /* restyle and relayout */
    JS_DIRTY_STYLE = 2,     /* <style>/<link> changed: reload stylesheets */
    JS_BUSY        = 4      /* timers, animation frames or requests pending */
};

typedef struct {
    int      x, y;           /* page coordinates (CSS px, document) */
    int      client_x, client_y;
    int      button;
    int      mods;           /* 1 shift, 2 alt, 4 ctrl */
    uint32_t key;            /* ICDA key code for keyboard events */
} js_event_t;

js_page_t *js_page_new(dom_doc_t *doc, const char *url, const js_host_t *host);
void       js_page_free(js_page_t *p);

/* Runs the document's scripts in order, then DOMContentLoaded and load. */
void       js_run_scripts(js_page_t *p);
/* Runs the next script (or, after the last, the load events); 0 when done.
 * The browser calls it once per tick so the page stays usable. */
int        js_run_step(js_page_t *p);
/* The script element js_run_step() runs next (0 at the end). */
dom_node_t *js_next_script(js_page_t *p);
/* Stop button: ends the running script and keeps further scripts from running. */
void       js_abort(js_page_t *p);

/* Dispatches a DOM event at target (bubbling).  Returns 1 if a listener
 * called preventDefault(). */
int        js_dispatch(js_page_t *p, dom_node_t *target, const char *type, const js_event_t *ev);

/* Runs due timers, promise jobs, animation frames and finished requests. */
int        js_tick(js_page_t *p);
int        js_take_dirty(js_page_t *p);

/* Milliseconds until the next timer is due (-1 if none). */
long       js_next_due(js_page_t *p);

/* Evaluates code in the page; the result as a string (debugging aid). */
size_t     js_eval_string(js_page_t *p, const char *code, char *out, size_t cap);

#endif
