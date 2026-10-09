/* /dev/notify: the notification store.  Any program writes a notification -
 * "title\nbody" (or "app|title\nbody") - and the window manager shows it and
 * keeps it in the notification centre.  The last NOTIFY_MAX stay here, in
 * the kernel, so the history outlives the window manager.
 *
 * read:  one entry per line, oldest first: "seq<TAB>uptime_s<TAB>app<TAB>title<TAB>body"
 * write: a notification, or "clear" (the history) */

#include "notify.h"
#include "../proc/sched.h"

#define NOTIFY_MAX   50
#define TITLE_MAX    64
#define BODY_MAX     160
#define APP_MAX      24

typedef struct {
    uint32_t seq;
    uint32_t at_s;
    char app[APP_MAX], title[TITLE_MAX], body[BODY_MAX];
} note_t;

static note_t notes[NOTIFY_MAX];
static int count, head;              /* head: next slot */
static uint32_t next_seq = 1;

static void copy_field(char *dst, int cap, const char *src, uint64_t len) {
    int n = 0;
    for (uint64_t i = 0; i < len && n + 1 < cap; i++) {
        char ch = src[i];
        if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
        if ((unsigned char)ch < 32) continue;
        dst[n++] = ch;
    }
    dst[n] = 0;
}

uint64_t notify_node_write(const char *buf, uint64_t len) {
    note_t *e;
    uint64_t nl = 0, bar = (uint64_t)-1, start = 0;
    if (len == 5 && buf[0] == 'c' && buf[1] == 'l' && buf[2] == 'e' && buf[3] == 'a' && buf[4] == 'r') {
        count = head = 0;
        return len;
    }
    if (len == 0) return (uint64_t)-1;
    while (nl < len && buf[nl] != '\n') nl++;
    for (uint64_t i = 0; i < nl; i++) if (buf[i] == '|') { bar = i; break; }
    e = &notes[head];
    e->app[0] = 0;
    if (bar != (uint64_t)-1) {
        copy_field(e->app, APP_MAX, buf, bar);
        start = bar + 1;
    }
    copy_field(e->title, TITLE_MAX, buf + start, nl - start);
    copy_field(e->body, BODY_MAX, nl < len ? buf + nl + 1 : buf, nl < len ? len - nl - 1 : 0);
    e->seq = next_seq++;
    e->at_s = (uint32_t)(sched_ticks() / 100);
    head = (head + 1) % NOTIFY_MAX;
    if (count < NOTIFY_MAX) count++;
    return len;
}

static void put(char *buf, uint64_t cap, uint64_t *n, const char *s) {
    while (*s && *n + 1 < cap) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static void put_num(char *buf, uint64_t cap, uint64_t *n, uint32_t v) {
    char t[12];
    int k = 0;
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) { char s[2] = { t[--k], 0 }; put(buf, cap, n, s); }
}

uint64_t notify_node_read(char *buf, uint64_t cap) {
    uint64_t n = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < count; i++) {
        const note_t *e = &notes[(head - count + i + NOTIFY_MAX) % NOTIFY_MAX];
        put_num(buf, cap, &n, e->seq);
        put(buf, cap, &n, "\t");
        put_num(buf, cap, &n, e->at_s);
        put(buf, cap, &n, "\t");
        put(buf, cap, &n, e->app);
        put(buf, cap, &n, "\t");
        put(buf, cap, &n, e->title);
        put(buf, cap, &n, "\t");
        put(buf, cap, &n, e->body);
        put(buf, cap, &n, "\n");
    }
    return n;
}
