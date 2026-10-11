/* The assistant overlay: the wave, the question card and the streamed answer.
 *
 * The wave is drawn per pixel, like a fragment shader: a few sine bands with
 * their own colour, speed and phase, each a soft glow (1 / (1 + d^2)) around
 * its curve, tapered to nothing at the screen edges and added over a dark
 * fade.  How tall and fast the bands move follows the state: calm while it
 * listens, quick while it thinks, lively while the answer streams in.
 *
 * Requests go to /tmp/ai/request ("seq\nquestion"); the daemon answers in
 * /tmp/ai/reply ("seq state provider\nanswer..."), state being thinking,
 * answer, done or error. */

#include "wm_assistant.h"
#include "wm_shell.h"

#define WAVE_H      150
#define CARD_W      620
#define CARD_PAD    18
#define Q_MAX       512
#define A_MAX       8192
#define REQ_PATH    "/tmp/ai/request"
#define REPLY_PATH  "/tmp/ai/reply"

enum { ST_CLOSED, ST_INPUT, ST_LISTEN, ST_THINKING, ST_ANSWER, ST_DONE, ST_ERROR };

static struct {
    int      state;
    char     q[Q_MAX];
    char     shown_q[Q_MAX];
    char     a[A_MAX];
    char     provider[48];
    uint32_t seq;
    uint64_t t0;              /* ms, animation clock */
    float    energy;          /* eased wave amplitude 0..1 */
    float    speed;
    int      level;           /* the voice while listening, 0..99 */
} A;

static uint64_t now_ms(void) { return icda_ticks() * 10; }

/* ---- fast sine (parabola, refined), enough for a wave ------------------ */
static float fsin(float x) {
    const float pi = 3.14159265f, tau = 6.2831853f;
    float y;
    x -= tau * (float)(int)(x / tau);
    if (x > pi) x -= tau;
    if (x < -pi) x += tau;
    y = 1.2732395f * x - 0.4052847f * x * (x < 0 ? -x : x);
    return 0.225f * (y * (y < 0 ? -y : y) - y) + y;
}

void wm_ai_init(void) {
    (void)icda_mkdir("/tmp");
    (void)icda_mkdir("/tmp/ai");
    (void)icda_spawn("/sbin/assistantd");
    A.seq = (uint32_t)icda_ticks();           /* never one the daemon saw before a restart */
}

int wm_ai_is_open(void) { return A.state != ST_CLOSED; }

void wm_ai_open(void) {
    if (A.state != ST_CLOSED) return;
    A.state = ST_INPUT;
    A.q[0] = 0;
    A.a[0] = 0;
    A.shown_q[0] = 0;
    A.provider[0] = 0;
    A.t0 = now_ms();
    A.energy = 0.0f;
    ic_sound("ai_listen");
}

void wm_ai_close(void) {
    if (A.state == ST_CLOSED) return;
    A.state = ST_CLOSED;
    ic_sound("ai_end");
}

ic_rect_t wm_ai_rect(int sw, int sh) {
    int top = sh - WM_BAR_H - WAVE_H - 260;
    if (top < 0) top = 0;
    return ic_rect_make(0, top, sw, sh - WM_BAR_H - top);
}

static ic_rect_t card_rect(int sw, int sh) {
    int w = sw - 40 < CARD_W ? sw - 40 : CARD_W;
    int h = A.state == ST_INPUT ? 64 : 230;
    return ic_rect_make((sw - w) / 2, sh - WM_BAR_H - WAVE_H / 2 - h - 10, w, h);
}

/* ---- talking to the daemon ------------------------------------------------- */

static void send_request(void) {
    char buf[Q_MAX + 16];
    int n = 0;
    uint32_t s;
    A.seq++;
    s = A.seq;
    {
        char tmp[12];
        int k = 0;
        do tmp[k++] = (char)('0' + s % 10); while (s /= 10);
        while (k) buf[n++] = tmp[--k];
    }
    buf[n++] = '\n';
    for (int i = 0; A.q[i] && n < (int)sizeof(buf) - 1; i++) buf[n++] = A.q[i];
    (void)icda_write_file(REQ_PATH, buf, (uint64_t)n);
    ic_strcpy(A.shown_q, ic_streq(A.q, "/listen") ? "Listening..." : A.q, sizeof(A.shown_q));
    A.state = ic_streq(A.q, "/listen") ? ST_LISTEN : ST_THINKING;
    if (A.state == ST_THINKING) ic_sound("ai_thinking");
    A.q[0] = 0;
    A.a[0] = 0;
}

static int poll_reply(void) {
    static char buf[A_MAX + 128];
    long n = (long)icda_read_file(REPLY_PATH, buf, sizeof(buf) - 1);
    uint32_t seq = 0;
    const char *p = buf, *nl;
    char state[16];
    int k = 0, changed = 0;
    if (n <= 0) return 0;
    buf[n] = 0;
    while (*p >= '0' && *p <= '9') seq = seq * 10 + (uint32_t)(*p++ - '0');
    if (seq != A.seq) return 0;
    if (*p == ' ') p++;
    while (*p && *p != ' ' && *p != '\n' && k < 15) state[k++] = *p++;
    state[k] = 0;
    if (*p == ' ') p++;
    nl = p;
    while (*nl && *nl != '\n') nl++;
    if (nl - p < (long)sizeof(A.provider)) {
        char prov[48];
        int m = 0;
        while (p < nl && m < 47) prov[m++] = *p++;
        prov[m] = 0;
        if (!ic_streq(prov, A.provider)) { ic_strcpy(A.provider, prov, sizeof(A.provider)); changed = 1; }
    }
    if (*nl) nl++;
    if (!ic_streq(nl, A.a)) {
        ic_strcpy(A.a, nl, sizeof(A.a));
        changed = 1;
    }
    {
        if (ic_streq(state, "listening")) {         /* the provider field carries the voice level */
            int lv = 0;
            for (const char *c = A.provider; *c >= '0' && *c <= '9'; c++) lv = lv * 10 + (*c - '0');
            A.level = lv;
            A.provider[0] = 0;
            return 1;
        }
        if (ic_streq(state, "heard")) {              /* what was said becomes the question */
            ic_strcpy(A.shown_q, A.a, sizeof(A.shown_q));
            A.a[0] = 0;
            if (A.state != ST_THINKING) ic_sound("ai_thinking");
            A.state = ST_THINKING;
            return 1;
        }
        int st = ic_streq(state, "thinking") ? ST_THINKING : ic_streq(state, "answer") ? ST_ANSWER :
                 ic_streq(state, "done") ? ST_DONE : ic_streq(state, "error") ? ST_ERROR : A.state;
        if (st != A.state) {
            if (st == ST_DONE || st == ST_ERROR) ic_sound("ai_end");
            A.state = st;
            changed = 1;
        }
    }
    return changed;
}

int wm_ai_tick(void) {
    float want, want_speed;
    if (A.state == ST_CLOSED) return WM_AI_KEEP;
    if (A.state == ST_LISTEN || A.state == ST_THINKING || A.state == ST_ANSWER) (void)poll_reply();
    want = A.state == ST_INPUT ? 0.45f : A.state == ST_LISTEN ? 0.2f + (float)A.level / 99.0f * 0.8f :
           A.state == ST_THINKING ? 0.8f : A.state == ST_ANSWER ? 1.0f : 0.3f;
    want_speed = A.state == ST_THINKING ? 2.6f : A.state == ST_ANSWER ? 1.8f : 1.0f;
    A.energy += (want - A.energy) * 0.08f;
    A.speed += (want_speed - A.speed) * 0.05f;
    return WM_AI_REDRAW;                       /* the wave moves every frame */
}

/* ---- drawing ---------------------------------------------------------------- */

static uint32_t add_px(uint32_t d, float r, float g, float b) {
    int R = (int)((d >> 16) & 255) + (int)r, G = (int)((d >> 8) & 255) + (int)g, B = (int)(d & 255) + (int)b;
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    return 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static void draw_wave(ic_canvas_t *c, int sw, int sh) {
    static const struct { float r, g, b, freq, spd, phase, amp; } band[4] = {
        { 64, 156, 255, 2.2f, 1.00f, 0.0f, 1.00f },     /* blue */
        { 175, 82, 222, 3.1f, -1.35f, 1.7f, 0.80f },    /* violet */
        { 255, 70, 140, 1.6f, 0.85f, 3.9f, 0.70f },     /* pink */
        { 50, 215, 225, 4.0f, 1.70f, 5.1f, 0.55f },     /* cyan */
    };
    int s = c->scale > 0 ? c->scale : 1;
    int y0 = (sh - WM_BAR_H - WAVE_H) * s, y1 = (sh - WM_BAR_H) * s, w = sw * s;
    float t = (float)(now_ms() - A.t0) / 1000.0f * A.speed;
    float mid = (float)(y0 + y1) * 0.5f, half = (float)(y1 - y0) * 0.5f;
    float curve[4];
    if (y0 < c->clip_y) y0 = c->clip_y;
    if (y1 > c->clip_y + c->clip_h) y1 = c->clip_y + c->clip_h;
    for (int x = c->clip_x; x < c->clip_x + c->clip_w && x < w; x++) {
        float u = (float)x / (float)w;                          /* 0..1 across */
        float taper = fsin(u * 3.14159265f);                     /* ends fade out */
        taper *= taper;
        for (int i = 0; i < 4; i++) {
            float a = band[i].amp * A.energy * taper * half * 0.85f;
            curve[i] = mid + a * fsin(u * band[i].freq * 6.2831853f + t * band[i].spd * 2.0f + band[i].phase);
        }
        for (int y = y0; y < y1; y++) {
            uint32_t *p = &c->px[y * c->w + x];
            float fy = (float)y, R = 0, G = 0, B = 0;
            float fade = (fy - (float)y0) / (float)(y1 - y0);     /* darker toward the taskbar */
            uint32_t d = *p;
            /* dim the desktop under the band so the glow reads */
            uint32_t dim = (uint32_t)(150.0f * fade * fade);
            d = (((((d >> 16) & 255) * (255 - dim)) >> 8) << 16) | (((((d >> 8) & 255) * (255 - dim)) >> 8) << 8) |
                (((d & 255) * (255 - dim)) >> 8);
            for (int i = 0; i < 4; i++) {
                float dd = (fy - curve[i]) / (2.5f * (float)s + 9.0f * A.energy * taper);
                float glow = taper / (1.0f + dd * dd);
                R += band[i].r * glow;
                G += band[i].g * glow;
                B += band[i].b * glow;
            }
            *p = add_px(d, R * 0.9f, G * 0.9f, B * 0.9f);
        }
    }
}

void wm_ai_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t card = card_rect(sw, sh);
    if (A.state == ST_CLOSED) return;
    draw_wave(c, sw, sh);

    ic_ui_panel(c, card, IC_R_PANEL + 4.0f, IC_ELEV_MENU, scratch, scratch_len);
    if (A.state == ST_INPUT) {
        ic_textfield_t tf;
        int len = (int)ic_strlen(A.q);
        tf.text = A.q;
        tf.cursor = len;
        tf.sel_start = tf.sel_end = len;
        tf.focused = 1;
        tf.caret_on = (now_ms() / 500) % 2 == 0;
        tf.placeholder = "Ask anything...  (Tab: speak, Enter: send, Esc: close)";
        tf.leading = IC_SYM_SEARCH;
        tf.scroll_px = 0;
        {
            ic_rect_t r = ic_rect_make(card.x + CARD_PAD, card.y + 14, card.w - 2 * CARD_PAD, 36);
            ic_ui_textfield_scroll(r, &tf);
            ic_ui_textfield(c, r, &tf);
        }
        return;
    }
    {
        ic_rect_t qr = ic_rect_make(card.x + CARD_PAD, card.y + 14, card.w - 2 * CARD_PAD, 18);
        ic_rect_t ar = ic_rect_make(card.x + CARD_PAD, card.y + 42, card.w - 2 * CARD_PAD, card.h - 42 - 30);
        ic_rect_t saved;
        const char *status = A.state == ST_LISTEN ? "Listening - speak, then pause" : A.state == ST_THINKING ? "Thinking..." : A.state == ST_ERROR ? "Something went wrong" :
                             A.state == ST_ANSWER ? "Answering" : "Enter: ask again   Esc: close";
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), qr, A.shown_q, p->label, IC_ALIGN_LEFT);
        ic_gfx_hline(c, card.x + CARD_PAD, card.y + 36, card.w - 2 * CARD_PAD, p->separator);
        ic_canvas_push_clip(c, ar.x, ar.y, ar.w, ar.h, &saved);
        {
            /* the last lines when the answer outgrows the card */
            const ic_face_t *f = ic_font(IC_FONT_BODY);
            ic_rect_t whole = ar, shifted = ar;
            int h;
            whole.h = 0;                                /* no limit: the full height */
            h = ic_text_draw_wrapped(0, f, whole, A.a, p->label, 4);
            if (h > ar.h) shifted.y -= h - ar.h;
            shifted.h = h > ar.h ? h : ar.h;
            ic_text_draw_wrapped(c, f, shifted, A.a[0] ? A.a : "", A.state == ST_ERROR ? p->danger : p->label, 4);
        }
        ic_canvas_pop_clip(c, &saved);
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(card.x + CARD_PAD, card.y + card.h - 24, card.w / 2, 14),
                        status, p->label_secondary, IC_ALIGN_LEFT);
        if (A.provider[0])
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION),
                            ic_rect_make(card.x + card.w / 2, card.y + card.h - 24, card.w / 2 - CARD_PAD, 14),
                            A.provider, p->label_tertiary, IC_ALIGN_RIGHT);
    }
}

/* ---- input --------------------------------------------------------------------- */

int wm_ai_key(int key) {
    int len = (int)ic_strlen(A.q);
    if (A.state == ST_CLOSED) return WM_AI_KEEP;
    if (key == 27) {
        wm_ai_close();
        return WM_AI_CLOSE;
    }
    if (A.state == ST_DONE || A.state == ST_ERROR) {
        if (key == 13) {                         /* ask again */
            A.state = ST_INPUT;
            A.q[0] = 0;
            ic_sound("ai_listen");
            return WM_AI_REDRAW;
        }
        if (key >= 32 && key < 127) {            /* typing starts the next question */
            A.state = ST_INPUT;
            A.q[0] = (char)key;
            A.q[1] = 0;
            return WM_AI_REDRAW;
        }
        return WM_AI_KEEP;
    }
    if (A.state != ST_INPUT) return WM_AI_KEEP;
    if (key == 13) {
        if (len) send_request();
    } else if (key == 9 && !len) {             /* Tab: speak the question */
        ic_strcpy(A.q, "/listen", sizeof(A.q));
        send_request();
        ic_sound("ai_listen");
    } else if (key == 8 || key == 127) {
        if (len) A.q[len - 1] = 0;
    } else if (key >= 32 && key < 127 && len < Q_MAX - 1) {
        A.q[len] = (char)key;
        A.q[len + 1] = 0;
    } else {
        return WM_AI_KEEP;
    }
    return WM_AI_REDRAW;
}

int wm_ai_click(int sw, int sh, int mx, int my) {
    if (A.state == ST_CLOSED) return WM_AI_KEEP;
    if (ic_ui_hit(card_rect(sw, sh), mx, my)) return WM_AI_KEEP;
    wm_ai_close();
    return WM_AI_CLOSE;
}
