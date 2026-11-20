/* Dialogs through the window manager: the request format is in
 * kernel/dev/dialog.c. */

#include "ic_dialog.h"
#include "libicda.h"
#include "icda_sys.h"

/* gui.c's, when the program has a window (command-line tools link no gui.o) */
__attribute__((weak)) uint32_t gui_window_id(void);
__attribute__((weak)) void gui_dialog_kick(void);

#define REQ_CAP 1536

static int put(char *req, int n, const char *s, int newlines_ok) {
    for (int i = 0; s && s[i] && n < REQ_CAP - 2; i++) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (ch == '\t' || (ch == '\n' && !newlines_ok)) ch = ' ';
        req[n++] = ch;
    }
    req[n] = 0;
    return n;
}

static int put_line(char *req, int n, const char *key, const char *value) {
    n = put(req, n, key, 0);
    n = put(req, n, value, 0);
    if (n < REQ_CAP - 1) req[n++] = '\n';
    req[n] = 0;
    return n;
}

static int put_num(char *req, int n, const char *key, long v) {
    char num[24];
    int k = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do num[k++] = (char)('0' + u % 10); while (u /= 10);
    if (neg) num[k++] = '-';
    {
        char out[24];
        int m = 0;
        while (k) out[m++] = num[--k];
        out[m] = 0;
        return put_line(req, n, key, out);
    }
}

int ic_dialog_open(const ic_dialog_t *d) {
    static const char *const kinds[] = { "info", "warning", "error", "question" };
    static char req[REQ_CAP];
    int n = 0, count;
    if (!d || !d->title) return -1;
    count = d->buttons ? d->count : 0;
    if (count > 4) count = 4;
    req[0] = 0;
    n = put_line(req, n, "kind=", kinds[d->kind >= IC_DIALOG_INFO && d->kind <= IC_DIALOG_QUESTION ? d->kind : 0]);
    n = put_line(req, n, "title=", d->title);
    if (d->message) {
        /* one message= line per line */
        const char *m = d->message;
        while (*m) {
            int len = 0;
            char line[512];
            while (m[len] && m[len] != '\n' && len < (int)sizeof(line) - 1) { line[len] = m[len]; len++; }
            line[len] = 0;
            n = put_line(req, n, "message=", line);
            m += len;
            if (*m == '\n') m++;
        }
    }
    if (count > 0) {
        n = put(req, n, "buttons=", 0);
        for (int i = 0; i < count; i++) {
            const char *b = d->buttons[i] ? d->buttons[i] : "";
            if (i) n = put(req, n, "|", 0);
            for (int k = 0; b[k] && n < REQ_CAP - 2; k++) req[n++] = b[k] == '|' || b[k] == '\n' ? ' ' : b[k];
            req[n] = 0;
        }
        if (n < REQ_CAP - 1) req[n++] = '\n';
        req[n] = 0;
        n = put_num(req, n, "default=", d->default_button);
        if (d->cancel_button >= 0) n = put_num(req, n, "cancel=", d->cancel_button);
        if (d->destructive_button >= 0) n = put_num(req, n, "destructive=", d->destructive_button);
    }
    if (gui_window_id && gui_window_id()) n = put_num(req, n, "window=", (long)gui_window_id());
    if ((long)icda_write_file("/dev/dialog", req, (uint64_t)n) != (long)n) return -1;
    if (gui_dialog_kick) gui_dialog_kick();
    return 0;
}

int ic_dialog_result(void) {
    char r[64];
    long n = (long)icda_read_file("/dev/dialog", r, sizeof(r) - 1);
    if (n <= 0) return -1;
    r[n] = 0;
    if (ic_strprefix(r, "pending")) return IC_DIALOG_OPEN;
    if (ic_strprefix(r, "nohost")) return IC_DIALOG_NO_HOST;
    if (ic_strprefix(r, "answer=")) {
        const char *s = r + 7;
        int neg = 0, v = 0;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        return neg ? -v : v;
    }
    return -1;                        /* none: nothing open */
}

void ic_dialog_cancel(void) {
    (void)icda_write_file("/dev/dialog", "cancel", 6);
}

int ic_dialog_run(const ic_dialog_t *d) {
    int r;
    if (ic_dialog_open(d) != 0) return -1;
    for (;;) {
        r = ic_dialog_result();
        if (r == IC_DIALOG_NO_HOST) {
            ic_dialog_cancel();
            return r;
        }
        if (r != IC_DIALOG_OPEN) return r;
        icda_sleep(2);
    }
}

int ic_dialog_message(ic_dialog_kind_t kind, const char *title, const char *message) {
    ic_dialog_t d;
    ic_memzero(&d, sizeof(d));
    d.kind = kind;
    d.title = title;
    d.message = message;
    d.cancel_button = -1;
    d.destructive_button = -1;
    return ic_dialog_open(&d);
}
