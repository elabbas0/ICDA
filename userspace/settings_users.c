/* Settings > Users (see settings_users.h). */
#include "settings_users.h"
#include "ic_ui.h"
#include "ic_time.h"

#define U_MAX     12
#define U_ROW_H   44
#define U_SIDE    200
#define PW_CAP    128

typedef struct {
    char name[32];
    int  admin, automatic, password;
} account_t;

/* the form's text fields */
enum { F_MINE = 0, F_NEWPW, F_ADD_NAME, F_ADD_PW, F_COUNT };

static struct {
    account_t acct[U_MAX];
    int  count, sel;            /* sel == count: the "new account" form */
    char me[32];
    int  me_admin;
    char field[F_COUNT][PW_CAP];
    int  focus, editing;
    int  add_admin;
    int  confirm_remove;
    int  hover;
    char status[96];
    int  status_bad;
} U;

static void wipe_field(int f) {
    for (int i = 0; i < PW_CAP; i++) ((volatile char *)U.field[f])[i] = 0;
}

static void load(void) {
    char buf[1024], who[96];
    long n = icda_account_list(buf, sizeof(buf));
    long w = icda_account_whoami(who, sizeof(who));
    int i = 0;
    U.count = 0;
    while (n > 0 && i < n && U.count < U_MAX) {
        account_t *a = &U.acct[U.count];
        int k = 0;
        while (i < n && buf[i] != ' ' && k < 31) a->name[k++] = buf[i++];
        a->name[k] = 0;
        while (i < n && buf[i] == ' ') i++;
        while (i < n && buf[i] != ' ') i++;
        a->admin = a->automatic = a->password = 0;
        for (; i < n && buf[i] != '\n'; i++) {
            if (buf[i] == 'a') a->admin = 1;
            if (buf[i] == 'l') a->automatic = 1;
            if (buf[i] == 'p') a->password = 1;
        }
        i++;
        if (a->name[0]) U.count++;
    }
    U.me[0] = 0;
    for (int k = 0; w > 0 && k < w && who[k] != ' ' && k < 31; k++) U.me[k] = who[k], U.me[k + 1] = 0;
    U.me_admin = 0;
    for (int k = 0; k < U.count; k++) if (!ic_strcmp(U.acct[k].name, U.me)) U.me_admin = U.acct[k].admin;
    if (U.sel > U.count) U.sel = 0;
}

static void say(const char *s, int bad) {
    ic_strcpy(U.status, s, sizeof(U.status));
    U.status_bad = bad;
}

void users_pane_enter(void) {
    ic_memset(&U, 0, sizeof(U));
    load();
    for (int k = 0; k < U.count; k++) if (!ic_strcmp(U.acct[k].name, U.me)) U.sel = k;
    U.hover = -1;
}

void users_pane_leave(void) {
    for (int f = 0; f < F_COUNT; f++) wipe_field(f);
}

int users_pane_modal(void) {
    return U.editing;
}

static account_t *cur(void) {
    return U.sel >= 0 && U.sel < U.count ? &U.acct[U.sel] : 0;
}

static int is_me(const account_t *a) { return a && !ic_strcmp(a->name, U.me); }
static int is_owner(int i) { return i == 0; }       /* the first account */

/* ---- layout ---------------------------------------------------------------- */

static ic_rect_t list_rect(ic_rect_t area) {
    int w = area.w / 3 < U_SIDE ? area.w / 3 : U_SIDE;
    return ic_rect_make(area.x, area.y, w, area.h);
}

static ic_rect_t row_rect(ic_rect_t list, int i) {
    return ic_rect_make(list.x + 6, list.y + 6 + i * U_ROW_H, list.w - 12, U_ROW_H - 4);
}

static ic_rect_t add_rect(ic_rect_t list) {
    return ic_rect_make(list.x + 6, list.y + list.h - IC_H_CONTROL - 6, list.w - 12, IC_H_CONTROL);
}

static int fx(ic_rect_t area) { return area.x + list_rect(area).w + IC_SP_4; }
static int fw(ic_rect_t area) { int w = area.x + area.w - fx(area) - IC_SP_3; return w > 360 ? 360 : w; }

/* parts of the form, by row (each row 52 px: a label and a control) */
enum { P_TOGGLE_ADMIN = 0, P_TOGGLE_AUTO, P_NEWPW, P_CHANGE, P_REMOVE,
       P_ADD_NAME, P_ADD_PW, P_ADD_ADMIN, P_CREATE, P_MINE, P_COUNT };

static ic_rect_t part(ic_rect_t area, int which) {
    int x = fx(area), w = fw(area), y = area.y + 64;
    if (U.sel == U.count) {                             /* the new account form */
        switch (which) {
        case P_ADD_NAME:  return ic_rect_make(x, y + 18, w, 30);
        case P_ADD_PW:    return ic_rect_make(x, y + 70, w, 30);
        case P_ADD_ADMIN: return ic_ui_toggle_rect(x + w - IC_TOGGLE_W, y + 116);
        case P_MINE:      return ic_rect_make(x, y + 170, w, 30);
        case P_CREATE:    return ic_rect_make(x, y + 214, 140, IC_H_CONTROL);
        default:          return ic_rect_make(0, 0, -1, 0);
        }
    }
    switch (which) {
    case P_TOGGLE_ADMIN: return ic_ui_toggle_rect(x + w - IC_TOGGLE_W, y + 4);
    case P_TOGGLE_AUTO:  return ic_ui_toggle_rect(x + w - IC_TOGGLE_W, y + 44);
    case P_NEWPW:        return ic_rect_make(x, y + 104, w - 150, 30);
    case P_CHANGE:       return ic_rect_make(x + w - 140, y + 104, 140, 30);
    case P_MINE:         return ic_rect_make(x, y + 170, w, 30);
    case P_REMOVE:       return ic_rect_make(x, y + 214, 170, IC_H_CONTROL);
    default:             return ic_rect_make(0, 0, -1, 0);
    }
}

/* who may do what here: the kernel decides; this only greys out the rest */
static int can_admin_toggle(void) { account_t *a = cur(); return a && U.me_admin && !is_owner(U.sel); }
static int can_auto_toggle(void) { account_t *a = cur(); return a && (U.me_admin || is_me(a)); }
static int can_change_pw(void) { account_t *a = cur(); return a && (U.me_admin || is_me(a)); }
static int can_remove(void) { account_t *a = cur(); return a && U.me_admin && !is_me(a) && !is_owner(U.sel); }

/* ---- drawing ---------------------------------------------------------------- */

static void avatar(ic_canvas_t *c, int cx, int cy, int r, const char *name) {
    const ic_palette_t *p = ic_palette();
    char initial[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    ic_gfx_circle(c, (float)cx, (float)cy, (float)r, p->accent);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(cx - r, cy - r, 2 * r, 2 * r), initial,
                    p->label_on_accent, IC_ALIGN_CENTER);
}

static void field_box(ic_canvas_t *c, ic_rect_t r, int f, const char *placeholder, int secret, int enabled) {
    char shown[PW_CAP];
    ic_textfield_t tf;
    int n = (int)ic_strlen(U.field[f]);
    for (int i = 0; i < n; i++) shown[i] = secret ? '*' : U.field[f][i];
    shown[n] = 0;
    ic_memset(&tf, 0, sizeof(tf));
    tf.text = shown;
    tf.cursor = tf.sel_start = tf.sel_end = n;
    tf.focused = enabled && U.editing && U.focus == f;
    tf.caret_on = tf.focused && ((ic_time_ms() / 500) % 2 == 0);
    tf.placeholder = enabled ? placeholder : "";
    tf.leading = secret ? IC_SYM_LOCK : IC_SYM_NONE;
    ic_ui_textfield_scroll(r, &tf);
    ic_ui_textfield(c, r, &tf);
}

static void label(ic_canvas_t *c, int x, int y, int w, const char *title, const char *sub) {
    const ic_palette_t *p = ic_palette();
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(x, y, w, 18), title, p->label, IC_ALIGN_LEFT);
    if (sub) ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(x, y + 18, w, 14), sub,
                             p->label_tertiary, IC_ALIGN_LEFT);
}

static ic_state_t st(int enabled, int which) {
    return ic_ui_state(enabled, U.hover == which, 0);
}

void users_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t list = list_rect(area);
    int x = fx(area), w = fw(area), y = area.y;
    (void)app;

    ic_ui_panel(c, list, IC_R_GROUP + 2.0f, IC_ELEV_MENU, 0, 0);
    for (int i = 0; i < U.count; i++) {
        ic_rect_t r = row_rect(list, i);
        account_t *a = &U.acct[i];
        if (i == U.sel) ic_gfx_rrect(c, r.x + 2, r.y + 1, r.w - 4, r.h - 1, IC_R_ROW, p->fill_selected_idle);
        avatar(c, r.x + 20, r.y + r.h / 2, 14, a->name);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + 42, r.y + 4, r.w - 48, 16), a->name,
                        p->label, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 42, r.y + 21, r.w - 48, 14),
                        is_me(a) ? (a->admin ? "You, administrator" : "You")
                                 : a->admin ? "Administrator" : "Standard user",
                        p->label_tertiary, IC_ALIGN_LEFT);
    }
    if (U.me_admin)
        ic_ui_button(c, add_rect(list), "Add account", IC_SYM_PLUS, IC_BUTTON_DEFAULT,
                     U.sel == U.count ? IC_STATE_PRESSED : st(1, 100));

    if (U.sel == U.count) {
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(x, y + 12, w, 24), "New account", p->label,
                        IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(x, y + 38, w, 14),
                        "Their files live in /users and are private to them.", p->label_tertiary, IC_ALIGN_LEFT);
        label(c, x, part(area, P_ADD_NAME).y - 18, w, "Name", 0);
        field_box(c, part(area, P_ADD_NAME), F_ADD_NAME, "lowercase letters and digits", 0, 1);
        label(c, x, part(area, P_ADD_PW).y - 18, w, "Password", 0);
        field_box(c, part(area, P_ADD_PW), F_ADD_PW, "optional", 1, 1);
        label(c, x, part(area, P_ADD_ADMIN).y, w - 50, "Administrator", "Can add accounts and use internal disks");
        ic_ui_toggle(c, part(area, P_ADD_ADMIN).x, part(area, P_ADD_ADMIN).y, U.add_admin ? 1.0f : 0.0f,
                     st(1, P_ADD_ADMIN));
        label(c, x, part(area, P_MINE).y - 18, w, "Your password, to confirm", 0);
        field_box(c, part(area, P_MINE), F_MINE, "", 1, 1);
        ic_ui_button(c, part(area, P_CREATE), "Create", IC_SYM_NONE, IC_BUTTON_PRIMARY, st(1, P_CREATE));
    } else if (cur()) {
        account_t *a = cur();
        avatar(c, x + 18, y + 26, 18, a->name);
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(x + 46, y + 10, w - 46, 22), a->name, p->label,
                        IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(x + 46, y + 32, w - 46, 14),
                        is_owner(U.sel) ? "Owner of this computer: files in /home" : "Files in /users",
                        p->label_tertiary, IC_ALIGN_LEFT);
        label(c, x, part(area, P_TOGGLE_ADMIN).y, w - 50, "Administrator", "Accounts, disks, other users' programs");
        ic_ui_toggle(c, part(area, P_TOGGLE_ADMIN).x, part(area, P_TOGGLE_ADMIN).y, a->admin ? 1.0f : 0.0f,
                     st(can_admin_toggle(), P_TOGGLE_ADMIN));
        label(c, x, part(area, P_TOGGLE_AUTO).y, w - 50, "Sign in automatically", "No sign-in screen at startup");
        ic_ui_toggle(c, part(area, P_TOGGLE_AUTO).x, part(area, P_TOGGLE_AUTO).y, a->automatic ? 1.0f : 0.0f,
                     st(can_auto_toggle(), P_TOGGLE_AUTO));
        label(c, x, part(area, P_NEWPW).y - 18, w, a->password ? "New password" : "Password (none yet)", 0);
        field_box(c, part(area, P_NEWPW), F_NEWPW, "empty: no password", 1, can_change_pw());
        ic_ui_button(c, part(area, P_CHANGE), "Change password", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                     st(can_change_pw(), P_CHANGE));
        label(c, x, part(area, P_MINE).y - 18, w, "Your password, to confirm changes", 0);
        field_box(c, part(area, P_MINE), F_MINE, "", 1, 1);
        if (can_remove())
            ic_ui_button(c, part(area, P_REMOVE), U.confirm_remove ? "Click again to remove" : "Remove account",
                         IC_SYM_TRASH, IC_BUTTON_DESTRUCTIVE, st(1, P_REMOVE));
    }
    if (U.status[0]) {
        int sy = part(area, U.sel == U.count ? P_CREATE : P_REMOVE).y + IC_H_CONTROL + 14;
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(x, sy, w, 16), U.status,
                        U.status_bad ? p->danger : p->success, IC_ALIGN_LEFT);
    }
}

/* ---- acting ------------------------------------------------------------------- */

static const char *why(long rc) {
    switch (rc) {
    case -1:  return "Only an administrator can do that";
    case -13: return "That password is not right";
    case -17: return "That name is taken";
    case -22: return "Use lowercase letters, digits, - and _";
    default:  return "That did not work";
    }
}

static void done(long rc, const char *ok) {
    if (rc < 0) {
        say(why(rc), 1);
    } else {
        say(ok, 0);
    }
    wipe_field(F_MINE);
    load();
}

static void set_flags(int admin, int automatic) {
    account_t *a = cur();
    uint32_t f = (admin ? ICDA_ACCT_ADMIN : 0) | (automatic ? ICDA_ACCT_AUTOLOGIN : 0);
    if (!a) return;
    done(icda_account_set(a->name, U.field[F_MINE], f), "Saved");
}

static void act(int which) {
    account_t *a = cur();
    U.confirm_remove = which == P_REMOVE ? U.confirm_remove : 0;
    switch (which) {
    case P_TOGGLE_ADMIN:
        if (can_admin_toggle()) set_flags(!a->admin, a->automatic);
        break;
    case P_TOGGLE_AUTO:
        if (can_auto_toggle()) set_flags(a->admin, !a->automatic);
        break;
    case P_CHANGE:
        if (!can_change_pw()) break;
        done(icda_account_password(a->name, U.field[F_MINE], U.field[F_NEWPW]), "Password changed");
        wipe_field(F_NEWPW);
        break;
    case P_REMOVE:
        if (!can_remove()) break;
        if (!U.confirm_remove) { U.confirm_remove = 1; say("Their files go too", 1); break; }
        U.confirm_remove = 0;
        done(icda_account_delete(a->name, U.field[F_MINE], 1), "Removed");
        U.sel = 0;
        break;
    case P_ADD_ADMIN:
        U.add_admin = !U.add_admin;
        break;
    case P_CREATE: {
        long rc = icda_account_create(U.field[F_ADD_NAME], U.field[F_ADD_PW], U.add_admin ? ICDA_ACCT_ADMIN : 0,
                                      U.field[F_MINE]);
        done(rc, "Account created");
        if (rc >= 0) {
            wipe_field(F_ADD_NAME);
            wipe_field(F_ADD_PW);
            U.add_admin = 0;
            U.sel = U.count - 1;
        }
        break;
    }
    default:
        break;
    }
}

int users_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area) {
    static const int clickable[] = { P_TOGGLE_ADMIN, P_TOGGLE_AUTO, P_CHANGE, P_REMOVE, P_ADD_ADMIN, P_CREATE };
    static const struct { int part, field; } fields[] = {
        { P_NEWPW, F_NEWPW }, { P_MINE, F_MINE }, { P_ADD_NAME, F_ADD_NAME }, { P_ADD_PW, F_ADD_PW } };
    ic_rect_t list = list_rect(area);
    (void)app;
    if (ev->type == IC_EV_KEY) {
        char *buf;
        int len;
        if (!U.editing) return 0;
        buf = U.field[U.focus];
        len = (int)ic_strlen(buf);
        if (ev->key == IC_KEY_ESCAPE) { U.editing = 0; return 1; }
        if (ev->key == IC_KEY_ENTER) {
            U.editing = 0;
            if (U.sel == U.count) act(P_CREATE);
            else if (U.focus == F_NEWPW) act(P_CHANGE);
            return 1;
        }
        if (ev->key == IC_KEY_TAB) {
            U.focus = U.sel == U.count ? (U.focus == F_ADD_NAME ? F_ADD_PW : U.focus == F_ADD_PW ? F_MINE : F_ADD_NAME)
                                       : (U.focus == F_NEWPW ? F_MINE : F_NEWPW);
            return 1;
        }
        if (ev->key == IC_KEY_BACKSPACE || ev->key == IC_KEY_DELETE) {
            if (len) buf[len - 1] = 0;
            return 1;
        }
        if (ev->key >= 32 && ev->key < 127 && len + 1 < PW_CAP) {
            char k = (char)ev->key;
            if (U.focus == F_ADD_NAME && k >= 'A' && k <= 'Z') k = (char)(k + 32);
            buf[len] = k;
            buf[len + 1] = 0;
        }
        return 1;
    }
    if (ev->type == IC_EV_MOUSE_MOVE) {
        int h = -1;
        if (!ic_ui_hit(area, ev->x, ev->y)) return 0;
        for (unsigned i = 0; i < sizeof(clickable) / sizeof(clickable[0]); i++)
            if (part(area, clickable[i]).w > 0 && ic_ui_hit(part(area, clickable[i]), ev->x, ev->y)) h = clickable[i];
        if (U.me_admin && ic_ui_hit(add_rect(list), ev->x, ev->y)) h = 100;
        if (h != U.hover) { U.hover = h; return 1; }
        return 0;
    }
    if (ev->type != IC_EV_MOUSE_DOWN) return 0;
    if (!ic_ui_hit(area, ev->x, ev->y)) { U.editing = 0; return 0; }
    if (U.me_admin && ic_ui_hit(add_rect(list), ev->x, ev->y)) {
        U.sel = U.count;
        U.focus = F_ADD_NAME;
        U.editing = 1;
        U.status[0] = 0;
        return 1;
    }
    for (int i = 0; i < U.count; i++) {
        if (ic_ui_hit(row_rect(list, i), ev->x, ev->y)) {
            U.sel = i;
            U.editing = 0;
            U.confirm_remove = 0;
            U.status[0] = 0;
            return 1;
        }
    }
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        ic_rect_t r = part(area, fields[i].part);
        if (r.w > 0 && ic_ui_hit(r, ev->x, ev->y)) {
            U.focus = fields[i].field;
            U.editing = 1;
            return 1;
        }
    }
    for (unsigned i = 0; i < sizeof(clickable) / sizeof(clickable[0]); i++) {
        ic_rect_t r = part(area, clickable[i]);
        if (r.w > 0 && ic_ui_hit(r, ev->x, ev->y)) {
            act(clickable[i]);
            return 1;
        }
    }
    U.editing = 0;
    return 1;
}
