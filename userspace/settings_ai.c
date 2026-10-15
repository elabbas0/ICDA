/* Settings > Assistant: the models the assistant asks.
 * assistantd reads /home/.config/ai.conf, one provider per line:
 *
 *   <tier> <name> <base url> <model> <api key or ->
 *   speak on | speak off
 *
 * Editing that by hand means getting the five columns and the tier spelling
 * right, and the file is the only place the assistant's models live.  This
 * pane reads it, lets each field be edited as a field, and writes it back in
 * exactly that format.  Nothing else changes: assistantd keeps reading the
 * same file, and picks up a change on its next question.
 *
 * A local provider is one served by icda-llm on this machine ("file:auto"
 * selects the Model filename in ICDA's linux/usr/share/icda/models); a cloud one
 * is any OpenAI-compatible endpoint. */

#include "settings_ai.h"
#include "settings_store.h"
#include "model_catalog.h"

#define AI_CONF     "/home/.config/ai.conf"
#define AI_MAX      12
#define AI_NAME     34
#define AI_URL      104
#define AI_MODEL    52
#define AI_KEY      48
#define AI_ROW_H    46
#define AI_SIDE     210

/* This pane links against libicda, not the C library, so it uses the ic_*
 * helpers.  These three are the ones it needs that libicda does not have. */
static int ic_strcmp_n(const char *a, const char *b, int n) {
    while (n-- > 0 && *a && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return n == 0 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

static char *ic_strchr(const char *s, char c) {
    for (; *s; s++)
        if (*s == c) return (char *)s;
    return c ? 0 : (char *)s;
}

/* Only %s is needed here; the width/left-align in the ai.conf layout is
 * cosmetic, so the flags are skipped and the arguments appended as they come. */
static int snprintf_(char *out, uint64_t cap, const char *fmt, ...) {
    __builtin_va_list ap;
    int n = 0;
    const char *p;

    __builtin_va_start(ap, fmt);
    for (p = fmt; *p; p++) {
        if (*p != '%') {
            if ((uint64_t)n + 1 < cap) out[n++] = *p;
            continue;
        }
        p++;
        while (*p == '-' || (*p >= '0' && *p <= '9')) p++;
        if (*p != 's') continue;
        {
            const char *s = __builtin_va_arg(ap, const char *);
            while (*s && (uint64_t)n + 1 < cap) out[n++] = *s++;
        }
    }
    __builtin_va_end(ap);
    if ((uint64_t)n < cap) out[n] = 0;
    return n;
}

enum { F_NAME = 0, F_URL, F_MODEL, F_KEY, F_COUNT };

static const char *const field_label[F_COUNT] = { "Name", "Address", "Model", "API key" };
static const char *const field_hint[F_COUNT] = {
    "How it appears while it answers",
    "file:auto  or  https://host/v1",
    "Local: click to choose Qwen 0.5B or 1.5B; cloud: server model name",
    "Leave empty for a local model"
};

typedef struct {
    int  used;
    int  cloud;                       /* 0 local, 1 cloud */
    char name[AI_NAME];
    char url[AI_URL];
    char model[AI_MODEL];
    char key[AI_KEY];
} ai_provider_t;

static struct {
    ai_provider_t p[AI_MAX];
    int           count;
    int           sel;
    int           focus;              /* F_COUNT = the tier switch */
    int           editing;            /* a field has the keyboard */
    int           hover_row;
    int           hover_btn;
    int           save_failed;
    int           speak;
    int           loaded;
    char          download_status[256];
    ic_tween_t    reveal;
} A;

/* ---- the file ------------------------------------------------------------ */

static int is_word(char c) { return c && c != ' ' && c != '\t' && c != '\n' && c != '\r'; }

static int next_word(const char **p, char *out, int cap) {
    int n = 0;
    while (**p == ' ' || **p == '\t') (*p)++;
    if (!is_word(**p)) return 0;
    while (is_word(**p)) {
        if (n + 1 < cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n;
}

static void ai_load(void) {
    static char text[8192];
    long n = (long)icda_read_file(AI_CONF, text, sizeof(text) - 1);
    A.count = 0;
    A.speak = 1;
    if (n <= 0) {
        (void)icda_mkdir("/home");
        (void)icda_mkdir("/home/.config");
        A.loaded = 1;
        return;
    }
    text[n] = 0;
    for (char *line = text; *line; ) {
        char *nl = ic_strchr(line, '\n');
        char tier[16];
        if (nl) *nl = 0;
        if (line[0] == '#' || !is_word(line[0])) goto next;
        {
            const char *q = line;
            ai_provider_t *v;
            if (next_word(&q, tier, sizeof(tier)) == 0) goto next;
            if (!ic_strcmp_n(tier, "speak", 5)) {
                char on[16];
                if (next_word(&q, on, sizeof(on))) A.speak = ic_strcmp_n(on, "off", 3) != 0;
                goto next;
            }
            if (A.count >= AI_MAX) goto next;
            v = &A.p[A.count];
            ic_memset(v, 0, sizeof(*v));
            v->used = 1;
            v->cloud = ic_strcmp(tier, "cloud") == 0;
            if (next_word(&q, v->name, sizeof(v->name)) &&
                next_word(&q, v->url, sizeof(v->url)) &&
                next_word(&q, v->model, sizeof(v->model)) &&
                next_word(&q, v->key, sizeof(v->key))) {
                if (ic_strcmp(v->url, "codex:chatgpt") == 0) goto next;
                if (ic_strcmp(v->key, "-") == 0) v->key[0] = 0;
                A.count++;
            } else {
                v->used = 0;
            }
        }
next:
        if (!nl) break;
        line = nl + 1;
    }
    A.loaded = 1;
}

/* Rewrites the file in assistantd's format, comments and all. */
static int ai_save(void) {
    char out[8192];
    int n = 0;
    n += snprintf_(out + n, sizeof(out) - (size_t)n,
                  "# ICDA assistant: the models it asks, tried in order.\n"
                  "#   <tier> <name> <base url> <model> <api key or ->\n"
                  "# file:auto selects the Model filename in ICDA's linux/usr/share/icda/models.\n"
                  "# cloud answers the complex questions.\n"
                  "# Edited in Settings > Assistant.\n");
    for (int i = 0; i < A.count && n < (int)sizeof(out) - 200; i++) {
        const ai_provider_t *v = &A.p[i];
        if (!v->used) continue;
        n += snprintf_(out + n, sizeof(out) - (size_t)n, "%-6s %-16s %-40s %-24s %s\n",
                      v->cloud ? "cloud" : "local", v->name, v->url, v->model,
                      v->key[0] ? v->key : "-");
    }
    n += snprintf_(out + n, sizeof(out) - (size_t)n, "speak %s\n", A.speak ? "on" : "off");
    if (n <= 0) return -1;
    (void)icda_mkdir("/home");
    (void)icda_mkdir("/home/.config");
    if ((long)icda_write_file(AI_CONF, out, (uint64_t)n) < 0) {
        A.save_failed = 1;
        return -1;
    }
    A.save_failed = 0;
    return 0;
}

/* ---- editing ------------------------------------------------------------- */

static ai_provider_t *cur(void) {
    if (A.sel < 0 || A.sel >= A.count) return 0;
    return A.p[A.sel].used ? &A.p[A.sel] : 0;
}

/* Choose from the downloadable pinned catalog, including absent models. */
static void choose_local_model(void) {
    ai_provider_t *v=cur(); if(!v) return;
    unsigned next=0;
    for(unsigned i=0;i<ICDA_MODEL_COUNT;i++) if(!ic_strcmp(v->model,icda_models[i].name)) next=(i+1)%ICDA_MODEL_COUNT;
    ic_strcpy(v->model,icda_models[next].name,sizeof(v->model));
    (void)ai_save();
}
static char *cur_field(int f) {
    ai_provider_t *v = cur();
    if (!v) return 0;
    switch (f) {
    case F_NAME:  return v->name;
    case F_URL:   return v->url;
    case F_MODEL: return v->model;
    case F_KEY:   return v->key;
    default:      return 0;
    }
}

static int cur_field_len(int f) {
    char *p = cur_field(f);
    return p ? (int)ic_strlen(p) : 0;
}

static void ai_add(void) {
    if (A.count >= AI_MAX) return;
    ai_provider_t *v = &A.p[A.count];
    ic_memset(v, 0, sizeof(*v));
    v->used = 1;
    v->cloud = 1;
    snprintf_(v->name, sizeof(v->name), "provider");
    snprintf_(v->url, sizeof(v->url), "https://");
    snprintf_(v->model, sizeof(v->model), "model");
    A.count++;
    A.sel = A.count - 1;
    A.focus = F_NAME;
}

static void ai_remove(void) {
    ai_provider_t *v = cur();
    if (!v) return;
    for (int i = A.sel; i + 1 < A.count; i++) A.p[i] = A.p[i + 1];
    A.count--;
    ic_memset(&A.p[A.count], 0, sizeof(A.p[0]));
    if (A.sel >= A.count) A.sel = A.count - 1;
    if (A.sel < 0) A.sel = 0;
}

static void ai_toggle_speak(void) {
    A.speak = !A.speak;
    (void)ai_save();
}

/* ---- layout -------------------------------------------------------------- */

static ic_rect_t list_rect(ic_rect_t area) {
    int w = area.w / 3 < AI_SIDE ? area.w / 3 : AI_SIDE;
    if (w < 150) w = area.w;
    return ic_rect_make(area.x, area.y, w, area.h);
}

static ic_rect_t row_rect(ic_rect_t list, int i) {
    return ic_rect_make(list.x + 6, list.y + i * AI_ROW_H, list.w - 12, AI_ROW_H - 6);
}

/* One field's vertical budget.  Drawn and hit-tested from the same numbers:
 * a label, the box, the hint under it, then the gap to the next label. */
#define FLD_LABEL_H  14
#define FLD_BOX_H    30
#define FLD_HINT_GAP 6
#define FLD_HINT_H   11
#define FLD_NEXT_GAP 10
#define FLD_H        (FLD_LABEL_H + FLD_BOX_H + FLD_HINT_GAP + FLD_HINT_H + FLD_NEXT_GAP)

/* where the fields start, and where the speak button sits at the end */
static int form_x(ic_rect_t area) {
    return area.x + list_rect(area).w + IC_SP_4;
}

static int form_w(ic_rect_t area) {
    return area.x + area.w - form_x(area) - IC_SP_3;
}

static int fields_top(ic_rect_t area) {
    return area.y + 4 + IC_H_CONTROL_SM + IC_SP_5;
}

static int form_y(ic_rect_t area) {
    return fields_top(area) + FLD_H * F_COUNT + IC_SP_4;
}

static ic_rect_t speak_rect(ic_rect_t area) {
    int y = form_y(area);
    return ic_rect_make(form_x(area), y + 4, 120, IC_H_CONTROL);
}

/* the field boxes, in the order they are drawn */
static ic_rect_t field_rect(ic_rect_t area, int f) {
    return ic_rect_make(form_x(area), fields_top(area) + f * FLD_H + FLD_LABEL_H,
                        form_w(area), FLD_BOX_H);
}

/* Add and Remove, side by side along the bottom of the list */
enum { BTN_ADD = 0, BTN_REMOVE, BTN_COUNT };
static const char *const btn_label[BTN_COUNT] = { "Add", "Remove" };

static int list_buttons_fit(ic_rect_t list) {
    return list.h > AI_ROW_H * 2 + IC_H_CONTROL + IC_SP_6;
}

static ic_rect_t btn_rect(ic_rect_t list, int b) {
    int w = (list.w - 12 - IC_SP_2) / 2;
    return ic_rect_make(list.x + 6 + b * (w + IC_SP_2), list.y + list.h - IC_H_CONTROL - 6,
                        w, IC_H_CONTROL);
}

/* ---- draw ---------------------------------------------------------------- */

void ai_pane_enter(void) {
    ic_memset(&A, 0, sizeof(A));
    ai_load();
    if (!A.count) {
        ai_provider_t *v=&A.p[A.count++]; ic_memset(v,0,sizeof(*v)); v->used=1;
        ic_strcpy(v->name,"qwen",sizeof(v->name)); ic_strcpy(v->url,"file:auto",sizeof(v->url));
        ic_strcpy(v->model,icda_models[0].name,sizeof(v->model));
    }
    (void)ai_save();
    A.sel = A.count ? 0 : -1;
    A.focus = F_NAME;
    ic_tween_set(&A.reveal, 1.0f);
}

void ai_pane_leave(void) {
    (void)ai_save();
}

void ai_pane_tick(ic_app_t *app) {
    char status[256]; long n=(long)icda_read_file("/tmp/ai/model-install.status",status,sizeof(status)-1);
    if(n<0) n=0;
    status[n]=0;
    if(ic_strcmp(status,A.download_status)) {ic_strcpy(A.download_status,status,sizeof(A.download_status));ic_app_invalidate(app);}
}

static void draw_provider_list(ic_canvas_t *c, ic_rect_t list) {
    const ic_palette_t *p = ic_palette();
    ic_ui_panel(c, list, IC_R_GROUP + 2.0f, IC_ELEV_MENU, 0, 0);
    for (int i = 0; i < A.count; i++) {
        const ai_provider_t *v = &A.p[i];
        ic_rect_t r = row_rect(list, i);
        if (i == A.sel) {
            ic_gfx_rrect(c, r.x + 2, r.y + 1, r.w - 4, r.h - 1, IC_R_ROW,
                         ic_color_fade(p->fill_selected_idle, 1.0f));
        }
        {
            ic_rect_t badge = ic_rect_make(r.x + 8, r.y + (r.h - 18) / 2, 40, 18);
            ic_gfx_rrect(c, badge.x, badge.y, badge.w, badge.h, 9.0f,
                         ic_color_fade(v->cloud ? p->accent : p->toggle_off, 0.85f));
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), badge,
                            v->cloud ? "cloud" : "local", p->label_on_accent, IC_ALIGN_CENTER);
        }
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + 54, r.y + 4, r.w - 60, 16),
                        v->name, p->label, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 54, r.y + 20, r.w - 60, 14),
                        v->url, p->label_tertiary, IC_ALIGN_LEFT);
    }
    if (!A.count) {
        ic_text_draw_in(c, ic_font(IC_FONT_BODY),
                        ic_rect_make(list.x + 12, list.y + 12, list.w - 24, 20),
                        "No models yet", p->label_secondary, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(list.x + 12, list.y + 32, list.w - 24, 32),
                        "Add one to let the assistant answer.", p->label_tertiary, IC_ALIGN_LEFT);
    }
    if (list_buttons_fit(list)) {
        for (int b = 0; b < BTN_COUNT; b++) {
            int off = b == BTN_REMOVE && !cur();
            ic_ui_button(c, btn_rect(list, b), btn_label[b], IC_SYM_NONE, IC_BUTTON_DEFAULT,
                         off ? IC_STATE_DISABLED : A.hover_btn == b ? IC_STATE_HOVER : IC_STATE_NORMAL);
        }
    }
}

static void draw_field(ic_canvas_t *c, ic_rect_t r, int f, const char *value, int editing) {
    ic_textfield_t tf;
    ic_memset(&tf, 0, sizeof(tf));
    tf.text = (char *)(uintptr_t)(value ? value : "");
    tf.cursor = cur_field_len(f);
    tf.sel_start = tf.sel_end = tf.cursor;
    tf.focused = editing;
    tf.caret_on = editing && ((ic_time_ms() / 500) % 2 == 0);
    ic_ui_textfield_scroll(r, &tf);
    ic_ui_textfield(c, r, &tf);
}

static void draw_form(ic_canvas_t *c, ic_rect_t area) {
    const ic_palette_t *p = ic_palette();
    int x = form_x(area);
    int w = form_w(area);
    ai_provider_t *v = cur();

    if (!v) {
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(x, area.y + 12, w, 24),
                        "Nothing selected", p->label_secondary, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(x, area.y + 44, w, 40),
                        "Add a model on the left, or pick one to edit it.", p->label_tertiary,
                        IC_ALIGN_LEFT);
        return;
    }

    /* which tier answers the everyday questions */
    {
        const char *segs[2] = { "Local", "Cloud" };
        ic_rect_t seg = ic_rect_make(form_x(area), area.y + 4, 240, IC_H_CONTROL_SM);
        ic_ui_segmented(c, seg, segs, 2, v->cloud ? 1.0f : 0.0f, -1);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(seg.x + 250, seg.y, form_w(area) - 250, IC_H_CONTROL_SM),
                        v->cloud ? "Complex questions, research and code"
                                 : "Everyday questions, offline on this machine",
                        p->label_tertiary, IC_ALIGN_LEFT);
    }

    for (int f = 0; f < F_COUNT; f++) {
        ic_rect_t box = field_rect(area, f);
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH),
                        ic_rect_make(box.x, box.y - FLD_LABEL_H, box.w, FLD_LABEL_H),
                        (!v->cloud && f == F_KEY) ? "Install selected Qwen model" : field_label[f], p->label_secondary, IC_ALIGN_LEFT);
        if (f == F_KEY && !v->cloud)
            ic_ui_button(c, box, "Download from Hugging Face", IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_NORMAL);
        else if (f == F_MODEL && !v->cloud && ic_strcmp(v->url, "file:auto") == 0) {
            const char *label = v->model;
            for (unsigned i = 0; i < ICDA_MODEL_COUNT; i++)
                if (!ic_strcmp(v->model, icda_models[i].name)) label = icda_models[i].label;
            ic_ui_button(c, box, label, IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_NORMAL);
        }
        else draw_field(c, box, f, cur_field(f), A.editing && A.focus == f);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(box.x, box.y + box.h + FLD_HINT_GAP, box.w, FLD_HINT_H),
                        !v->cloud && f == F_KEY ? (A.download_status[0] ? A.download_status : "Official Qwen source; verified download; no restart needed") : field_hint[f], p->label_tertiary, IC_ALIGN_LEFT);
    }

    if (speak_rect(area).y + IC_H_CONTROL <= area.y + area.h) {
        ic_rect_t b = speak_rect(area);
        ic_ui_button(c, b, A.speak ? "Spoken: on" : "Spoken: off", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                     A.hover_btn == BTN_COUNT ? IC_STATE_HOVER : IC_STATE_NORMAL);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(b.x + 130, b.y, form_w(area) - 130, IC_H_CONTROL),
                        "Read answers aloud with the offline voice", p->label_tertiary, IC_ALIGN_LEFT);
    }
    if (A.save_failed) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(form_x(area), area.y + area.h - 18, form_w(area), 14),
                        "Could not write ai.conf - is the disk read-only?", p->danger, IC_ALIGN_LEFT);
    }
}

void ai_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area) {
    (void)app;
    A.hover_row = -1;
    A.hover_btn = BTN_COUNT;
    draw_provider_list(c, list_rect(area));
    draw_form(c, area);
}

/* ---- input --------------------------------------------------------------- */

/* Only a field being edited takes the keyboard: otherwise Escape, the
 * shortcuts and the sidebar all have to keep working. */
int ai_pane_modal(void) {
    return A.editing;
}

static int field_cap(int f) {
    switch (f) {
    case F_NAME:  return AI_NAME;
    case F_MODEL: return AI_MODEL;
    case F_KEY:   return AI_KEY;
    default:      return AI_URL;
    }
}

int ai_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area) {
    ic_rect_t list = list_rect(area);
    (void)app;
    if (!A.loaded) return 0;
    if ((ev->type == IC_EV_MOUSE_DOWN || ev->type == IC_EV_MOUSE_MOVE) &&
        !ic_ui_hit(area, ev->x, ev->y)) {
        if (ev->type == IC_EV_MOUSE_DOWN) A.editing = 0;
        return 0;
    }

    if (ev->type == IC_EV_KEY) {
        char *buf = A.editing && A.focus < F_COUNT ? cur_field(A.focus) : 0;
        int len, cap;
        if (!buf) return 0;
        len = (int)ic_strlen(buf);
        cap = field_cap(A.focus);
        if (ev->key == IC_KEY_ESCAPE) { A.editing = 0; return 1; }
        if (ev->key == IC_KEY_TAB) {
            A.focus = (A.focus + 1) % F_COUNT;
            (void)ai_save();
            return 1;
        }
        if (ev->key == IC_KEY_ENTER) { A.editing = 0; (void)ai_save(); return 1; }
        if (ev->key == IC_KEY_BACKSPACE || ev->key == IC_KEY_DELETE) {
            if (len > 0) { buf[len - 1] = 0; (void)ai_save(); }
            return 1;
        }
        if (ev->key >= 32 && ev->key < 127 && len + 1 < cap) {
            buf[len] = (char)ev->key;
            buf[len + 1] = 0;
            (void)ai_save();
            return 1;
        }
        return 1;                       /* the field owns the keyboard while it has one */
    }

    if (ev->type == IC_EV_MOUSE_DOWN) {
        if (list_buttons_fit(list)) {
            if (ic_ui_hit(btn_rect(list, BTN_ADD), ev->x, ev->y)) {
                ai_add();
                A.editing = 1;
                return 1;
            }
            if (ic_ui_hit(btn_rect(list, BTN_REMOVE), ev->x, ev->y) && cur()) {
                ai_remove();
                A.editing = 0;
                return 1;
            }
        }
        for (int i = 0; i < A.count; i++) {
            if (ic_ui_hit(row_rect(list, i), ev->x, ev->y)) {
                A.sel = i;
                A.focus = F_NAME;
                A.editing = 0;
                return 1;
            }
        }
        if (cur()) {
            ai_provider_t *v = cur();
            ic_rect_t tier = ic_rect_make(form_x(area), area.y + 4, 240, IC_H_CONTROL_SM);
            int choice = ic_ui_segmented_hit(tier, 2, ev->x, ev->y);
            if (choice >= 0) {
                v->cloud = choice != 0;
                if (!choice) {
                    ic_strcpy(v->url, "file:auto", sizeof(v->url));
                    ic_strcpy(v->model, "qwen2.5-0.5b-instruct-q4_k_m.gguf", sizeof(v->model));
                    v->key[0] = 0;
                } else if (!ic_strcmp(v->url, "file:auto")) {
                    ic_strcpy(v->url, "https://", sizeof(v->url));
                    ic_strcpy(v->model, "model", sizeof(v->model));
                }
                (void)ai_save();
                return 1;
            }
            if (!v->cloud && ic_ui_hit(field_rect(area,F_KEY),ev->x,ev->y)) {
                (void)ai_save();
                (void)icda_mkdir("/tmp"); (void)icda_mkdir("/tmp/ai");
                long pid=(long)icda_spawn_args("/sbin/model-install",v->model);
                if(pid<0) ic_strcpy(A.download_status,"Could not start model installer",sizeof(A.download_status));
                A.editing=0; return 1;
            }
            if (!v->cloud && !ic_strcmp(v->url, "file:auto") &&
                ic_ui_hit(field_rect(area, F_MODEL), ev->x, ev->y)) {
                choose_local_model();
                A.editing = 0;
                return 1;
            }
            if (ic_ui_hit(speak_rect(area), ev->x, ev->y)) { ai_toggle_speak(); return 1; }
            for (int f = 0; f < F_COUNT; f++) {
                if (ic_ui_hit(field_rect(area, f), ev->x, ev->y)) {
                    A.focus = f;
                    A.editing = 1;
                    return 1;
                }
            }
        }
        A.editing = 0;                   /* a click on empty space lets the field go */
    }
    return 1;
}
