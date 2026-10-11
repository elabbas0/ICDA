/* assistantd - the assistant's answers.
 *
 * The window manager's assistant overlay writes a question to
 * /tmp/ai/request ("seq\nquestion"); this daemon picks a model, streams its
 * answer into /tmp/ai/reply ("seq state provider\nanswer so far") and keeps
 * the last few exchanges as the conversation.
 *
 * Models are any OpenAI-compatible chat endpoint, listed in
 * /home/.config/ai.conf (made with examples on first start):
 *
 *   <tier> <name> <base url> <model> <api key or ->
 *
 * tier "local" serves daily questions (file: - icda-llm, llama.cpp on this
 * machine, through /tmp/ai/local-request and -reply),
 * "cloud" the complex ones: code, research, long questions, or anything
 * starting with "/cloud".  Each request tries its tier's providers in order,
 * then the other tier's, until one answers - a provider out of free quota or
 * offline is skipped. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "icda_sys.h"

#define CONF_PATH   "/home/.config/ai.conf"
#define REQ_PATH    "/tmp/ai/request"
#define REPLY_PATH  "/tmp/ai/reply"
#define MAX_PROV    12
#define HIST        6            /* exchanges remembered */
#define Q_MAX       512
#define A_MAX       8000

typedef struct {
    int  cloud;
    char name[32], url[160], model[96], key[160];
} provider_t;

static provider_t prov[MAX_PROV];
static int nprov;

static struct { char q[Q_MAX], a[A_MAX]; } hist[HIST];
static int nhist;

static char answer[A_MAX];
static uint32_t cur_seq;
static const char *cur_name = "";

static const char conf_default[] =
    "# ICDA assistant: the models it asks, tried in order.\n"
    "#   <tier> <name> <base url> <model> <api key or ->\n"
    "# local answers daily questions: file:auto runs the first .gguf model found in\n"
    "# an icda-models folder on any drive (C:\\icda-models from Windows works), on\n"
    "# this computer, offline.  cloud answers the complex ones (code, research,\n"
    "# long questions, or \"/cloud ...\"): any OpenAI-compatible endpoint works.\n"
    "# Free cloud tiers are rate-limited and may train on what you send: keep\n"
    "# private things out of them.\n"
    "local qwen       file:auto   local   -\n"
    "# Free keys:  console.groq.com   aistudio.google.com   openrouter.ai\n"
    "#cloud groq       https://api.groq.com/openai/v1                           llama-3.3-70b-versatile               YOUR_KEY\n"
    "#cloud gemini     https://generativelanguage.googleapis.com/v1beta/openai  gemini-2.5-flash                      YOUR_KEY\n"
    "#cloud openrouter https://openrouter.ai/api/v1                             meta-llama/llama-3.3-70b-instruct:free YOUR_KEY\n"
    "# OpenCode Zen with your own key (its free tier only works inside OpenCode):\n"
    "#cloud opencode   https://opencode.ai/zen/v1                               claude-haiku-4-5                      YOUR_KEY\n";

/* ---- config ---------------------------------------------------------------- */

static int word(const char **p, char *out, int cap) {
    int n = 0;
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r') {
        if (n + 1 < cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n;
}

static void load_conf(void) {
    static char text[8192];
    long n = (long)icda_read_file(CONF_PATH, text, sizeof(text) - 1);
    const char *p;
    if (n <= 0) {
        (void)icda_mkdir("/home/.config");
        (void)icda_write_file(CONF_PATH, conf_default, sizeof(conf_default) - 1);
        strcpy(text, conf_default);
        n = (long)(sizeof(conf_default) - 1);
    }
    text[n] = 0;
    nprov = 0;
    for (p = text; *p && nprov < MAX_PROV; ) {
        char tier[16];
        provider_t *v = &prov[nprov];
        if (*p != '#' && word(&p, tier, sizeof(tier)) && word(&p, v->name, sizeof(v->name)) &&
            word(&p, v->url, sizeof(v->url)) && word(&p, v->model, sizeof(v->model)) && word(&p, v->key, sizeof(v->key))) {
            v->cloud = strcmp(tier, "cloud") == 0;
            if (strcmp(v->key, "-") == 0) v->key[0] = 0;
            if (!strstr(v->key, "YOUR_KEY")) nprov++;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

/* ---- the reply file ------------------------------------------------------- */

static void reply(const char *state) {
    static char out[A_MAX + 256];
    int n = snprintf(out, sizeof(out), "%u %s %s\n%s", cur_seq, state, cur_name, answer);
    if (n > (int)sizeof(out) - 1) n = (int)sizeof(out) - 1;
    (void)icda_write_file(REPLY_PATH, out, (uint64_t)n);
}

/* ---- JSON ------------------------------------------------------------------ */

static void json_str(char *out, size_t cap, size_t *n, const char *s) {
    if (*n + 1 < cap) out[(*n)++] = '"';
    for (; *s && *n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[(*n)++] = '\\'; out[(*n)++] = (char)c; }
        else if (c == '\n') { out[(*n)++] = '\\'; out[(*n)++] = 'n'; }
        else if (c == '\t') { out[(*n)++] = '\\'; out[(*n)++] = 't'; }
        else if (c < 0x20) *n += (size_t)snprintf(out + *n, cap - *n, "\\u%04x", c);
        else out[(*n)++] = (char)c;
    }
    if (*n + 1 < cap) out[(*n)++] = '"';
    out[*n] = 0;
}

static void put(char *out, size_t cap, size_t *n, const char *s) {
    while (*s && *n + 1 < cap) out[(*n)++] = *s++;
    out[*n] = 0;
}

/* appends the JSON string at p (just past its opening quote) to dst */
static const char *json_unescape(const char *p, char *dst, size_t cap, size_t *n) {
    while (*p && *p != '"') {
        unsigned cp = 0;
        if (*p != '\\') { if (*n + 1 < cap) dst[(*n)++] = *p; p++; continue; }
        p++;
        switch (*p) {
        case 'n': cp = '\n'; break;
        case 't': cp = '\t'; break;
        case 'r': cp = 0; break;
        case 'u': {
            for (int i = 1; i <= 4 && p[i]; i++) {
                char h = p[i];
                cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            }
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {   /* surrogate pair */
                unsigned lo = 0;
                for (int i = 3; i <= 6 && p[i]; i++) {
                    char h = p[i];
                    lo = lo * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            break;
        }
        default: cp = (unsigned char)*p;
        }
        if (*p) p++;
        if (!cp) continue;
        if (cp < 0x80) { if (*n + 1 < cap) dst[(*n)++] = (char)cp; }
        else if (cp < 0x800) { if (*n + 2 < cap) { dst[(*n)++] = (char)(0xC0 | (cp >> 6)); dst[(*n)++] = (char)(0x80 | (cp & 63)); } }
        else if (cp < 0x10000) { if (*n + 3 < cap) { dst[(*n)++] = (char)(0xE0 | (cp >> 12)); dst[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); dst[(*n)++] = (char)(0x80 | (cp & 63)); } }
        else if (*n + 4 < cap) { dst[(*n)++] = (char)(0xF0 | (cp >> 18)); dst[(*n)++] = (char)(0x80 | ((cp >> 12) & 63)); dst[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); dst[(*n)++] = (char)(0x80 | (cp & 63)); }
    }
    dst[*n] = 0;
    return p;
}

/* the text of "content" inside "delta" (a stream chunk) or "message" */
static int take_content(const char *json, char *dst, size_t cap, size_t *n) {
    const char *o = strstr(json, "\"delta\"");
    const char *c;
    if (!o) o = strstr(json, "\"message\"");
    if (!o) return 0;
    c = strstr(o, "\"content\"");
    if (!c) return 0;
    c += 9;
    while (*c == ' ' || *c == ':') c++;
    if (*c != '"') return 0;                   /* null: a role or tool chunk */
    json_unescape(c + 1, dst, cap, n);
    return 1;
}

/* an error message from a provider's JSON error body, or "" */
static void take_error(const char *json, char *out, size_t cap) {
    const char *m = strstr(json, "\"message\"");
    size_t n = 0;
    out[0] = 0;
    if (!m) return;
    m += 9;
    while (*m == ' ' || *m == ':') m++;
    if (*m == '"') json_unescape(m + 1, out, cap, &n);
}

/* ---- asking a provider ------------------------------------------------------- */

static void today(char *out, size_t cap) {
    char r[96];
    long n = (long)icda_read_file("/dev/rtc", r, sizeof(r) - 1);
    out[0] = 0;
    if (n > 0) {
        r[n] = 0;
        for (long i = 0; i < n; i++) if (r[i] == '\n') r[i] = ' ';
        snprintf(out, cap, "%s", r);
    }
}

static size_t build_body(const provider_t *v, const char *q, char *body, size_t cap) {
    size_t n = 0;
    char sys[512], when[96];
    today(when, sizeof(when));
    snprintf(sys, sizeof(sys),
             "You are the assistant built into ICDA, a small operating system. Answer plainly and briefly "
             "unless asked for detail; no markdown tables. Current date and time: %s.", when);
    put(body, cap, &n, "{\"model\":");
    json_str(body, cap, &n, v->model);
    put(body, cap, &n, ",\"stream\":true,\"messages\":[{\"role\":\"system\",\"content\":");
    json_str(body, cap, &n, sys);
    put(body, cap, &n, "}");
    for (int i = 0; i < nhist; i++) {
        put(body, cap, &n, ",{\"role\":\"user\",\"content\":");
        json_str(body, cap, &n, hist[i].q);
        put(body, cap, &n, "},{\"role\":\"assistant\",\"content\":");
        json_str(body, cap, &n, hist[i].a);
        put(body, cap, &n, "}");
    }
    put(body, cap, &n, ",{\"role\":\"user\",\"content\":");
    json_str(body, cap, &n, q);
    put(body, cap, &n, "}]}");
    return n;
}

/* 0 answered; -1 this provider failed (the next one is tried); err says why */
static int ask(const provider_t *v, const char *q, char *err, size_t errcap) {
    static char body[24000], line[16384];
    char url[200], headers[300];
    size_t blen = build_body(v, q, body, sizeof(body)), llen = 0, alen = 0;
    http_req_t *r;
    uint64_t last_write = 0, t0 = icda_ticks();
    int st, got = 0;

    snprintf(url, sizeof(url), "%s/chat/completions", v->url);
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\nAccept: text/event-stream\r\n%s%s%s",
             v->key[0] ? "Authorization: Bearer " : "", v->key, v->key[0] ? "\r\n" : "");
    r = http_open_body(url, "POST", headers, body, blen);
    if (!r) {
        snprintf(err, errcap, "%s: cannot connect", v->name);
        return -1;
    }
    answer[0] = 0;
    for (;;) {
        st = http_poll(r, 200);
        if (r->headers_done && r->status != 200) {
            /* collect the error body for its message */
            while (st == HTTP_PENDING && icda_ticks() - t0 < 1000) st = http_poll(r, 200);
            if (r->body && r->body_len) {
                char m[200];
                size_t k = r->body_len < sizeof(line) - 1 ? r->body_len : sizeof(line) - 1;
                memcpy(line, r->body, k);
                line[k] = 0;
                take_error(line, m, sizeof(m));
                snprintf(err, errcap, "%s: %d %s", v->name, r->status, m);
            } else {
                snprintf(err, errcap, "%s: HTTP %d", v->name, r->status);
            }
            http_free(r);
            return -1;
        }
        /* server-sent events: "data: {json}" lines, "data: [DONE]" at the end */
        for (size_t bi = 0; bi < r->body_len; bi++) {
            uint8_t ch = r->body[bi];
            if (ch != '\n') {
                if (llen + 1 < sizeof(line)) line[llen++] = (char)ch;
                continue;
            }
            line[llen] = 0;
            if (llen > 6 && strncmp(line, "data:", 5) == 0) {
                const char *d = line + 5;
                while (*d == ' ') d++;
                if (strncmp(d, "[DONE]", 6) != 0 && take_content(d, answer, sizeof(answer), &alen)) got = 1;
            } else if (llen > 2 && line[0] == '{' && take_content(line, answer, sizeof(answer), &alen)) {
                got = 1;                                /* a provider that ignored "stream" */
            }
            llen = 0;
        }
        r->body_len = 0;
        if (got && icda_ticks() - last_write >= 10) {   /* the overlay sees it grow */
            last_write = icda_ticks();
            reply("answer");
        }
        if (st != HTTP_PENDING) break;
        if (icda_ticks() - t0 > 12000) {               /* two minutes */
            snprintf(err, errcap, "%s: no answer in time", v->name);
            http_free(r);
            return got ? 0 : -1;
        }
    }
    if (llen && line[0] == '{' && take_content(line, answer, sizeof(answer), &alen)) got = 1;
    if (!got) snprintf(err, errcap, "%s: %s", v->name, st == HTTP_ERROR ? (r->error[0] ? r->error : "connection failed") : "empty answer");
    http_free(r);
    return got ? 0 : -1;
}

/* ---- the local model (icda-llm, through files) --------------------------------- */

#define LREQ   "/tmp/ai/local-request"
#define LREPLY "/tmp/ai/local-reply"

static int llm_started;
static uint32_t lseq;

/* the first entry of dir ending in suffix ("" for any), as dir/name in out */
static int find_in(const char *dir, const char *suffix, char *out, size_t cap) {
    static char list[8192];
    long n = (long)icda_list_dir(dir, list, sizeof(list) - 1);
    size_t sl = strlen(suffix);
    if (n <= 0) return 0;
    list[n] = 0;
    for (char *p = list; *p;) {
        char *e = strchr(p, '\n');
        size_t len;
        if (e) *e = 0;
        len = strlen(p);
        if (len && p[len - 1] == '/') p[--len] = 0;
        if (len >= sl && strcmp(p + len - sl, suffix) == 0 && len) {
            snprintf(out, cap, "%s/%s", dir, p);
            return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* path on any mounted volume: /volumes/<fs>-<n>/<rel>, or under / */
static int find_on_volumes(const char *rel, const char *suffix, char *out, size_t cap) {
    static const char *const fs[] = { "fat32", "exfat", "ntfs", "usb" };
    char dir[160];
    snprintf(dir, sizeof(dir), "/%s", rel);
    if (find_in(dir, suffix, out, cap)) return 1;
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 8; i++) {
            snprintf(dir, sizeof(dir), "/volumes/%s-%d/%s", fs[f], i, rel);
            if (find_in(dir, suffix, out, cap)) return 1;
        }
    return 0;
}

/* starts icda-llm with the model: "file:auto" finds the first .gguf in an
 * icda-models folder on any drive (Windows' C: included), "file:/path" names it */
static int llm_start(const provider_t *v, char *err, size_t errcap) {
    char model[256], bin[256], args[320];
    if (llm_started) return 0;
    if (strcmp(v->url, "file:auto") == 0) {
        if (!find_on_volumes("icda-models", ".gguf", model, sizeof(model))) {
            snprintf(err, errcap, "%s: no local model - put a .gguf model in an icda-models folder on any drive "
                     "(for example C:\\icda-models in Windows)", v->name);
            return -1;
        }
    } else {
        snprintf(model, sizeof(model), "%s", v->url + 5);
    }
    if (!find_on_volumes("linux/usr/bin", "icda-llm", bin, sizeof(bin))) {
        snprintf(err, errcap, "%s: icda-llm is not installed (it comes with system updates)", v->name);
        return -1;
    }
    snprintf(args, sizeof(args), "-m %s -t 4 -c 4096", model);
    {
        char note[700];
        int nn = snprintf(note, sizeof(note), "starting %s %s\n", bin, args);
        (void)icda_write_file("/tmp/ai/log", note, (uint64_t)nn);
    }
    if ((long)icda_spawn_args(bin, args) < 0) {
        snprintf(err, errcap, "%s: icda-llm did not start", v->name);
        return -1;
    }
    llm_started = 1;
    return 0;
}

static void lput(char *out, size_t cap, size_t *n, const char *role, const char *text) {
    put(out, cap, n, role);
    if (*n + 1 < cap) out[(*n)++] = '\x1f';
    for (const char *t = text; *t && *n + 2 < cap; t++) if (*t != '\x1e' && *t != '\x1f') out[(*n)++] = *t;
    if (*n + 1 < cap) out[(*n)++] = '\x1e';
    out[*n] = 0;
}

static int ask_local(const provider_t *v, const char *q, char *err, size_t errcap) {
    static char req[24000], rep[A_MAX + 64];
    char sys[400], when[96];
    size_t n;
    uint64_t t0 = icda_ticks(), last = 0;
    if (llm_start(v, err, errcap)) return -1;
    today(when, sizeof(when));
    snprintf(sys, sizeof(sys), "You are the assistant built into ICDA, a small operating system. "
             "Answer plainly and briefly. Current date and time: %s.", when);
    lseq = lseq ? lseq + 1 : (uint32_t)icda_ticks();
    n = (size_t)snprintf(req, sizeof(req), "%u\n", lseq);
    lput(req, sizeof(req), &n, "system", sys);
    for (int i = 0; i < nhist; i++) {
        lput(req, sizeof(req), &n, "user", hist[i].q);
        lput(req, sizeof(req), &n, "assistant", hist[i].a);
    }
    lput(req, sizeof(req), &n, "user", q);
    (void)icda_write_file(LREQ, req, n);
    answer[0] = 0;
    for (;;) {
        long r = (long)icda_read_file(LREPLY, rep, sizeof(rep) - 1);
        if (r > 0) {
            uint32_t s = 0;
            char *p = rep, state[12];
            int k = 0;
            rep[r] = 0;
            while (*p >= '0' && *p <= '9') s = s * 10 + (uint32_t)(*p++ - '0');
            if (*p == ' ') p++;
            while (*p && *p != '\n' && k < 11) state[k++] = *p++;
            state[k] = 0;
            if (*p == '\n') p++;
            if (s == 0 && strcmp(state, "error") == 0) {      /* the model failed to load */
                snprintf(err, errcap, "%s: %s", v->name, p);
                llm_started = 0;
                return -1;
            }
            if (s == lseq) {
                snprintf(answer, sizeof(answer), "%s", p);
                if (strcmp(state, "done") == 0) return answer[0] ? 0 : -1;
                if (strcmp(state, "error") == 0) {
                    snprintf(err, errcap, "%s: %s", v->name, p);
                    return -1;
                }
                if (icda_ticks() - last >= 10) {
                    last = icda_ticks();
                    reply(answer[0] ? "answer" : "thinking");
                }
            }
        }
        /* the first question waits for the model to load */
        if (icda_ticks() - t0 > (uint64_t)(answer[0] ? 30000 : 18000)) {
            snprintf(err, errcap, "%s: the local model did not answer in time", v->name);
            return answer[0] ? 0 : -1;
        }
        icda_sleep(5);
    }
}

/* code, research, long questions - or the user said so */
static int complex_question(const char **q) {
    static const char *const words[] = { "code", "program", "function", "debug", "compile", "research", "compare",
                                         "explain in detail", "write a", "essay", "analy", "step by step" };
    if (strncmp(*q, "/cloud", 6) == 0) {
        *q += 6;
        while (**q == ' ') (*q)++;
        return 1;
    }
    if (strlen(*q) > 280) return 1;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (strstr(*q, words[i])) return 1;
    return 0;
}

static void handle(const char *question) {
    char err[400], errs[800];
    const char *q = question;
    int cloud_first = complex_question(&q);
    load_conf();
    errs[0] = 0;
    cur_name = "";
    answer[0] = 0;
    reply("thinking");
    for (int pass = 0; pass < 2; pass++) {
        int want_cloud = pass == 0 ? cloud_first : !cloud_first;
        for (int i = 0; i < nprov; i++) {
            if (prov[i].cloud != want_cloud) continue;
            cur_name = prov[i].name;
            reply("thinking");
            if ((strncmp(prov[i].url, "file:", 5) == 0 ? ask_local(&prov[i], q, err, sizeof(err))
                                                       : ask(&prov[i], q, err, sizeof(err))) == 0) {
                /* remembered for the conversation */
                if (nhist == HIST) {
                    memmove(&hist[0], &hist[1], sizeof(hist[0]) * (HIST - 1));
                    nhist--;
                }
                snprintf(hist[nhist].q, Q_MAX, "%s", q);
                snprintf(hist[nhist].a, A_MAX, "%s", answer);
                nhist++;
                reply("done");
                return;
            }
            if (strlen(errs) + strlen(err) + 2 < sizeof(errs)) {
                strcat(errs, err);
                strcat(errs, "\n");
            }
        }
    }
    cur_name = "";
    snprintf(answer, sizeof(answer), "%s%s", nprov ? "No model answered:\n" :
             "No AI model is set up yet. Add one to " CONF_PATH " (it lists free options), "
             "or start the local model.\n", errs);
    reply("error");
}

int main(void) {
    static char buf[Q_MAX + 32];
    (void)icda_mkdir("/tmp");
    (void)icda_mkdir("/tmp/ai");
    load_conf();
    {   /* a question left from before this start is not news */
        long n = (long)icda_read_file(REQ_PATH, buf, sizeof(buf) - 1);
        for (long i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) cur_seq = cur_seq * 10 + (uint32_t)(buf[i] - '0');
    }
    for (;;) {
        long n = (long)icda_read_file(REQ_PATH, buf, sizeof(buf) - 1);
        if (n > 0) {
            uint32_t seq = 0;
            char *p = buf;
            buf[n] = 0;
            while (*p >= '0' && *p <= '9') seq = seq * 10 + (uint32_t)(*p++ - '0');
            if (*p == '\n') p++;
            if (seq && seq != cur_seq) {
                cur_seq = seq;
                handle(p);
            }
        }
        icda_sleep(10);                   /* a tenth of a second */
    }
}
