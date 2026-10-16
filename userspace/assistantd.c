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
#include "settings_store.h"
#include "model_catalog.h"

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
static int speak = 1;                /* read answers aloud (ai.conf: speak on / off) */
static int threads_conf;              /* ai.conf: threads N - the local model's threads, 0: one per core */
/* ai.conf: "search brave <key>" - Brave's search API (free keys at
 * api-dashboard.search.brave.com): full web results, news included */
static char search_key[160];

static struct { char q[Q_MAX], a[A_MAX]; } hist[HIST];
static int nhist;

static char answer[A_MAX];
static uint32_t cur_seq;
static const char *cur_name = "";

static const char conf_default[] =
    "# ICDA assistant: the models it asks, tried in order.\n"
    "#   <tier> <name> <base url> <model> <api key or ->\n"
    "# local answers daily questions: file:auto loads the named GGUF from\n"
    "# ICDA's linux/usr/share/icda/models directory, on\n"
    "# this computer, offline.  cloud answers the complex ones (code, research,\n"
    "# long questions, or \"/cloud ...\"): any OpenAI-compatible endpoint works.\n"
    "# Free cloud tiers are rate-limited and may train on what you send: keep\n"
    "# private things out of them.\n"
    "local qwen       file:auto   qwen2.5-0.5b-instruct-q4_k_m.gguf   -\n"
    "# answers read aloud (offline voice): speak on / speak off\n"
    "speak on\n"
    "# web search: DuckDuckGo, then Wikipedia.  For full results (news too) add a\n"
    "# free Brave Search API key from api-dashboard.search.brave.com:\n"
    "#search brave YOUR_KEY\n"
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
    speak = 1;
    threads_conf = 0;
    search_key[0] = 0;
    for (p = text; *p && nprov < MAX_PROV; ) {
        char tier[16];
        provider_t *v = &prov[nprov];
        if (!strncmp(p, "speak ", 6)) {              /* "speak on" / "speak off": answers read aloud */
            speak = strncmp(p + 6, "off", 3) != 0;
        } else if (!strncmp(p, "threads ", 8)) {
            threads_conf = atoi(p + 8);
        } else if (!strncmp(p, "search brave ", 13)) {
            const char *k = p + 13;
            word(&k, search_key, sizeof(search_key));
            if (strstr(search_key, "YOUR_KEY")) search_key[0] = 0;
        } else if (*p != '#' && word(&p, tier, sizeof(tier)) && word(&p, v->name, sizeof(v->name)) &&
            word(&p, v->url, sizeof(v->url)) && word(&p, v->model, sizeof(v->model)) && word(&p, v->key, sizeof(v->key))) {
            v->cloud = strcmp(tier, "cloud") == 0;
            if (strcmp(v->key, "-") == 0) v->key[0] = 0;
            if (!strstr(v->key, "YOUR_KEY") && strcmp(v->url, "codex:chatgpt")) nprov++;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (!nprov) {
        provider_t *v = &prov[nprov++]; memset(v, 0, sizeof(*v));
        strcpy(v->name, "qwen"); strcpy(v->url, "file:auto");
        strcpy(v->model, icda_models[0].name);
    }
}

/* ---- the reply file ------------------------------------------------------- */

/* the answer as it is shown: a tool call being written is the assistant's
 * business, not the person's */
static const char *shown(void) {
    static char s[A_MAX];
    const char *p = answer;
    char *t;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if ((*p == '{' || !strncmp(p, "```", 3) || *p == '<') && (strstr(p, "\"name\"") || strlen(p) < 12)) return "";
    snprintf(s, sizeof(s), "%s", answer);
    if ((t = strstr(s, "<tool_call>")) != 0) *t = 0;
    return s;
}

static void reply(const char *state) {
    static char out[A_MAX + 256];
    int n = snprintf(out, sizeof(out), "%u %s %s\n%s", cur_seq, state, cur_name, shown());
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

static long long now_local(void);
static void fmt_local(long long t, char *out, size_t cap);
static void rem_check(void);
static void say(const char *text);

/* the local date and time, for the model */
static void today(char *out, size_t cap) {
    long long t = now_local();
    out[0] = 0;
    if (t) fmt_local(t, out, cap);
}

/* ---- what every model is told ------------------------------------------------- */

/* What ICDA is and how to work in it: a system file, so updates can teach the
 * assistant more without a new program. */
#define GUIDE_PATH "/usr/share/icda/assistant/guide.txt"

static const char tools_text[] =
    "\n\nTools. To use one, reply with only the call, nothing before or after it:\n"
    "<tool_call>\n{\"name\": \"read_file\", \"arguments\": {\"path\": \"/home/notes.txt\"}}\n</tool_call>\n"
    "Its result comes back inside <tool_response>. Then use another tool or give your answer.\n"
    "- list_folder {\"path\"}: the files and folders in a folder (folders end with /)\n"
    "- read_file {\"path\"}: the text of a file\n"
    "- write_file {\"path\", \"text\"}: create a file, or replace all its text\n"
    "- append_file {\"path\", \"text\"}: add text to the end of a file\n"
    "- make_folder {\"path\"}: create a folder\n"
    "- move {\"from\", \"to\"}: move or rename a file or folder\n"
    "- web_search {\"query\"}: the top results from the web\n"
    "- read_webpage {\"url\"}: the text of a web page\n"
    "- open_app {\"app\", \"target\"}: open Explorer, Terminal, Surfer, Editor, Media, Disk Utility, "
    "Activity or Settings; target (optional) is a web address for Surfer or a file for Editor\n"
    "- remind {\"minutes\", \"text\"}: a reminder after that many minutes\n"
    "- system_info {}: ICDA's version, memory, processors and how long it has been running\n"
    "Files can be written in /home, /tmp and /volumes. Nothing can be deleted.";

static char sys_prompt[12000];

/* The instructions stay the same from one question to the next - the date
 * goes with the question instead - so the local model reads them only once. */
static void build_system(const provider_t *v) {
    static char guide[8192];
    long n = (long)icda_read_file(GUIDE_PATH, guide, sizeof(guide) - 1);
    if (n <= 0) {
        n = snprintf(guide, sizeof(guide), "You are the assistant built into ICDA (\"I Can Do Anything\"), "
                     "its own operating system. Answer plainly and briefly. Use your tools to act on the computer.");
    }
    guide[n] = 0;
    snprintf(sys_prompt, sizeof(sys_prompt), "%s%s%s", guide, tools_text,
             /* Qwen3 reasons at length first: on a laptop that is minutes */
             strstr(v->model, "Qwen3") || strstr(v->model, "qwen3") ? "\n/no_think" : "");
}

/* The question as the model gets it: with the time, which changes */
static void dated(const char *q, char *out, size_t cap) {
    char when[96];
    today(when, sizeof(when));
    snprintf(out, cap, "%s\n\n(Now: %s)", q, when[0] ? when : "unknown");
}

/* The steps of the current request: what the model said (a tool call) and
 * what the tool gave back, in turns after the question. */
#define STEPS     12
#define STEP_TEXT 8400
static char step_role[STEPS][12];
static char step_text[STEPS][STEP_TEXT];
static int nsteps;

static size_t build_body(const provider_t *v, const char *q, char *body, size_t cap) {
    size_t n = 0;
    static char asked[Q_MAX + 4200];
    dated(q, asked, sizeof(asked));
    put(body, cap, &n, "{\"model\":");
    json_str(body, cap, &n, v->model);
    put(body, cap, &n, ",\"stream\":true,\"messages\":[{\"role\":\"system\",\"content\":");
    json_str(body, cap, &n, sys_prompt);
    put(body, cap, &n, "}");
    for (int i = 0; i < nhist; i++) {
        put(body, cap, &n, ",{\"role\":\"user\",\"content\":");
        json_str(body, cap, &n, hist[i].q);
        put(body, cap, &n, "},{\"role\":\"assistant\",\"content\":");
        json_str(body, cap, &n, hist[i].a);
        put(body, cap, &n, "}");
    }
    put(body, cap, &n, ",{\"role\":\"user\",\"content\":");
    json_str(body, cap, &n, asked);
    put(body, cap, &n, "}");
    for (int i = 0; i < nsteps; i++) {
        put(body, cap, &n, ",{\"role\":");
        json_str(body, cap, &n, step_role[i]);
        put(body, cap, &n, ",\"content\":");
        json_str(body, cap, &n, step_text[i]);
        put(body, cap, &n, "}");
    }
    put(body, cap, &n, "]}");
    return n;
}

/* 0 answered; -1 this provider failed (the next one is tried); err says why */
static int ask(const provider_t *v, const char *q, char *err, size_t errcap) {
    static char body[200000], line[16384];
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
        rem_check();
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

/* ---- local time ------------------------------------------------------------------ */

/* days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) */
static long days_from_civil(long y, long m, long d) {
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* now in local time, as seconds since 1970 (the RTC is UTC; settings give the zone) */
static long long now_local(void) {
    char r[40];
    long n = (long)icda_read_file("/dev/rtc", r, sizeof(r) - 1);
    icda_settings_t s;
    int Y, M, D, h, m, sec;
    if (n < 19) return 0;
    r[n] = 0;
    Y = atoi(r); M = atoi(r + 5); D = atoi(r + 8); h = atoi(r + 11); m = atoi(r + 14); sec = atoi(r + 17);
    icda_settings_load(&s);
    return (long long)days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec + (long long)s.tz_minutes * 60;
}

static void fmt_local(long long t, char *out, size_t cap) {
    static const char *const wd[] = { "Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday" };
    long days = (long)(t / 86400), z = days + 719468, era, doe, yoe, y, doy, mp, d, m;
    int secs = (int)(t % 86400);
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    snprintf(out, cap, "%s %04ld-%02ld-%02ld %02d:%02d", wd[((days % 7) + 7) % 7], y + (m <= 2), m, d,
             secs / 3600, secs / 60 % 60);
}

/* ---- reminders ---------------------------------------------------------------- */

#define REM_PATH "/home/.config/reminders"
#define REM_MAX  32

static struct { long long due; char text[160]; } rem[REM_MAX];
static int nrem;

static void rem_save(void) {
    static char out[REM_MAX * 200];
    size_t n = 0;
    for (int i = 0; i < nrem; i++) n += (size_t)snprintf(out + n, sizeof(out) - n, "%lld %s\n", rem[i].due, rem[i].text);
    (void)icda_write_file(REM_PATH, out, n);
}

static void rem_load(void) {
    static char in[REM_MAX * 200];
    long n = (long)icda_read_file(REM_PATH, in, sizeof(in) - 1);
    nrem = 0;
    if (n <= 0) return;
    in[n] = 0;
    for (char *p = in; *p && nrem < REM_MAX;) {
        char *e = strchr(p, '\n'), *sp;
        if (e) *e = 0;
        sp = strchr(p, ' ');
        if (sp) {
            rem[nrem].due = strtoll(p, 0, 10);
            snprintf(rem[nrem].text, sizeof(rem[nrem].text), "%s", sp + 1);
            nrem++;
        }
        if (!e) break;
        p = e + 1;
    }
}

/* due reminders become notifications (with their sound), even with the overlay closed */
static void rem_check(void) {
    static uint64_t last;
    long long now;
    int fired = 0;
    if (last && icda_ticks() - last < 100) return;      /* once a second at most */
    last = icda_ticks();
    now = now_local();
    for (int i = 0; i < nrem; i++) {
        if (now && rem[i].due <= now) {
            char msg[220];
            int k = snprintf(msg, sizeof(msg), "Assistant|Reminder\n%s", rem[i].text);
            (void)icda_write_file("/dev/notify", msg, (uint64_t)k);
            {
                char spoken[200];
                snprintf(spoken, sizeof(spoken), "Reminder. %s", rem[i].text);
                say(spoken);
            }
            rem[i] = rem[--nrem];
            i--;
            fired = 1;
        }
    }
    if (fired) rem_save();
}

static const char *after_word(const char *s, const char *w) {
    size_t n = strlen(w);
    while (*s == ' ') s++;
    return strncmp(s, w, n) == 0 ? s + n : 0;
}

static int number_word(const char **p) {
    static const char *const nums[] = { "a ", "an ", "one ", "two ", "three ", "four ", "five ", "ten ", "fifteen ",
                                        "twenty ", "thirty " };
    static const int vals[] = { 1, 1, 1, 2, 3, 4, 5, 10, 15, 20, 30 };
    while (**p == ' ') (*p)++;
    if (**p >= '0' && **p <= '9') {
        int v = 0;
        while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
        return v;
    }
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
        if (strncmp(*p, nums[i], strlen(nums[i])) == 0) {
            *p += strlen(nums[i]) - 1;
            return vals[i];
        }
    return -1;
}

/* "remind me in 10 minutes to ...", "remind me at 18:30 to ...", "set a timer for 5 minutes":
 * 1 with the reminder scheduled and a confirmation in answer */
static int try_reminder(const char *q) {
    char low[Q_MAX], when[40];
    const char *p, *what;
    long long now = now_local(), due = 0;
    int timer = 0;
    size_t i;
    for (i = 0; q[i] && i + 1 < sizeof(low); i++) low[i] = (char)(q[i] >= 'A' && q[i] <= 'Z' ? q[i] + 32 : q[i]);
    low[i] = 0;
    if (!now) return 0;
    if ((p = strstr(low, "remind me")) != 0) p += 9;
    else if ((p = strstr(low, "timer")) != 0) { p += 5; timer = 1; }
    else return 0;
    if (timer) { const char *f = after_word(p, "for"); if (f) p = f; }
    {
        const char *in = after_word(p, "in");
        const char *at = after_word(p, "at");
        if (!in) in = after_word(p, "after");
        if (in || timer) {
            int v;
            const char *u = in ? in : p;
            v = number_word(&u);
            while (*u == ' ') u++;
            if (v < 0) return 0;
            if (!strncmp(u, "sec", 3)) due = now + v;
            else if (!strncmp(u, "min", 3)) due = now + (long long)v * 60;
            else if (!strncmp(u, "hour", 4) || !strncmp(u, "hr", 2)) due = now + (long long)v * 3600;
            else return 0;
            while (*u && *u != ' ') u++;
            p = u;
        } else if (at) {
            int h = 0, m = 0;
            const char *u = at;
            while (*u == ' ') u++;
            if (*u < '0' || *u > '9') return 0;
            while (*u >= '0' && *u <= '9') h = h * 10 + (*u++ - '0');
            if (*u == ':' || *u == '.') { u++; while (*u >= '0' && *u <= '9') m = m * 10 + (*u++ - '0'); }
            while (*u == ' ') u++;
            if (!strncmp(u, "pm", 2) && h < 12) h += 12;
            if (!strncmp(u, "am", 2) && h == 12) h = 0;
            if (!strncmp(u, "am", 2) || !strncmp(u, "pm", 2)) u += 2;
            if (h > 23 || m > 59) return 0;
            due = now - now % 86400 + h * 3600 + m * 60;
            if (due <= now) due += 86400;                       /* that time tomorrow */
            p = u;
        } else {
            return 0;
        }
    }
    /* what to be reminded of: the rest, in the user's own capitals */
    what = q + (p - low);
    while (*what == ' ' || *what == ',') what++;
    if (!strncmp(what, "to ", 3)) what += 3;
    else if (!strncmp(what, "about ", 6)) what += 6;
    else if (!strncmp(what, "that ", 5)) what += 5;
    if (nrem >= REM_MAX) {
        snprintf(answer, sizeof(answer), "There are already %d reminders waiting.", REM_MAX);
        return 1;
    }
    rem[nrem].due = due;
    snprintf(rem[nrem].text, sizeof(rem[nrem].text), "%s", *what ? what : timer ? "Your timer is done" : "Reminder");
    nrem++;
    rem_save();
    /* "at 21:03" or "tomorrow at 07:30": short, and it reads aloud well */
    fmt_local(due, when, sizeof(when));
    snprintf(answer, sizeof(answer), "%s set for %sat %s%s%s.", timer ? "Timer" : "Reminder",
             due / 86400 != now / 86400 ? "tomorrow " : "", strrchr(when, ' ') + 1, *what ? ": " : "", what);
    return 1;
}

/* ---- web search ------------------------------------------------------------------ */

static void strip_tags(const char *s, size_t n, char *out, size_t cap) {
    size_t k = 0;
    int tag = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        if (s[i] == '<') tag = 1;
        else if (s[i] == '>') tag = 0;
        else if (!tag) {
            if (s[i] == '&' && !strncmp(s + i, "&amp;", 5)) { out[k++] = '&'; i += 4; }
            else if (s[i] == '&' && !strncmp(s + i, "&#x27;", 6)) { out[k++] = '\''; i += 5; }
            else if (s[i] == '&' && !strncmp(s + i, "&quot;", 6)) { out[k++] = '"'; i += 5; }
            else if (s[i] != '\n') out[k++] = s[i];
        }
    }
    out[k] = 0;
}

/* the top results for query from DuckDuckGo's plain HTML page, as numbered lines
 * (it answers some requests with a check for robots instead: 0 then) */
static int ddg_search(const char *query, char *out, size_t cap) {
    char url[700], final[700];
    size_t k = 0, u = 0;
    int found = 0;
    http_req_t *r;
    char *page;
    u = (size_t)snprintf(url, sizeof(url), "https://html.duckduckgo.com/html/?q=");
    for (const char *q = query; *q && u + 4 < sizeof(url); q++) {
        unsigned char c = (unsigned char)*q;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.') url[u++] = (char)c;
        else if (c == ' ') url[u++] = '+';
        else u += (size_t)snprintf(url + u, sizeof(url) - u, "%%%02X", c);
    }
    url[u] = 0;
    r = http_get(url, final, sizeof(final));
    out[0] = 0;
    if (!r || r->status != 200 || !r->body) {
        if (r) http_free(r);
        return 0;
    }
    page = (char *)malloc(r->body_len + 1);
    if (!page) { http_free(r); return 0; }
    memcpy(page, r->body, r->body_len);
    page[r->body_len] = 0;
    http_free(r);
    for (char *p = page; found < 4 && (p = strstr(p, "class=\"result__a\"")) != 0;) {
        char title[160], snip[240], link[200];   /* short: a local model reads every word */
        char *te = strstr(p, "</a>"), *href, *s;
        link[0] = 0;
        href = strstr(p, "href=\"");
        if (href && href < te) {
            char *e = strchr(href + 6, '"');
            char *ud = strstr(href, "uddg=");
            size_t lk = 0;
            if (ud && e && ud < e) {             /* /l/?uddg=<encoded target> */
                for (char *c = ud + 5; c < e && *c != '&' && lk + 1 < sizeof(link); c++) {
                    if (*c == '%' && c[1] && c[2]) {
                        char hx[3] = { c[1], c[2], 0 };
                        link[lk++] = (char)strtol(hx, 0, 16);
                        c += 2;
                    } else link[lk++] = *c;
                }
            }
            link[lk] = 0;
        }
        if (!te) break;
        strip_tags(strchr(p, '>') + 1, (size_t)(te - strchr(p, '>') - 1), title, sizeof(title));
        s = strstr(te, "class=\"result__snippet\"");
        snip[0] = 0;
        if (s) {
            char *se = strstr(s, "</a>");
            if (se) strip_tags(strchr(s, '>') + 1, (size_t)(se - strchr(s, '>') - 1), snip, sizeof(snip));
        }
        found++;
        k += (size_t)snprintf(out + k, cap - k, "%d. %s - %s (%s)\n", found, title, snip, link);
        if (k >= cap) break;
        p = te;
    }
    free(page);
    return found;
}

static void url_encode(const char *s, char *out, size_t cap) {
    size_t u = 0;
    for (; *s && u + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') out[u++] = (char)c;
        else u += (size_t)snprintf(out + u, cap - u, "%%%02X", c);
    }
    out[u] = 0;
}

/* the body of a GET (following redirects), 0 unless it answered 200 */
static char *fetch(const char *url, const char *headers, size_t *len) {
    http_req_t *r = http_open_follow(url, headers);
    char *body = 0;
    size_t n = 0, cap = 0;
    uint64_t t0 = icda_ticks();
    int st = HTTP_PENDING;
    if (!r) return 0;
    while (st == HTTP_PENDING && icda_ticks() - t0 < 2000) {      /* 20 s */
        st = http_poll(r, 200);
        if (r->headers_done && r->status != 200) break;
        if (r->body_len) {
            if (n + r->body_len + 1 > cap) {
                size_t want = (n + r->body_len + 1) * 2;
                char *b = (char *)realloc(body, want);
                if (!b) break;
                body = b;
                cap = want;
            }
            memcpy(body + n, r->body, r->body_len);
            n += r->body_len;
            r->body_len = 0;
        }
        if (n > 1 << 20) break;                                      /* enough */
    }
    if (r->status != 200 || !body) {
        free(body);
        http_free(r);
        return 0;
    }
    http_free(r);
    body[n] = 0;
    *len = n;
    return body;
}

/* the next "key": "string" at or after p, unescaped into out; the place after it */
static const char *next_str(const char *p, const char *key, char *out, size_t cap) {
    char pat[40];
    size_t n = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = 0;
    if (!p || !(p = strstr(p, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return p;
    return json_unescape(p + 1, out, cap, &n);
}

static int brave_search(const char *query, char *out, size_t cap) {
    char url[700], q[600], headers[260], title[200], link[300], desc[400], plain[400];
    size_t len = 0, k = 0;
    int found = 0;
    char *body;
    const char *p;
    url_encode(query, q, sizeof(q));
    snprintf(url, sizeof(url), "https://api.search.brave.com/res/v1/web/search?count=5&q=%s", q);
    snprintf(headers, sizeof(headers), "Accept: application/json\r\nX-Subscription-Token: %s\r\n", search_key);
    body = fetch(url, headers, &len);
    if (!body) return 0;
    p = strstr(body, "\"web\"");
    while (p && found < 5 && (p = strstr(p, "\"title\"")) != 0) {
        p = next_str(p, "title", title, sizeof(title));
        p = next_str(p, "url", link, sizeof(link));
        p = next_str(p, "description", desc, sizeof(desc));
        if (!p) break;
        strip_tags(desc, strlen(desc), plain, sizeof(plain));
        found++;
        k += (size_t)snprintf(out + k, cap - k, "%d. %s - %s (%s)\n", found, title, plain, link);
        if (k >= cap) break;
    }
    free(body);
    return found;
}

/* Wikipedia: the pages that match, and the start of the best one - not news,
 * but it always answers, with no key */
static int wiki_search(const char *query, char *out, size_t cap) {
    char url[700], q[600], title[200], snip[400], plain[400], first[200];
    size_t len = 0, k = 0;
    int found = 0;
    char *body;
    const char *p;
    url_encode(query, q, sizeof(q));
    snprintf(url, sizeof(url), "https://en.wikipedia.org/w/api.php?action=query&list=search&srlimit=3&format=json&srsearch=%s", q);
    body = fetch(url, 0, &len);
    if (!body) return 0;
    first[0] = 0;
    p = strstr(body, "\"search\"");
    while (p && found < 3 && (p = strstr(p, "\"title\"")) != 0) {
        p = next_str(p, "title", title, sizeof(title));
        p = next_str(p, "snippet", snip, sizeof(snip));
        if (!p) break;
        strip_tags(snip, strlen(snip), plain, sizeof(plain));
        if (!found) snprintf(first, sizeof(first), "%s", title);
        found++;
        k += (size_t)snprintf(out + k, cap - k, "%d. Wikipedia: %s - %s\n", found, title, plain);
        if (k >= cap) break;
    }
    free(body);
    if (first[0] && k < cap) {
        static char text[1400];
        url_encode(first, q, sizeof(q));
        snprintf(url, sizeof(url), "https://en.wikipedia.org/w/api.php?action=query&prop=extracts&exintro=1"
                 "&explaintext=1&exchars=1000&format=json&titles=%s", q);
        body = fetch(url, 0, &len);
        if (body && next_str(strstr(body, "\"pages\""), "extract", text, sizeof(text)) && text[0])
            k += (size_t)snprintf(out + k, cap - k, "\n%s:\n%s\n", first, text);
        free(body);
    }
    return found;
}

/* the web's answer to query, from the best source that answers */
static int web_search(const char *query, char *out, size_t cap) {
    out[0] = 0;
    if (search_key[0] && brave_search(query, out, cap)) return 1;
    if (ddg_search(query, out, cap)) return 1;
    if (wiki_search(query, out, cap)) return 1;
    return 0;
}

/* questions that need the web: "search ...", "look up ...", news, prices ... */
static int wants_web(const char *q, char *query, size_t cap) {
    static const char *const lead[] = { "search the web for ", "search for ", "search ", "look up ", "google ", "/web " };
    static const char *const hint[] = { "latest", "news", "today's", "this week", "current price", "price of",
                                        "weather", "who won", "score", "release date" };
    char low[Q_MAX];
    size_t i;
    for (i = 0; q[i] && i + 1 < sizeof(low); i++) low[i] = (char)(q[i] >= 'A' && q[i] <= 'Z' ? q[i] + 32 : q[i]);
    low[i] = 0;
    for (i = 0; i < sizeof(lead) / sizeof(lead[0]); i++)
        if (!strncmp(low, lead[i], strlen(lead[i]))) {
            snprintf(query, cap, "%s", q + strlen(lead[i]));
            return 1;
        }
    for (i = 0; i < sizeof(hint) / sizeof(hint[0]); i++)
        if (strstr(low, hint[i])) {
            snprintf(query, cap, "%s", q);
            return 1;
        }
    return 0;
}

/* ---- the local model (icda-llm, through files) --------------------------------- */

#define LREQ   "/tmp/ai/local-request"
#define LREPLY "/tmp/ai/local-reply"

static int llm_started;
static uint64_t llm_pid;
static char llm_model[256];
static uint32_t lseq;

/* Only ICDA system partitions are eligible; never search Windows for AI. */
static int find_system_file(const char *rel, char *out, size_t cap) {
    icda_stat_t st;
    char marker[128];
    snprintf(out, cap, "/linux/%s", rel);
    if ((long)icda_stat(out, &st) >= 0) return 1;
    for (int i = 0; i < 32; i++) {
        snprintf(marker, sizeof(marker), "/volumes/fat32-%d/ICDACFG.TXT", i);
        if ((long)icda_stat(marker, &st) < 0) continue;
        snprintf(out, cap, "/volumes/fat32-%d/linux/%s", i, rel);
        if ((long)icda_stat(out, &st) >= 0) return 1;
    }
    return 0;
}

static void llm_stop(void) {
    if (llm_started) (void)icda_kill(llm_pid, 1);
    llm_started = 0;
    llm_pid = 0;
    llm_model[0] = 0;
}

/* ---- listening (the microphone and icda-stt, Whisper) --------------------------- */

#define MIC_RATE    16000
#define MIC_MAX_S   15

static int16_t heard_pcm[MIC_RATE * MIC_MAX_S];

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* records until a pause after speech (or 15 s); the wave follows the voice
 * through "listening" replies whose provider field is the level 0..99.
 * Returns the samples kept (16 kHz mono), 0 when nothing was said. */
static int record(void) {
    static uint8_t raw[65536];
    int n = 0, spoke = 0, quiet_ms = 0, elapsed = 0;
    long floor_sum = 0, floor_n = 0;
    char level[8];
    if (icda_mic_start() < 0) return -1;
    while (elapsed < MIC_MAX_S * 1000) {
        long got = icda_mic_read(raw, sizeof(raw));
        long long sq = 0;
        int frames = got > 0 ? (int)(got / 4) : 0, block = 0;
        /* 48 kHz stereo -> 16 kHz mono: every third frame, both channels */
        for (int f = 0; f + 2 < frames && n < MIC_RATE * MIC_MAX_S; f += 3) {
            const int16_t *s = (const int16_t *)raw + f * 2;
            int v = (s[0] + s[1] + s[2] + s[3] + s[4] + s[5]) / 6;
            heard_pcm[n++] = (int16_t)v;
            sq += (long long)v * v;
            block++;
        }
        if (block) {
            int rms = 0;
            long long m = sq / block;
            while ((long long)(rms + 1) * (rms + 1) <= m) rms++;
            if (elapsed < 300) { floor_sum += rms; floor_n++; }         /* the room, before speaking */
            else {
                int fl = floor_n ? (int)(floor_sum / floor_n) : 0;
                if (rms > fl * 3 + 300) { spoke = 1; quiet_ms = 0; }
                else if (spoke) quiet_ms += 50;
            }
            snprintf(level, sizeof(level), "%d", rms * 99 / 6000 > 99 ? 99 : rms * 99 / 6000);
            cur_name = level;
            reply("listening");
        }
        if (spoke && quiet_ms >= 1200) break;            /* a pause: the question is over */
        if (!spoke && elapsed >= 6000) break;            /* nothing said */
        icda_sleep(5);
        elapsed += 50;
    }
    icda_mic_stop();
    cur_name = "";
    return spoke ? n : 0;
}

/* Speech assets use the same ICDA-owned storage as the chat model. */
static int transcribe(int samples, char *out, size_t cap, char *err, size_t errcap) {
    static uint8_t wav[44 + sizeof(heard_pcm)];
    char model[256], bin[256], args[600];
    icda_stat_t st;
    uint64_t pid, t0;
    long n;
    if (!find_system_file("usr/share/icda/models/ggml-base.en.bin", model, sizeof(model))) {
        snprintf(err, errcap, "No speech model: install ggml-base.en.bin in ICDA's linux/usr/share/icda/models directory.");
        return -1;
    }
    if (!find_system_file("usr/bin/icda-stt", bin, sizeof(bin))) {
        snprintf(err, errcap, "icda-stt is not installed (it comes with system updates).");
        return -1;
    }
    memcpy(wav, "RIFF", 4); put_le(wav + 4, 36 + (uint32_t)samples * 2, 4); memcpy(wav + 8, "WAVEfmt ", 8);
    put_le(wav + 16, 16, 4); put_le(wav + 20, 1, 2); put_le(wav + 22, 1, 2); put_le(wav + 24, MIC_RATE, 4);
    put_le(wav + 28, MIC_RATE * 2, 4); put_le(wav + 32, 2, 2); put_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4); put_le(wav + 40, (uint32_t)samples * 2, 4);
    memcpy(wav + 44, heard_pcm, (size_t)samples * 2);
    (void)icda_write_file("/tmp/ai/heard.wav", (const char *)wav, 44 + (uint64_t)samples * 2);
    (void)icda_remove("/tmp/ai/heard.txt");
    snprintf(args, sizeof(args), "%s /tmp/ai/heard.wav /tmp/ai/heard.txt", model);
    pid = icda_spawn_args(bin, args);
    if ((long)pid <= 0) {
        snprintf(err, errcap, "icda-stt did not start.");
        return -1;
    }
    t0 = icda_ticks();
    while ((long)icda_stat("/tmp/ai/heard.txt", &st) < 0) {      /* renamed into place when done */
        if (icda_ticks() - t0 > 6000) {
            snprintf(err, errcap, "Speech recognition took too long.");
            return -1;
        }
        rem_check();
        icda_sleep(5);
    }
    n = (long)icda_read_file("/tmp/ai/heard.txt", out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    {   /* trim */
        char *s = out, *e;
        while (*s == ' ' || *s == '\n') s++;
        memmove(out, s, strlen(s) + 1);
        e = out + strlen(out);
        while (e > out && (e[-1] == ' ' || e[-1] == '\n')) *--e = 0;
    }
    if (!out[0] || !strcmp(out, "[BLANK_AUDIO]")) {
        snprintf(err, errcap, "I didn't catch that.");
        return -1;
    }
    return 0;
}

/* ---- the voice (icda-say, Flite) ------------------------------------------------ */

/* reads text aloud: icda-say writes /tmp/ai/say.wav, the mixer plays it while
 * the next question can already be asked.  Long answers are cut at a sentence
 * end near 700 characters. */
static void say(const char *text) {
    static char bin[256], clip[800];
    size_t k = strlen(text);
    uint64_t pid;
    if (!speak || !k) return;
    if (!bin[0] && !find_system_file("usr/bin/icda-say", bin, sizeof(bin))) return;
    if (k > 700) {
        size_t cut = 700;
        for (size_t i = 700; i > 200; i--)
            if (text[i] == '.' || text[i] == '!' || text[i] == '?') { cut = i + 1; break; }
        k = cut;
    }
    memcpy(clip, text, k);
    clip[k] = 0;
    (void)icda_write_file("/tmp/ai/say.txt", clip, k);
    (void)icda_remove("/tmp/ai/say.wav");
    pid = icda_spawn_args(bin, "/tmp/ai/say.txt /tmp/ai/say.wav");
    if ((long)pid <= 0) {
        bin[0] = 0;
        return;
    }
    {   /* icda-say renames the file into place when it is whole */
        icda_stat_t st;
        uint64_t t0 = icda_ticks();
        while ((long)icda_stat("/tmp/ai/say.wav", &st) < 0) {
            if (icda_ticks() - t0 > 3000) return;      /* 30 s: give up */
            icda_sleep(5);
        }
    }
    (void)icda_audio_effect("/tmp/ai/say.wav", 230);
}

/* file:auto selects the Model field inside ICDA's own model directory. */
static int llm_start(const provider_t *v, char *err, size_t errcap) {
    char model[256], bin[256], args[320], rel[160];
    if (strcmp(v->url, "file:auto") == 0) {
        const char *name = strcmp(v->model, "local") == 0 ?
            "qwen2.5-0.5b-instruct-q4_k_m.gguf" : v->model;
        if (!name[0] || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
            snprintf(err, errcap, "%s: choose a GGUF filename in Settings > Assistant", v->name);
            return -1;
        }
        snprintf(rel, sizeof(rel), "usr/share/icda/models/%s", name);
        if (!find_system_file(rel, model, sizeof(model))) {
            snprintf(err, errcap, "%s: %s is not installed in ICDA. Download it in Settings > Assistant, or use /model install 4b (or 0.5b, 1.5b, 3b).", v->name, name);
            return -1;
        }
    } else {
        /* Explicit paths still stay in ICDA, rather than mounted foreign drives. */
        if (strncmp(v->url, "file:/linux/usr/share/icda/models/", strlen("file:/linux/usr/share/icda/models/")) != 0 ||
            strstr(v->url, "..")) {
            snprintf(err, errcap, "%s: use file:auto and an ICDA model filename", v->name);
            return -1;
        }
        snprintf(rel, sizeof(rel), "%s", v->url + 12);
        if (!find_system_file(rel, model, sizeof(model))) {
            snprintf(err, errcap, "%s: the selected ICDA model is not installed", v->name);
            return -1;
        }
    }
    if (llm_started && strcmp(llm_model, model) == 0) {
        icda_proc_info_t info;
        if ((long)icda_proc_info(llm_pid, &info) >= 0 && info.state < 5) return 0;
    }
    llm_stop();
    if (!find_system_file("usr/bin/icda-llm", bin, sizeof(bin))) {
        snprintf(err, errcap, "%s: icda-llm is not installed (it comes with system updates)", v->name);
        return -1;
    }
    {
        /* one thread per core: a laptop's second threads per core (hyper-
         * threading) share the core's arithmetic and only add contention */
        icda_sys_stats_t st;
        int threads = 1;
        if ((long)icda_sys_stats(&st) >= 0 && st.cpus > 1) threads = (int)(st.cpus >= 4 ? st.cpus / 2 : st.cpus);
        if (threads > 8) threads = 8;
        if (threads_conf > 0 && threads_conf <= 16) threads = threads_conf;
        /* room for the guide, the tools and a few steps of a task */
        snprintf(args, sizeof(args), "-m %s -t %d -c 6144", model, threads);
    }
    (void)icda_write_file(LREPLY, "", 0);
    (void)icda_write_file(LREQ, "", 0);
    {
        char note[700];
        int nn = snprintf(note, sizeof(note), "starting %s %s\n", bin, args);
        (void)icda_write_file("/tmp/ai/log", note, (uint64_t)nn);
        /* the same on the serial port: if the local model takes the machine
         * down, this is the only record of what it was asked to load */
        (void)icda_write_file("/dev/serial", note, strlen(note));
    }
    long pid = (long)icda_spawn_args(bin, args);
    if (pid < 0) {
        snprintf(err, errcap, "%s: icda-llm did not start", v->name);
        return -1;
    }
    llm_started = 1;
    llm_pid = (uint64_t)pid;
    snprintf(llm_model, sizeof(llm_model), "%s", model);
    return 0;
}

static void lput(char *out, size_t cap, size_t *n, const char *role, const char *text) {
    put(out, cap, n, role);
    if (*n + 1 < cap) out[(*n)++] = '\x1f';
    for (const char *t = text; *t && *n + 2 < cap; t++) if (*t != '\x1e' && *t != '\x1f') out[(*n)++] = *t;
    if (*n + 1 < cap) out[(*n)++] = '\x1e';
    out[*n] = 0;
}

/* How long to wait for the local model.
 *
 * The first question pays for loading the model, and a model big enough to be
 * worth running is a couple of gigabytes: reading that off the disk and
 * setting it up takes minutes, not the seconds a spinner would suggest.  A
 * short wait meant every first question came back "did not answer in time"
 * while the model was still loading perfectly well behind it - and because the
 * daemon had already given up, nothing it was doing was ever seen.
 *
 * So the wait is generous, and it says what it is waiting for. */
#define LLM_LOAD_MS   900000          /* 15 minutes: the first question */
#define LLM_ANSWER_MS  120000         /* 2 minutes: once it is loaded */

static int ask_local(const provider_t *v, const char *q, char *err, size_t errcap) {
    static char req[200000], rep[A_MAX + 64], asked[Q_MAX + 4200];
    size_t n;
    uint64_t t0 = icda_ticks(), last = 0, load_noted = 0, moved = 0;
    size_t had = 0;
    if (llm_start(v, err, errcap)) return -1;
    lseq = lseq ? lseq + 1 : (uint32_t)icda_ticks();
    if (!lseq) lseq = 1;
    n = (size_t)snprintf(req, sizeof(req), "%u\n", lseq);
    lput(req, sizeof(req), &n, "system", sys_prompt);
    /* a small context: the last few answers, shortened */
    for (int i = nhist > 3 ? nhist - 3 : 0; i < nhist; i++) {
        char a[700];
        snprintf(a, sizeof(a), "%s", hist[i].a);
        lput(req, sizeof(req), &n, "user", hist[i].q);
        lput(req, sizeof(req), &n, "assistant", a);
    }
    dated(q, asked, sizeof(asked));
    lput(req, sizeof(req), &n, "user", asked);
    for (int i = 0; i < nsteps; i++) lput(req, sizeof(req), &n, step_role[i], step_text[i]);
    if ((long)icda_write_file(LREQ, req, n) < 0) {
        snprintf(err, errcap, "%s: could not write the local request", v->name);
        return -1;
    }
    answer[0] = 0;
    for (;;) {
        uint64_t waited = (icda_ticks() - t0) * 10;
        icda_proc_info_t info;
        if ((long)icda_proc_info(llm_pid, &info) < 0 || info.state >= 5) {
            /* Read a startup error before falling back to the exit status. */
            long got = (long)icda_read_file(LREPLY, rep, sizeof(rep) - 1);
            if (got > 0) rep[got] = 0;
            snprintf(err, errcap, "%s: %s", v->name,
                     got > 8 && strncmp(rep, "0 error\n", 8) == 0 ? rep + 8 :
                     "The local model stopped. See /tmp/ai/llm.log for details.");
            llm_stop();
            return -1;
        }
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
                llm_stop();
                return -1;
            }
            if (s == lseq) {
                snprintf(answer, sizeof(answer), "%s", p);
                if (strlen(answer) != had) {          /* it is still writing */
                    had = strlen(answer);
                    moved = icda_ticks();
                }
                if (strcmp(state, "done") == 0) {
                    (void)icda_write_file("/dev/serial", "ai: local model finished\n", sizeof("ai: local model finished\n") - 1);
                    if (!answer[0]) snprintf(err, errcap, "%s: the model returned an empty answer", v->name);
                    return answer[0] ? 0 : -1;
                }
                if (strcmp(state, "error") == 0) {
                    (void)icda_write_file("/dev/serial", "ai: local model error\n", sizeof("ai: local model error\n") - 1);
                    snprintf(err, errcap, "%s: %s", v->name, p);
                    return -1;
                }
                if (icda_ticks() - last >= 10) {
                    last = icda_ticks();
                    reply(answer[0] ? "answer" : "thinking");
                }
            }
        }
        /* the first question waits for the model to load; once it writes, the
         * wait is for the next words - a long load must not count against it */
        if (answer[0] ? (icda_ticks() - moved) * 10 > (uint64_t)LLM_ANSWER_MS : waited > (uint64_t)LLM_LOAD_MS) {
            snprintf(err, errcap, "%s: the local model did not answer in %d minutes. "
                     "The first question loads the model, which can take a while on a large one; "
                     "ask again once it has finished.", v->name, (int)(waited / 60000));
            llm_stop();
            if (answer[0] && strlen(answer) + 40 < sizeof(answer)) strcat(answer, " ... (the model stopped answering)");
            return answer[0] ? 0 : -1;
        }
        /* say what the silence is: the model is loading, not stuck */
        if (!answer[0] && !load_noted && waited > 3000) {
            load_noted = 1;
            reply("loading");
            (void)icda_write_file("/dev/serial", "ai: waiting for the local model to load\n", sizeof("ai: waiting for the local model to load\n") - 1);
        }
        rem_check();                     /* a long answer must not hold up a reminder */
        icda_sleep(5);
    }
}

/* ---- tools: what the assistant can do on the computer ---------------------------- */

/* the value of "key" in a JSON object: a string, unescaped, or a bare number */
static int json_field(const char *obj, const char *key, char *out, size_t cap) {
    char pat[40];
    const char *p;
    size_t n = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = 0;
    if (!obj || !(p = strstr(obj, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\n') p++;
    if (*p == '"') {
        json_unescape(p + 1, out, cap, &n);
        return 1;
    }
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

/* the tool call in a model's reply - "<tool_call>{...}</tool_call>", or the
 * bare object small models tend to write - or 0 for a final answer */
static const char *tool_call(const char *a) {
    const char *t = strstr(a, "<tool_call>");
    if (t) return strchr(t, '{');
    while (*a == ' ' || *a == '\n' || *a == '\r') a++;
    if (!strncmp(a, "```", 3)) a = strchr(a, '{');
    if (a && *a == '{' && strstr(a, "\"name\"") && strstr(a, "\"arguments\"")) return a;
    return 0;
}

/* a path the person means: their own home unless it says otherwise */
static int tool_path(const char *in, char *out, size_t cap) {
    while (*in == ' ') in++;
    if (!*in || strstr(in, "..")) return -1;
    if (*in == '~') in++;
    if (*in == '/') snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "/home/%s", in);
    return 0;
}

static int may_write(const char *p) {
    return !strncmp(p, "/home/", 6) || !strncmp(p, "/tmp/", 5) || !strncmp(p, "/volumes/", 9);
}

/* the folders above path, made if missing */
static void make_parents(const char *path) {
    char d[400];
    snprintf(d, sizeof(d), "%s", path);
    for (char *s = d + 1; *s; s++)
        if (*s == '/') { *s = 0; (void)icda_mkdir(d); *s = '/'; }
}

/* the text of a web page, without its markup, scripts and styles */
static void page_text(const char *html, size_t len, char *out, size_t cap) {
    size_t k = 0;
    int space = 0;
    for (size_t i = 0; i < len && k + 2 < cap; i++) {
        if (html[i] == '<') {
            const char *skip = 0;
            if (!strncmp(html + i, "<script", 7)) skip = "</script>";
            else if (!strncmp(html + i, "<style", 6)) skip = "</style>";
            else if (!strncmp(html + i, "<head", 5)) skip = "</head>";
            if (skip) {
                const char *e = strstr(html + i, skip);
                if (!e) break;
                i = (size_t)(e - html) + strlen(skip) - 1;
                continue;
            }
            while (i < len && html[i] != '>') i++;
            space = 1;
            continue;
        }
        char c = html[i];
        if (c == '&') {
            if (!strncmp(html + i, "&amp;", 5)) { c = '&'; i += 4; }
            else if (!strncmp(html + i, "&nbsp;", 6)) { c = ' '; i += 5; }
            else if (!strncmp(html + i, "&quot;", 6)) { c = '"'; i += 5; }
            else if (!strncmp(html + i, "&#39;", 5)) { c = '\''; i += 4; }
            else if (!strncmp(html + i, "&lt;", 4)) { c = '<'; i += 3; }
            else if (!strncmp(html + i, "&gt;", 4)) { c = '>'; i += 3; }
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { space = 1; continue; }
        if (space && k) out[k++] = ' ';
        space = 0;
        out[k++] = c;
    }
    out[k] = 0;
}

static const struct { const char *name, *path; } tool_apps[] = {
    { "explorer", "/apps/desktop.app" }, { "files", "/apps/desktop.app" },
    { "terminal", "/apps/terminal.app" }, { "surfer", "/apps/browser.app" },
    { "browser", "/apps/browser.app" }, { "editor", "/apps/editor.app" },
    { "media", "/apps/media.app" }, { "disk utility", "/apps/diskman.app" },
    { "activity", "/apps/taskman.app" }, { "settings", "/apps/settings.app" },
};

/* the files read during this request: only those may be replaced whole */
#define SEEN_MAX 8
static char seen[SEEN_MAX][400];
static int nseen;

static int was_read(const char *path) {
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], path)) return 1;
    return 0;
}

/* Runs the call: a short line for the overlay goes to status, what the model
 * gets back to out (kept short: a small model reads every word of it). */
static void run_tool(const char *call, char *status, size_t scap, char *out, size_t cap) {
    static char text[A_MAX];
    char name[32], a[400], b[400], path[400];
    const char *args = strstr(call, "\"arguments\"");
    json_field(call, "name", name, sizeof(name));
    if (!args) args = call;
    snprintf(status, scap, "%s", name);
    out[0] = 0;

    if (!strcmp(name, "list_folder")) {
        static char list[3000];
        json_field(args, "path", a, sizeof(a));
        if (tool_path(a[0] ? a : "/home", path, sizeof(path))) { snprintf(out, cap, "Not a usable path."); return; }
        snprintf(status, scap, "Looking in %s", path);
        long n = (long)icda_list_dir(path, list, sizeof(list) - 1);
        if (n < 0) { snprintf(out, cap, "%s cannot be opened (missing, or not allowed).", path); return; }
        list[n] = 0;
        snprintf(out, cap, "%s contains:\n%s", path, n ? list : "(nothing)\n");
    } else if (!strcmp(name, "read_file")) {
        icda_stat_t st;
        json_field(args, "path", a, sizeof(a));
        if (tool_path(a, path, sizeof(path))) { snprintf(out, cap, "Not a usable path."); return; }
        snprintf(status, scap, "Reading %s", path);
        if (!was_read(path) && nseen < SEEN_MAX) snprintf(seen[nseen++], sizeof(seen[0]), "%s", path);
        long n = (long)icda_read_file(path, text, 2400);
        if (n < 0) { snprintf(out, cap, "%s cannot be read (missing, or not allowed).", path); return; }
        text[n] = 0;
        if ((long)strlen(text) < n) { snprintf(out, cap, "%s is not a text file.", path); return; }
        (void)icda_stat(path, &st);
        snprintf(out, cap, "%s%s", text, st.size > (uint64_t)n ? "\n[... the rest of the file is not shown]" : "");
    } else if (!strcmp(name, "write_file") || !strcmp(name, "append_file")) {
        int append = name[0] == 'a';
        icda_stat_t st;
        json_field(args, "path", a, sizeof(a));
        json_field(args, "text", text, sizeof(text));
        if (tool_path(a, path, sizeof(path)) || !may_write(path)) {
            snprintf(out, cap, "Files can only be written in /home, /tmp or /volumes.");
            return;
        }
        if (!append && (long)icda_stat(path, &st) >= 0 && st.size && !was_read(path)) {
            /* a small model reaches for write_file to "add" to a list, and
             * the list was gone: what is there must have been read first */
            snprintf(out, cap, "%s already has text, which write_file would replace. To add to it use "
                     "append_file. To change it, read it with read_file first, then write all of the new text.", path);
            return;
        }
        snprintf(status, scap, "%s %s", append ? "Adding to" : "Writing", path);
        make_parents(path);
        if ((long)icda_stat(path, &st) >= 0 && !append && st.size) {
            /* the text it had is kept for this session, in case it was wanted */
            static char old[65536];
            long n = (long)icda_read_file(path, old, sizeof(old));
            const char *base = strrchr(path, '/') + 1;
            char keep[400];
            snprintf(keep, sizeof(keep), "/tmp/ai/previous/%s", base);
            (void)icda_mkdir("/tmp/ai/previous");
            if (n > 0) (void)icda_write_file(keep, old, (uint64_t)n);
        }
        long rc;
        if (append && (long)icda_stat(path, &st) >= 0) {
            /* a new line of its own: "milk" and "eggs" must not become "milkeggs" */
            char lastc = '\n';
            if (st.size && text[0] != '\n') (void)icda_read_file_at(path, st.size - 1, &lastc, 1);
            if (lastc != '\n' && (long)icda_write_file_at(path, st.size, "\n", 1) == 1) st.size++;
            rc = (long)icda_write_file_at(path, st.size, text, strlen(text));
        } else {
            rc = (long)icda_write_file(path, text, strlen(text));
        }
        snprintf(out, cap, rc < 0 ? "%s could not be written (not allowed, or the disk is full)." :
                 append ? "Added to %s." : "Saved %s.", path);
    } else if (!strcmp(name, "make_folder")) {
        json_field(args, "path", a, sizeof(a));
        if (tool_path(a, path, sizeof(path)) || !may_write(path)) {
            snprintf(out, cap, "Folders can only be made in /home, /tmp or /volumes.");
            return;
        }
        snprintf(status, scap, "Making %s", path);
        make_parents(path);
        icda_stat_t st;
        snprintf(out, cap, (long)icda_mkdir(path) >= 0 || (long)icda_stat(path, &st) >= 0 ?
                 "The folder %s is there." : "%s could not be made.", path);
    } else if (!strcmp(name, "move")) {
        char to[400];
        json_field(args, "from", a, sizeof(a));
        json_field(args, "to", b, sizeof(b));
        if (tool_path(a, path, sizeof(path)) || tool_path(b, to, sizeof(to)) || !may_write(path) || !may_write(to)) {
            snprintf(out, cap, "Only files in /home, /tmp or /volumes can be moved.");
            return;
        }
        snprintf(status, scap, "Moving %s", path);
        make_parents(to);
        snprintf(out, cap, icda_rename(path, to) < 0 ? "%s could not be moved to %s." : "Moved %s to %s.", path, to);
    } else if (!strcmp(name, "web_search")) {
        json_field(args, "query", a, sizeof(a));
        snprintf(status, scap, "Searching the web");
        if (!a[0] || !web_search(a, out, cap)) snprintf(out, cap, "The web search found nothing (or there is no internet).");
    } else if (!strcmp(name, "read_webpage")) {
        char final[700];
        http_req_t *r;
        json_field(args, "url", a, sizeof(a));
        if (strncmp(a, "http://", 7) && strncmp(a, "https://", 8)) { snprintf(out, cap, "Give a full web address."); return; }
        snprintf(status, scap, "Reading a web page");
        r = http_get(a, final, sizeof(final));
        if (!r || r->status != 200 || !r->body) {
            snprintf(out, cap, "The page did not load (HTTP %d).", r ? r->status : 0);
            if (r) http_free(r);
            return;
        }
        page_text((const char *)r->body, r->body_len, text, 2400);
        http_free(r);
        snprintf(out, cap, "%s", text[0] ? text : "The page has no readable text.");
    } else if (!strcmp(name, "open_app")) {
        char low[40], target[400];
        const char *app = 0;
        json_field(args, "app", a, sizeof(a));
        if (!a[0]) json_field(args, "name", a, sizeof(a));
        json_field(args, "target", target, sizeof(target));
        for (size_t i = 0; a[i] && i + 1 < sizeof(low); i++) { low[i] = (char)(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i]); low[i + 1] = 0; }
        for (size_t i = 0; a[0] && i < sizeof(tool_apps) / sizeof(tool_apps[0]); i++)
            if (!strcmp(low, tool_apps[i].name)) app = tool_apps[i].path;
        if (!app) { snprintf(out, cap, "There is no app called %s.", a); return; }
        if (target[0] && target[0] != 'h' && tool_path(target, path, sizeof(path)) == 0) snprintf(target, sizeof(target), "%s", path);
        snprintf(status, scap, "Opening %s", a);
        long pid = target[0] ? (long)icda_spawn_args(app, target) : (long)icda_spawn(app);
        snprintf(out, cap, pid < 0 ? "%s did not open." : "%s is open.", a);
    } else if (!strcmp(name, "remind")) {
        long long now = now_local();
        json_field(args, "minutes", a, sizeof(a));
        json_field(args, "text", b, sizeof(b));
        int m = atoi(a);
        if (m <= 0 || !now || nrem >= REM_MAX) { snprintf(out, cap, "The reminder could not be set."); return; }
        rem[nrem].due = now + (long long)m * 60;
        snprintf(rem[nrem].text, sizeof(rem[nrem].text), "%s", b[0] ? b : "Reminder");
        nrem++;
        rem_save();
        snprintf(status, scap, "Setting a reminder");
        snprintf(out, cap, "Reminder set for %d minutes from now.", m);
    } else if (!strcmp(name, "system_info")) {
        icda_sys_stats_t st;
        char rel[256], *ver;
        long n = (long)icda_read_file("/etc/icda-release.txt", rel, sizeof(rel) - 1);
        rel[n > 0 ? n : 0] = 0;
        ver = strstr(rel, "version ");
        if (ver) { ver += 8; for (char *e = ver; *e; e++) if (*e == '\n') { *e = 0; break; } }
        snprintf(status, scap, "Checking the system");
        if ((long)icda_sys_stats(&st) < 0) memset(&st, 0, sizeof(st));
        snprintf(out, cap, "ICDA %s; %llu processors; memory %llu MB, %llu MB free; running for %llu minutes.",
                 ver ? ver : "(version unknown)", (unsigned long long)st.cpus,
                 (unsigned long long)(st.mem_total >> 20), (unsigned long long)(st.mem_free >> 20),
                 (unsigned long long)(st.uptime_ticks / 6000));
    } else {
        snprintf(out, cap, "There is no tool called %s.", name);
    }
}

#define ROUNDS 6

/* A model at work: it answers, or asks for a tool, gets the result and goes
 * on - until it answers, or ROUNDS tools have run. */
static int work(const provider_t *v, const char *q, char *err, size_t errcap) {
    static char result[2600];
    static char status[120];
    nsteps = 0;
    nseen = 0;
    build_system(v);
    for (int round = 0;; round++) {
        const char *call;
        int rc = strncmp(v->url, "file:", 5) == 0 ? ask_local(v, q, err, errcap) : ask(v, q, err, errcap);
        if (rc) return rc;
        call = tool_call(answer);
        if (!call) return 0;                                 /* the answer */
        if (round == ROUNDS || nsteps + 2 > STEPS) {
            snprintf(answer, sizeof(answer), "I stopped after %d steps without finishing. Ask again, more precisely.", ROUNDS);
            return 0;
        }
        snprintf(step_role[nsteps], sizeof(step_role[0]), "assistant");
        snprintf(step_text[nsteps++], STEP_TEXT, "%s", answer);
        run_tool(call, status, sizeof(status), result, sizeof(result));
        cur_name = status;
        answer[0] = 0;
        reply("thinking");
        snprintf(step_role[nsteps], sizeof(step_role[0]), "user");
        snprintf(step_text[nsteps++], STEP_TEXT, "<tool_response>\n%s\n</tool_response>", result);
        cur_name = v->name;
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

static void remember_answer(const char *q) {
    if (nhist == HIST) {
        memmove(&hist[0], &hist[1], sizeof(hist[0]) * (HIST - 1));
        nhist--;
    }
    snprintf(hist[nhist].q, Q_MAX, "%s", q);
    snprintf(hist[nhist].a, A_MAX, "%s", answer);
    nhist++;
}

/* Model installation belongs to the assistant, independently of OTA. */
static int model_command(const char *q) {
    if (strncmp(q, "/model", 6) || (q[6] && q[6] != ' ')) return 0;
    q += 6; while (*q == ' ') q++;
    cur_name = "Qwen installer";
    if (!strncmp(q, "install", 7) && (!q[7] || q[7] == ' ')) {
        const char *name = q + 7; while (*name == ' ') name++;
        if (!*name) { name = icda_models[0].name; for (int i=0;i<nprov;i++) if (!prov[i].cloud) {name=prov[i].model;break;} }
        if (!strcmp(name, "0.5b")) name=icda_models[0].name;
        if (!strcmp(name, "1.5b")) name=icda_models[1].name;
        if (!strcmp(name, "3b")) name=icda_models[2].name;
        if (!strcmp(name, "4b")) name=icda_models[3].name;
        int valid=0; for(unsigned i=0;i<ICDA_MODEL_COUNT;i++) if(!strcmp(name,icda_models[i].name)) valid=1;
        if (!valid) { strcpy(answer,"Choose /model install 0.5b, 1.5b, 3b or 4b."); reply("error"); return 1; }
        long pid=(long)icda_spawn_args("/sbin/model-install",name);
        snprintf(answer,sizeof(answer),"%s",pid<0 ? "Could not start the Qwen installer." : "Downloading model from official Hugging Face. Use /model status or Settings > Assistant to follow progress. The download is verified before installation.");
        reply(pid<0 ? "error":"done");
    } else if (!strcmp(q,"status")) {
        long n=(long)icda_read_file("/tmp/ai/model-install.status",answer,sizeof(answer)-1);
        if(n>0) answer[n]=0; else strcpy(answer,"No model download yet. Use /model install 0.5b, 1.5b, 3b or 4b.");
        reply("done");
    } else {
        strcpy(answer,"Qwen downloads: /model install 0.5b (469 MiB), 1.5b (1066 MiB), 3b (2007 MiB) or 4b (2382 MiB, the best at using ICDA). Models come directly from official Hugging Face and are stored on ICDA. Use /model status for progress, or choose a model in Settings > Assistant."); reply("done");
    }
    return 1;
}
static void handle(const char *question) {
    char err[400], errs[800];
    static char spoken[Q_MAX];
    const char *q = question, *asked;
    if (!strcmp(question, "/listen")) {          /* a spoken question */
        cur_name = "";
        int samples;
        answer[0] = 0;
        samples = record();
        err[0] = 0;
        if (samples < 0) snprintf(err, sizeof(err), "No microphone was found.");
        else if (samples == 0) snprintf(err, sizeof(err), "I didn't hear anything.");
        if (err[0] || transcribe(samples, spoken, sizeof(spoken), err, sizeof(err)) != 0) {
            snprintf(answer, sizeof(answer), "%s", err);
            reply("error");
            return;
        }
        snprintf(answer, sizeof(answer), "%s", spoken);   /* the overlay shows what it heard */
        reply("heard");
        q = question = spoken;
    }
    static char augmented[Q_MAX + 4096];
    char query[Q_MAX];
    int cloud_first = complex_question(&q);
    load_conf();
    errs[0] = 0;
    cur_name = "";
    answer[0] = 0;
    reply("thinking");
    if (model_command(q)) return;
    /* reminders and timers need no model */
    if (try_reminder(q)) {
        cur_name = "reminders";
        reply("done");
        say(answer);
        return;
    }
    /* the web: the top results go to the model with the question */
    asked = q;
    if (wants_web(q, query, sizeof(query))) {
        static char results[3000];
        cur_name = "web search";
        reply("thinking");
        if (web_search(query, results, sizeof(results))) {
            snprintf(augmented, sizeof(augmented),
                     "Web search results for \"%s\":\n%s\nUsing these results, answer: %s\n"
                     "Mention which results you used.", query, results, q);
            asked = augmented;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        int want_cloud = pass == 0 ? cloud_first : !cloud_first;
        for (int i = 0; i < nprov; i++) {
            if (prov[i].cloud != want_cloud) continue;
            cur_name = prov[i].name;
            reply("thinking");
            if (work(&prov[i], asked, err, sizeof(err)) == 0) {
                /* remembered for the conversation */
                remember_answer(q);
                cur_name = prov[i].name;
                reply("done");
                say(answer);
                return;
            }
            if (strlen(errs) + strlen(err) + 2 < sizeof(errs)) {
                strcat(errs, err);
                strcat(errs, "\n");
            }
            /* tools already ran: another model would do it all again */
            if (nsteps) {
                pass = 2;
                break;
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
    {   /* Skip completed replies only; a request arriving during startup must run. */
        long n = (long)icda_read_file(REPLY_PATH, buf, sizeof(buf) - 1);
        if (n > 0) {
            char *p = buf;
            uint32_t previous = 0;
            buf[n] = 0;
            while (*p >= '0' && *p <= '9') previous = previous * 10 + (uint32_t)(*p++ - '0');
            if (!strncmp(p, " done ", 6) || !strncmp(p, " error ", 7)) cur_seq = previous;
        }
    }
    rem_load();
    for (uint64_t loop = 0;; loop++) {
        if (loop % 10 == 0) rem_check();  /* reminders: about once a second */
        long n = (long)icda_read_file(REQ_PATH, buf, sizeof(buf) - 1);
        if (n > 0) {
            uint32_t seq = 0;
            char *p = buf;
            buf[n] = 0;
            while (*p >= '0' && *p <= '9') seq = seq * 10 + (uint32_t)(*p++ - '0');
            if (*p == '\n') p++;
            if (seq && seq != cur_seq) {
                cur_seq = seq;
                (void)icda_write_file("/dev/serial", "ai: request received\n", 21);
                handle(p);
            }
        }
        icda_sleep(10);                   /* a tenth of a second */
    }
}
