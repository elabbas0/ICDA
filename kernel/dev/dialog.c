/* /dev/dialog: dialogs that programs, and the kernel, ask the window manager
 * to show, and their answers.
 *
 * A program writes its request as key=value lines:
 *
 *     kind=info | warning | error | question      (info)
 *     title=Save changes to "notes.txt"?           (required)
 *     message=Your changes are lost if you don't.  (each a line; any number)
 *     buttons=Save|Don't Save|Cancel               (up to 4; "OK")
 *     default=0                                    (Enter's button: the first)
 *     cancel=2                                     (Escape's: the last)
 *     window=7                                     (its window: the dialog sits
 *                                                   on it, and the window waits)
 *
 * and reads its answer: "pending" while the dialog is up, "nohost" when no
 * window manager is there to show it (text mode), then "answer=N" once - N
 * the button, -1 when it went without one - and "none" after.  "cancel"
 * takes the request back.  A program has one request at a time.
 *
 * The window manager (/apps/wm.app, started by the system) writes "host",
 * then reads the open requests - "gen=N", then per request "id=", "pid=",
 * "uid=", "name=" and the request's own lines, and an empty line - and
 * answers with "answer <id> <button>".  The kernel's own requests have pid
 * 0 and need no answer.
 *
 * Callers hold the kernel lock (system calls, faults), like /dev/notify's. */

#include "dialog.h"
#include "../proc/sched.h"
#include "../proc/process.h"

#define DIALOG_MAX       16
#define DIALOG_TEXT_MAX  1536
#define DIALOG_KERNEL_MAX 4          /* the kernel's open at once: a crash loop must not fill the screen */

enum { DLG_FREE = 0, DLG_OPEN, DLG_ANSWERED };

typedef struct {
    uint32_t id;
    int      state;
    uint64_t pid;                     /* who asked; 0: the kernel */
    uint32_t uid;                     /* whose: 0 anyone's */
    int      answer;
    uint32_t len;
    char     name[32];                /* the asking program */
    char     text[DIALOG_TEXT_MAX];   /* the request, cleaned: key=value lines */
} dlg_t;

static dlg_t dlgs[DIALOG_MAX];
static uint32_t next_id = 1;
static uint32_t gen = 1;              /* changes whenever the open requests do */
static uint64_t host_pid;

static process_t *me(void) {
    return sched_current_process();
}

static int alive(uint64_t pid) {
    process_t *p = pid ? sched_find_process(pid) : 0;
    return p && p->state != PROCESS_EXITED && p->state != PROCESS_REAPED;
}

static int is_host(const process_t *p) {
    return p && host_pid && p->pid == host_pid;
}

static int str_starts(const char *s, uint64_t len, const char *pre) {
    uint64_t i = 0;
    for (; pre[i]; i++)
        if (i >= len || s[i] != pre[i]) return 0;
    return 1;
}

static int str_is(const char *s, uint64_t len, const char *word) {
    uint64_t i = 0;
    for (; word[i]; i++)
        if (i >= len || s[i] != word[i]) return 0;
    while (i < len && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) i++;
    return i == len;
}

static void copy_str(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* the request's text, kept to printable characters and newlines (a tab is a
 * space), ending in a newline */
static uint32_t clean(char *dst, const char *src, uint64_t len) {
    uint32_t n = 0;
    for (uint64_t i = 0; i < len && n + 2 < DIALOG_TEXT_MAX; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\t') ch = ' ';
        if (ch == '\n' || ch >= 32) {
            if (ch == 127) continue;
            dst[n++] = (char)ch;
        }
    }
    if (n && dst[n - 1] != '\n') dst[n++] = '\n';
    dst[n] = 0;
    return n;
}

/* a "title=" line with something after it */
static int has_title(const char *t, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        if (str_starts(t + i, len - i, "title=") && i + 6 < len && t[i + 6] != '\n') return 1;
        while (i < len && t[i] != '\n') i++;
        i++;
    }
    return 0;
}

static dlg_t *of_pid(uint64_t pid) {
    for (int i = 0; i < DIALOG_MAX; i++)
        if (dlgs[i].state != DLG_FREE && dlgs[i].pid == pid && pid) return &dlgs[i];
    return 0;
}

static dlg_t *of_id(uint32_t id) {
    for (int i = 0; i < DIALOG_MAX; i++)
        if (dlgs[i].state != DLG_FREE && dlgs[i].id == id) return &dlgs[i];
    return 0;
}

static void release(dlg_t *d) {
    if (d->state == DLG_OPEN) gen++;
    d->state = DLG_FREE;
    d->id = 0;
    d->pid = 0;
}

static dlg_t *slot(void) {
    for (int i = 0; i < DIALOG_MAX; i++)
        if (dlgs[i].state == DLG_FREE) return &dlgs[i];
    return 0;
}

static dlg_t *open_request(uint64_t pid, uint32_t uid, const char *name) {
    dlg_t *d = slot();
    if (!d) return 0;
    d->id = next_id++;
    if (!next_id) next_id = 1;
    d->state = DLG_OPEN;
    d->pid = pid;
    d->uid = uid;
    d->answer = -1;
    copy_str(d->name, sizeof(d->name), name);
    gen++;
    return d;
}

static uint64_t parse_int(const char *s, uint64_t len, uint64_t *i, int *neg) {
    uint64_t v = 0;
    *neg = 0;
    while (*i < len && s[*i] == ' ') (*i)++;
    if (*i < len && s[*i] == '-') { *neg = 1; (*i)++; }
    while (*i < len && s[*i] >= '0' && s[*i] <= '9') v = v * 10 + (uint64_t)(s[(*i)++] - '0');
    return v;
}

uint64_t dialog_node_write(const char *buf, uint64_t len) {
    process_t *p = me();
    dlg_t *d;
    if (!p || !buf || len == 0) return (uint64_t)-1;

    if (str_is(buf, len, "host")) {
        /* the session's window manager, while no other one is alive */
        if (p->kind != PROCESS_USER || !p->ex_wm) return (uint64_t)-1;
        if (host_pid && host_pid != p->pid && alive(host_pid)) return (uint64_t)-1;
        host_pid = p->pid;
        gen++;
        return len;
    }
    if (str_starts(buf, len, "answer ")) {
        uint64_t i = 7, id, button;
        int neg, neg_id;
        if (!is_host(p)) return (uint64_t)-1;
        id = parse_int(buf, len, &i, &neg_id);
        button = parse_int(buf, len, &i, &neg);
        d = of_id((uint32_t)id);
        if (!d || d->state != DLG_OPEN || neg_id) return (uint64_t)-1;
        if (!d->pid) {
            release(d);                    /* the kernel's: nobody waits */
        } else {
            d->state = DLG_ANSWERED;
            d->answer = neg || button > 16 ? -1 : (int)button;
            gen++;
        }
        return len;
    }
    if (str_is(buf, len, "cancel")) {
        d = of_pid(p->pid);
        if (d) release(d);
        return len;
    }

    /* a request */
    if (of_pid(p->pid)) return (uint64_t)-1;          /* one at a time */
    {
        static char tmp[DIALOG_TEXT_MAX];
        uint32_t n = clean(tmp, buf, len);
        if (!has_title(tmp, n)) return (uint64_t)-1;
        d = open_request(p->pid, p->kind == PROCESS_USER ? p->ex_uid : 0, p->name);
        if (!d) return (uint64_t)-1;
        for (uint32_t i = 0; i <= n; i++) d->text[i] = tmp[i];
        d->len = n;
    }
    return len;
}

static void put(char *buf, uint64_t cap, uint64_t *n, const char *s) {
    while (*s && *n + 1 < cap) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static void put_num(char *buf, uint64_t cap, uint64_t *n, uint64_t v) {
    char t[24];
    int k = 0;
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) { char s[2] = { t[--k], 0 }; put(buf, cap, n, s); }
}

uint64_t dialog_node_read(char *buf, uint64_t cap) {
    process_t *p = me();
    uint64_t n = 0;
    dlg_t *d;
    if (!cap) return 0;
    buf[0] = 0;
    if (!p) return 0;

    if (is_host(p)) {
        uint32_t uid = p->ex_uid;
        put(buf, cap, &n, "gen=");
        put_num(buf, cap, &n, gen);
        put(buf, cap, &n, "\n\n");
        for (int i = 0; i < DIALOG_MAX; i++) {
            d = &dlgs[i];
            if (d->state != DLG_OPEN || (d->uid && d->uid != uid)) continue;
            /* whole requests only: a cut one would read as a different dialog */
            if (n + d->len + 96 >= cap) break;
            put(buf, cap, &n, "id=");
            put_num(buf, cap, &n, d->id);
            put(buf, cap, &n, "\npid=");
            put_num(buf, cap, &n, d->pid);
            put(buf, cap, &n, "\nuid=");
            put_num(buf, cap, &n, d->uid);
            put(buf, cap, &n, "\nname=");
            put(buf, cap, &n, d->name);
            put(buf, cap, &n, "\n");
            put(buf, cap, &n, d->text);
            put(buf, cap, &n, "\n");
        }
        return n;
    }

    d = of_pid(p->pid);
    if (!d) {
        put(buf, cap, &n, "none\n");
    } else if (d->state == DLG_OPEN) {
        put(buf, cap, &n, host_pid && alive(host_pid) ? "pending\n" : "nohost\n");
    } else {
        put(buf, cap, &n, "answer=");
        if (d->answer < 0) put(buf, cap, &n, "-1");
        else put_num(buf, cap, &n, (uint64_t)d->answer);
        put(buf, cap, &n, "\n");
        release(d);
    }
    return n;
}

static void line(char *t, uint32_t *n, const char *key, const char *value) {
    if (!value || !value[0]) return;
    for (const char *k = key; *k && *n + 2 < DIALOG_TEXT_MAX; k++) t[(*n)++] = *k;
    for (const char *v = value; *v && *n + 2 < DIALOG_TEXT_MAX; v++) {
        if (*v == '\n') {
            /* each line of the message its own message= line */
            t[(*n)++] = '\n';
            for (const char *k = key; *k && *n + 2 < DIALOG_TEXT_MAX; k++) t[(*n)++] = *k;
        } else if ((unsigned char)*v >= 32) {
            t[(*n)++] = *v;
        }
    }
    t[(*n)++] = '\n';
    t[*n] = 0;
}

static void num_line(char *t, uint32_t *n, const char *key, uint64_t v) {
    char d[24], s[24];
    int k = 0, m = 0;
    do d[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) s[m++] = d[--k];
    s[m] = 0;
    line(t, n, key, s);
}

static void post(const char *kind, const char *app, const char *title, const char *message, uint32_t uid,
                 uint64_t stopped, const char *label) {
    static char t[DIALOG_TEXT_MAX];
    uint32_t n = 0;
    int open = 0;
    dlg_t *d, *oldest = 0;
    t[0] = 0;
    line(t, &n, "kind=", kind ? kind : "error");
    line(t, &n, "title=", title);
    line(t, &n, "message=", message);
    line(t, &n, "app=", app);
    if (stopped) num_line(t, &n, "stopped=", stopped);
    line(t, &n, "label=", label);
    for (int i = 0; i < DIALOG_MAX; i++) {
        d = &dlgs[i];
        if (d->state != DLG_OPEN || d->pid) continue;
        open++;
        if (!oldest || d->id < oldest->id) oldest = d;
        /* the same one again (a program that keeps stopping): once is enough */
        if (d->uid == uid && d->len == n) {
            uint32_t k = 0;
            while (k < n && d->text[k] == t[k]) k++;
            if (k == n) return;
        }
    }
    if (open >= DIALOG_KERNEL_MAX && oldest) release(oldest);
    d = open_request(0, uid, app ? app : "kernel");
    if (!d) return;
    for (uint32_t i = 0; i <= n; i++) d->text[i] = t[i];
    d->len = n;
}

void dialog_post(const char *kind, const char *app, const char *title, const char *message, uint32_t uid) {
    post(kind, app, title, message, uid, 0, 0);
}

void dialog_post_stopped(uint64_t pid, const char *app, const char *label, const char *title,
                         const char *message, uint32_t uid) {
    post("error", app, title, message, uid, pid, label);
}

void dialog_proc_exit(struct process *proc) {
    if (!proc) return;
    for (int i = 0; i < DIALOG_MAX; i++)
        if (dlgs[i].state != DLG_FREE && dlgs[i].pid == proc->pid) release(&dlgs[i]);
    if (proc->pid == host_pid) {
        host_pid = 0;
        gen++;
    }
}

void dialog_app_label(const char *name, char *out, int cap) {
    int n = 0;
    if (cap <= 0) return;
    for (int i = 0; name && name[i] && n + 1 < cap; i++) {
        if (name[i] == '.') break;                       /* "editor.app" */
        out[n] = (char)(n == 0 && name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        n++;
    }
    out[n] = 0;
    if (!n) copy_str(out, cap, "A program");
}
