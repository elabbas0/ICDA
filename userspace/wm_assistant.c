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

/* The wave is redrawn on a clock of its own.  Every frame of it costs a
 * per-pixel pass over the whole width of the screen, so asking for one on
 * every pass of the compositor's loop - a hundred times a second - starved
 * everything else in the machine: the assistant's own daemon, the mixer and
 * the input path.  30 frames a second is all a wave needs to look alive. */
#define AI_FRAME_MS 33u
/* The card comes after the wave, not with it: it waits this long and then
 * fades in and rises. */
#define AI_CARD_DELAY_MS  260u
#define AI_CARD_FADE_MS   320u
#define AI_FADE_IN_MS     340u
#define AI_FADE_OUT_MS    220u

enum { ST_CLOSED, ST_INPUT, ST_LISTEN, ST_LOADING, ST_THINKING, ST_ANSWER, ST_DONE, ST_ERROR };

static struct {
    int      state;
    char     q[Q_MAX];
    char     shown_q[Q_MAX];
    char     a[A_MAX];
    char     provider[48];
    uint32_t seq;
    uint64_t t0;              /* ms, when the wave last moved */
    uint64_t t_open;          /* ms, when it opened (the card's fade starts here) */
    uint64_t last_frame;      /* ms, the last frame the wave was drawn on */
    float    energy;          /* eased wave amplitude 0..1 */
    float    speed;
    float    phase;           /* wave position: speed added up over time, so a change
                                * of speed changes how fast it moves, never where it is */
    int      level;           /* the voice while listening, 0..99 */
    int      shown;           /* drawn: still true while it fades out */
    int      closing;         /* fading out, ignore input */
    int      card_dirty;      /* the card changed since the last frame */
    int      caret;           /* the text field's caret, on or off */
    float    card_alpha;      /* the card's fade, 0..1 */
    int      a_h;             /* measured height of the answer, -1 when stale */
    uint32_t a_seen;          /* the answer's length when it was measured */
    ic_tween_t fade;          /* the whole overlay: 0 closed, 1 open */
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
    ic_tween_set(&A.fade, 0.0f);
}

int wm_ai_is_open(void) { return A.shown; }

float wm_ai_opacity(void) {
    float v = ic_tween_value(&A.fade);
    return v < 0.0f ? 0.0f : v;
}

void wm_ai_open(void) {
    if (A.shown && !A.closing) return;
    A.state = ST_INPUT;
    A.q[0] = 0;
    A.a[0] = 0;
    A.shown_q[0] = 0;
    A.provider[0] = 0;
    A.t0 = now_ms();
    A.t_open = A.t0;
    A.last_frame = 0;
    A.energy = 0.0f;
    A.speed = 0.0f;
    A.phase = 0.0f;
    A.shown = 1;
    A.closing = 0;
    A.card_dirty = 1;
    A.card_alpha = 0.0f;
    A.caret = -1;
    A.a_h = -1;
    A.a_seen = 0xFFFFFFFFu;
    ic_tween_to(&A.fade, 1.0f, AI_FADE_IN_MS, IC_EASE_DECELERATE);
    ic_sound("ai_listen");
}

void wm_ai_close(void) {
    if (!A.shown || A.closing) return;
    /* The state is left alone so the card keeps the shape it had while it
     * fades; ST_CLOSED is set when the fade is over. */
    A.closing = 1;
    A.q[0] = 0;
    ic_tween_to(&A.fade, 0.0f, AI_FADE_OUT_MS, IC_EASE_ACCELERATE);
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
    ic_rect_t r = ic_rect_make((sw - w) / 2, sh - WM_BAR_H - WAVE_H - h - 10, w, h);
    /* the card settles down into place as it fades in (from above the wave,
     * so the two layers never share a row) */
    if (A.card_alpha < 1.0f) r.y -= (int)((1.0f - A.card_alpha) * 16.0f + 0.5f);
    return r;
}

/* Only the band the wave moves in: while just the wave animates this is the
 * whole of what has to be recomposited, and it is a third of wm_ai_rect. */
ic_rect_t wm_ai_wave_rect(int sw, int sh) {
    int top = sh - WM_BAR_H - WAVE_H;
    if (top < 0) top = 0;
    return ic_rect_make(0, top, sw, sh - WM_BAR_H - top);
}

/* The card clears the wave band, so the two are composited as two layers
 * with two opacities without either one landing twice on the same pixels. */
ic_rect_t wm_ai_card_rect(int sw, int sh) { return card_rect(sw, sh); }

int wm_ai_card_visible(void) { return A.shown && A.card_alpha > 0.0f; }

float wm_ai_card_opacity(void) { return A.card_alpha; }

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
    A.a_h = -1;                    /* the answer's height has to be measured again */
    A.a_seen = 0xFFFFFFFFu;
    A.card_dirty = 1;
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
        if (!ic_streq(prov, A.provider)) {
            ic_strcpy(A.provider, prov, sizeof(A.provider));
            changed = 1;
            A.card_dirty = 1;
        }
    }
    if (*nl) nl++;
    if (!ic_streq(nl, A.a)) {
        ic_strcpy(A.a, nl, sizeof(A.a));
        A.a_h = -1;                /* its wrapped height is stale now */
        changed = 1;
        A.card_dirty = 1;
    }
    {
        if (ic_streq(state, "listening")) {         /* the provider field carries the voice level */
            int lv = 0;
            for (const char *c = A.provider; *c >= '0' && *c <= '9'; c++) lv = lv * 10 + (*c - '0');
            A.level = lv;
            A.provider[0] = 0;
            return changed;                          /* the voice moves the wave, not the card */
        }
        if (ic_streq(state, "heard")) {              /* what was said becomes the question */
            ic_strcpy(A.shown_q, A.a, sizeof(A.shown_q));
            A.a[0] = 0;
            A.a_h = -1;
            if (A.state != ST_THINKING) ic_sound("ai_thinking");
            A.state = ST_THINKING;
            A.card_dirty = 1;
            return 1;
        }
        int st = ic_streq(state, "loading") ? ST_LOADING :
                 ic_streq(state, "thinking") ? ST_THINKING : ic_streq(state, "answer") ? ST_ANSWER :
                 ic_streq(state, "done") ? ST_DONE : ic_streq(state, "error") ? ST_ERROR : A.state;
        if (st != A.state) {
            if (st == ST_DONE || st == ST_ERROR) ic_sound("ai_end");
            A.state = st;
            changed = 1;
            A.card_dirty = 1;
        }
    }
    return changed;
}

/* The card waits for the wave, then fades in and rises; it holds still after
 * that, so it only forces a repaint while its alpha actually moves. */
static void tick_card_fade(uint64_t t) {
    float a;
    if (A.closing) {
        a = 1.0f;
    } else if (t <= A.t_open + AI_CARD_DELAY_MS) {
        a = 0.0f;
    } else {
        uint64_t d = t - A.t_open - AI_CARD_DELAY_MS;
        a = ic_ease(IC_EASE_DECELERATE, (float)d / (float)AI_CARD_FADE_MS);
        if (a > 1.0f) a = 1.0f;
    }
    if (a < 0.0f) a = 0.0f;
    if (a != A.card_alpha) A.card_dirty = 1;
    A.card_alpha = a;
}

int wm_ai_tick(void) {
    uint64_t t;
    float want, want_speed;
    if (!A.shown) return WM_AI_KEEP;
    t = now_ms();
    /* the fade-out has run out: the overlay is gone */
    if (A.closing && wm_ai_opacity() <= 0.0f) {
        A.shown = 0;
        A.closing = 0;
        A.state = ST_CLOSED;
        A.card_alpha = 0.0f;
        return WM_AI_CLOSE;
    }
    if (!A.closing && (A.state == ST_LISTEN || A.state == ST_LOADING ||
                   A.state == ST_THINKING || A.state == ST_ANSWER))
        if (poll_reply()) A.card_dirty = 1;
    tick_card_fade(t);
    if (A.state == ST_INPUT) {                   /* the caret blinks on its own */
        int on = (int)((t / 500) % 2 == 0);
        if (on != A.caret) { A.caret = on; A.card_dirty = 1; }
    }
    /* Loading is quieter and slower than thinking: it is a wait, not work, and
     * the wave settling is the only sign of it that can be had. */
    want = A.state == ST_INPUT ? 0.45f : A.state == ST_LISTEN ? 0.2f + (float)A.level / 99.0f * 0.8f :
           A.state == ST_LOADING ? 0.5f :
           A.state == ST_THINKING ? 0.8f : A.state == ST_ANSWER ? 1.0f : 0.3f;
    want_speed = A.state == ST_THINKING ? 1.1f : A.state == ST_ANSWER ? 0.9f :
                 A.state == ST_LOADING ? 0.5f : 0.6f;
    A.energy += (want - A.energy) * 0.08f;
    A.speed += (want_speed - A.speed) * 0.05f;
    if (t > A.t0) A.phase += (float)(t - A.t0) / 1000.0f * A.speed;
    A.t0 = t;
    /* the whole overlay is one alpha: while it moves, all of it is repainted,
     * but never faster than 60 frames a second either */
    if (A.card_dirty || ic_tween_running(&A.fade)) {
        if (t - A.last_frame < AI_FRAME_MS / 2) return WM_AI_KEEP;   /* the flag stands */
        A.card_dirty = 0;
        A.last_frame = t;
        return WM_AI_REDRAW_ALL;
    }
    /* otherwise just the wave, on its own clock */
    if (t - A.last_frame < AI_FRAME_MS) return WM_AI_KEEP;
    A.last_frame = t;
    return WM_AI_REDRAW;
}

/* ---- drawing ---------------------------------------------------------------- */

static uint32_t add_px(uint32_t d, float r, float g, float b) {
    int R = (int)((d >> 16) & 255) + (int)r, G = (int)((d >> 8) & 255) + (int)g, B = (int)(d & 255) + (int)b;
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    return 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

/* One table of dimming amounts for the band, filled once a frame: the fade
 * toward the taskbar depends only on the row, and it was being worked out
 * again for every one of the quarter of a million pixels under the wave. */
#define WAVE_DIM_ROWS 4096

static void draw_wave(ic_canvas_t *c, int sw, int sh) {
    static const struct { float r, g, b, freq, spd, phase, amp; } band[4] = {
        { 64, 156, 255, 2.2f, 1.00f, 0.0f, 1.00f },     /* blue */
        { 175, 82, 222, 3.1f, -1.35f, 1.7f, 0.80f },    /* violet */
        { 255, 70, 140, 1.6f, 0.85f, 3.9f, 0.70f },     /* pink */
        { 50, 215, 225, 4.0f, 1.70f, 5.1f, 0.55f },     /* cyan */
    };
    static uint16_t dim_row[WAVE_DIM_ROWS];
    int s = c->scale > 0 ? c->scale : 1;
    int by0 = (sh - WM_BAR_H - WAVE_H) * s, by1 = (sh - WM_BAR_H) * s, w = sw * s;
    int bx0, by0c, bx1, by1c, x0, x1, y0, y1;
    float t = A.phase;
    float mid = (float)(by0 + by1) * 0.5f, half = (float)(by1 - by0) * 0.5f;
    float curve[4];
    /* what the caller lets us touch, never past the canvas itself */
    if (!ic_canvas_bounds(c, &bx0, &by0c, &bx1, &by1c)) return;
    x0 = bx0;
    x1 = bx1 < w ? bx1 : w;
    y0 = by0 < by0c ? by0 : by0c;
    y1 = by1 > by1c ? by1c : by1;
    if (y0 >= y1 || x0 >= x1) return;
    if (by0 < 0) by0 = 0;
    /* dim the desktop under the band so the glow reads */
    for (int y = by0; y < by1 && y < WAVE_DIM_ROWS; y++) {
        float fade = (float)(y - by0) / (float)(by1 - by0);   /* darker toward the taskbar */
        dim_row[y] = (uint16_t)(150.0f * fade * fade);
    }
    for (int x = x0; x < x1; x++) {
        float u = (float)x / (float)w;                          /* 0..1 across */
        float taper = fsin(u * 3.14159265f);                     /* ends fade out */
        float wid, reach;
        int lo = y1, hi = y0;
        taper *= taper;
        for (int i = 0; i < 4; i++) {
            float a = band[i].amp * A.energy * taper * half * 0.85f;
            curve[i] = mid + a * fsin(u * band[i].freq * 6.2831853f + t * band[i].spd * 2.0f + band[i].phase);
        }
        wid = 2.5f * (float)s + 9.0f * A.energy * taper;
        /* Past |dy| of 16 * taper * wid a band adds less than one 255th, so
         * the rows outside the bands' reach are dimmed and left alone -
         * most of the band, on most columns. */
        reach = 16.0f * taper * wid;
        for (int i = 0; i < 4; i++) {
            int a = (int)(curve[i] - reach), b = (int)(curve[i] + reach) + 1;
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        }
        if (lo < y0) lo = y0;
        if (hi > y1) hi = y1;
        for (int y = y0; y < y1; y++) {
            uint32_t *p = &c->px[y * c->w + x];
            uint32_t d = *p, dim = y < WAVE_DIM_ROWS ? dim_row[y] : 0;
            /* dim the desktop under the band so the glow reads */
            d = (((((d >> 16) & 255) * (255 - dim)) >> 8) << 16) | (((((d >> 8) & 255) * (255 - dim)) >> 8) << 8) |
                (((d & 255) * (255 - dim)) >> 8);
            if (y >= lo && y < hi) {
                float fy = (float)y, R = 0, G = 0, B = 0;
                for (int i = 0; i < 4; i++) {
                    float dd = (fy - curve[i]) / wid;
                    float glow = taper / (1.0f + dd * dd);
                    R += band[i].r * glow;
                    G += band[i].g * glow;
                    B += band[i].b * glow;
                }
                *p = add_px(d, R * 0.9f, G * 0.9f, B * 0.9f);
            } else {
                *p = d;
            }
        }
    }
}

/* the wave band, on its own: the compositor redraws it every frame it moves */
void wm_ai_draw_wave(ic_canvas_t *c, int sw, int sh) {
    if (!A.shown) return;
    draw_wave(c, sw, sh);
}

/* the card: skipped whole when the compositor is only repainting the wave */
void wm_ai_draw_card(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p;
    ic_rect_t card;
    int cx0, cy0, cx1, cy1;
    if (!wm_ai_card_visible()) return;
    card = card_rect(sw, sh);
    if (!ic_canvas_bounds(c, &cx0, &cy0, &cx1, &cy1)) return;
    if (card.x + card.w <= cx0 || cx1 <= card.x || card.y + card.h <= cy0 || cy1 <= card.y) return;
    p = ic_palette();

    ic_ui_panel(c, card, IC_R_PANEL + 4.0f, IC_ELEV_MENU, scratch, scratch_len);
    if (A.state == ST_INPUT) {
        ic_textfield_t tf;
        int len = (int)ic_strlen(A.q);
        tf.text = A.q;
        tf.cursor = len;
        tf.sel_start = tf.sel_end = len;
        tf.focused = 1;
        tf.caret_on = A.caret;
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
        const char *status = A.state == ST_LISTEN ? "Listening - speak, then pause" :
                             A.state == ST_LOADING ? "Loading the local model - a large one takes a few minutes" :
                             A.state == ST_THINKING ? "Thinking..." : A.state == ST_ERROR ? "Something went wrong" :
                             A.state == ST_ANSWER ? "Answering" : "Enter: ask again   Esc: close";
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), qr, A.shown_q, p->label, IC_ALIGN_LEFT);
        ic_gfx_hline(c, card.x + CARD_PAD, card.y + 36, card.w - 2 * CARD_PAD, p->separator);
        ic_canvas_push_clip(c, ar.x, ar.y, ar.w, ar.h, &saved);
        {
            /* The last lines when the answer outgrows the card.  Measuring it
             * walks every glyph of the whole answer, so it is done when the
             * answer changes, not on every frame of the wave behind it. */
            const ic_face_t *f = ic_font(IC_FONT_BODY);
            ic_rect_t shifted = ar;
            uint32_t len = (uint32_t)ic_strlen(A.a);
            if (A.a_h < 0 || A.a_seen != len) {
                ic_rect_t whole = ar;
                whole.h = 0;                            /* no limit: the full height */
                A.a_h = ic_text_draw_wrapped(0, f, whole, A.a, p->label, 4);
                A.a_seen = len;
            }
            if (A.a_h > ar.h) shifted.y -= A.a_h - ar.h;
            shifted.h = A.a_h > ar.h ? A.a_h : ar.h;
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
    if (!A.shown) return WM_AI_KEEP;
    if (A.closing) return WM_AI_KEEP;             /* fading out: nothing else takes */
    if (key == 27) {
        wm_ai_close();
        return WM_AI_REDRAW_ALL;
    }
    if (A.state == ST_DONE || A.state == ST_ERROR) {
        if (key == 13) {                         /* ask again */
            A.state = ST_INPUT;
            A.q[0] = 0;
            A.card_dirty = 1;
            ic_sound("ai_listen");
            return WM_AI_REDRAW_ALL;
        }
        if (key >= 32 && key < 127) {            /* typing starts the next question */
            A.state = ST_INPUT;
            A.q[0] = (char)key;
            A.q[1] = 0;
            A.card_dirty = 1;
            return WM_AI_REDRAW_ALL;
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
    A.card_dirty = 1;
    return WM_AI_REDRAW_ALL;
}

int wm_ai_click(int sw, int sh, int mx, int my) {
    if (!A.shown || A.closing) return WM_AI_KEEP;
    if (ic_ui_hit(card_rect(sw, sh), mx, my)) return WM_AI_KEEP;
    wm_ai_close();
    return WM_AI_REDRAW_ALL;
}
