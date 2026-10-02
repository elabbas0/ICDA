











#include "libicda.h"

#define WIN_W  620
#define WIN_H  700

#define GROUP_PAD  IC_SP_5
#define ROW_H      IC_H_ROW_TALL


typedef enum {
    ROW_BUTTONS = 0,   
    ROW_TOGGLE,         
    ROW_SEGMENTED,      
    ROW_SLIDER,         
    ROW_PROGRESS,       
    ROW_TEXTFIELD,      
    ROW_LIST,           
    ROW_TABLE,          
    ROW_SIDEBAR,        
    ROW_SYMBOLS,        
    ROW_MENU            
} row_kind_t;

typedef struct {
    row_kind_t kind;
    const char *label;
    const char *note;
} row_t;

static const row_t ROWS[] = {
    { ROW_BUTTONS,    "Buttons",        "Default, primary, destructive and plain" },
    { ROW_TOGGLE,     "Switch",         "Animates over the base duration" },
    { ROW_SEGMENTED,  "Segmented",      "Two options, sliding pill" },
    { ROW_SLIDER,     "Slider",         "Drag the handle" },
    { ROW_PROGRESS,   "Progress",       "Determinate and indeterminate" },
    { ROW_TEXTFIELD,  "Text field",     "Click and type" },
    { ROW_LIST,       "List",           "Rows highlight while the list has focus" },
    { ROW_TABLE,      "Table",          "Right-aligned numbers in mono" },
    { ROW_SIDEBAR,    "Sidebar",        "Source list" },
    { ROW_SYMBOLS,    "Symbols",        "The vector set" },
    { ROW_MENU,       "Menu",           "Material over blur" }
};
#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

enum { SYM_FIRST = IC_SYM_CLOSE, SYM_LAST = IC_SYM_TRASH };

static struct {
    int         scroll;
    int         content_h;
    int         view_h;
    int         selected_row;        
    int         hover_list;
    int         hover_toggle;
    int         hover_segment;
    int         hover_button;
    int         dragging_slider;
    int         list_focused;
    float       toggle;
    float       segment;
    float       slider;
    float       progress;
    char        field[64];
    int         field_cursor;
    int         field_scroll;
    int         selected_sidebar;
    int         hover_menu;
    ic_tween_t  toggle_tw;
    ic_tween_t  segment_tw;
    ic_tween_t  row_tw[ROW_COUNT];
    ic_textfield_t tf;
    char        status[64];
} gd;


#define GD_SCRATCH_PX (400 * 300)
static uint32_t scratch[GD_SCRATCH_PX];



static ic_rect_t view_rect(ic_app_t *app) {
    return ic_rect_make(0, IC_H_TITLEBAR, app->width, app->height - IC_H_TITLEBAR);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - 24, app->width, 24);
}



static int group_y(int i) {
    int y = GROUP_PAD;
    for (int k = 0; k < i; k++) y += ROW_H + 8 + ROW_H + IC_SP_4;
    return y;
}

static int group_h(void) { return ROW_H + 8 + ROW_H; }

static int group_at(int y) {
    for (int i = 0; i < ROW_COUNT; i++) {
        int top = group_y(i) - gd.scroll;
        if (y >= top && y < top + group_h()) return i;
    }
    return -1;
}

static int control_top(int i) { return group_y(i) + ROW_H + 8; }


static ic_rect_t control_rect(ic_app_t *app, int i) {
    int top = control_top(i) - gd.scroll;
    return ic_rect_make(GROUP_PAD, top, app->width - 2 * GROUP_PAD, ROW_H);
}

static ic_rect_t toggle_rect(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    return ic_ui_toggle_rect(r.x + r.w - IC_TOGGLE_W, r.y + (r.h - IC_TOGGLE_H) / 2);
}

static ic_rect_t slider_rect(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    int w = r.w / 2;
    return ic_rect_make(r.x, r.y + (r.h - 6) / 2, w, 6);
}

static ic_rect_t progress_rect(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    int w = r.w / 2 - IC_SP_3;
    return ic_rect_make(r.x, r.y + (r.h - 6) / 2, w, 6);
}

static ic_rect_t field_rect(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    return ic_rect_make(r.x, r.y + (r.h - IC_H_CONTROL) / 2, r.w - IC_SP_6, IC_H_CONTROL);
}

static ic_rect_t segmented_rect(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    int w = 176;
    return ic_rect_make(r.x + r.w - w, r.y + (r.h - IC_H_CONTROL_SM) / 2, w, IC_H_CONTROL_SM);
}

static ic_rect_t list_row_rect(ic_app_t *app, int i, int k) {
    ic_rect_t r = control_rect(app, i);
    return ic_rect_make(r.x, r.y + k * IC_H_ROW, r.w, IC_H_ROW);
}

static ic_rect_t sidebar_rect_in(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    return ic_rect_make(r.x, r.y, 180, 3 * IC_H_ROW + IC_SP_2);
}

static ic_rect_t menu_rect_in(ic_app_t *app, int i) {
    ic_rect_t r = control_rect(app, i);
    return ic_rect_make(r.x, r.y, 200, 3 * IC_H_MENU_ITEM + 2 * IC_SP_2);
}

static void layout(ic_app_t *app) {
    ic_rect_t v = view_rect(app);
    gd.view_h = v.h;
    gd.content_h = group_y(ROW_COUNT) + IC_SP_6;
    if (gd.scroll > gd.content_h - gd.view_h) gd.scroll = gd.content_h - gd.view_h;
    if (gd.scroll < 0) gd.scroll = 0;
}



static void draw_buttons(ic_app_t *app, ic_canvas_t *c, int i) {
    ic_rect_t r = control_rect(app, i);
    int y = r.y + (r.h - IC_H_CONTROL) / 2;
    int x = r.x;
    int hover = gd.hover_button;
    ic_state_t st = (hover == 0) ? IC_STATE_HOVER : IC_STATE_NORMAL;

    ic_ui_button(c, ic_rect_make(x, y, ic_ui_button_width("Default", IC_SYM_NONE), IC_H_CONTROL),
                 "Default", IC_SYM_NONE, IC_BUTTON_DEFAULT, st);
    x += ic_ui_button_width("Default", IC_SYM_NONE) + IC_SP_2;
    ic_ui_button(c, ic_rect_make(x, y, ic_ui_button_width("Primary", IC_SYM_PLAY), IC_H_CONTROL),
                 "Primary", IC_SYM_PLAY, IC_BUTTON_PRIMARY, IC_STATE_NORMAL);
    x += ic_ui_button_width("Primary", IC_SYM_PLAY) + IC_SP_2;
    ic_ui_button(c, ic_rect_make(x, y, ic_ui_button_width("Delete", IC_SYM_TRASH), IC_H_CONTROL),
                 "Delete", IC_SYM_TRASH, IC_BUTTON_DESTRUCTIVE, IC_STATE_NORMAL);
    x += ic_ui_button_width("Delete", IC_SYM_TRASH) + IC_SP_2;
    ic_ui_button(c, ic_rect_make(x, y, ic_ui_button_width("Disabled", IC_SYM_NONE), IC_H_CONTROL),
                 "Disabled", IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_DISABLED);
    x += ic_ui_button_width("Disabled", IC_SYM_NONE) + IC_SP_2;
    ic_ui_icon_button(c, ic_rect_make(x, y + 1, 28, 28), IC_SYM_RELOAD,
                      hover == 4 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    x += 28 + IC_SP_2;
    ic_ui_button(c, ic_rect_make(x, y, ic_ui_button_width("Plain", IC_SYM_NONE), IC_H_CONTROL),
                 "Plain", IC_SYM_NONE, IC_BUTTON_PLAIN, hover == 5 ? IC_STATE_HOVER
                                                                  : IC_STATE_NORMAL);
}

static const char *const LIST_ITEMS[3] = { "Inter views", "Kernel sources", "Release notes" };
static const char *const SIDEBAR_ITEMS[3] = { "Documents", "Pictures", "Music" };
static const char *const TABLE_HEAD[3] = { "NAME", "SIZE", "KIND" };
static const int TABLE_WIDTHS[3] = { 240, 120, 140 };
static const char *const TABLE_ROWS[2][3] = {
    { "kernel.iso", "34 MB", "Image" },
    { "notes.txt", "2 KB", "Text" }
};
static const char *const MENU_LABELS[3] = { "Open", "Rename", IC_MENU_SEPARATOR };
static const char *const MENU_SHORTCUTS[3] = { "", "F2", "" };

static void draw_group(ic_app_t *app, ic_canvas_t *c, int i) {
    const ic_palette_t *p = ic_palette();
    int top = group_y(i) - gd.scroll;
    int ctrl = control_top(i) - gd.scroll;
    float hover = ic_tween_value(&gd.row_tw[i]);
    int h = group_h();

    if (ctrl + h < 0 || top > gd.view_h) return;      

    ic_ui_group(c, ic_rect_make(GROUP_PAD, ctrl, app->width - 2 * GROUP_PAD, h));
    ic_ui_group_row(c, ic_rect_make(GROUP_PAD, top, app->width - 2 * GROUP_PAD, ROW_H), 0, 1,
                    hover);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE),
                    ic_rect_make(GROUP_PAD + IC_SP_3, top, app->width / 2, ROW_H),
                    ROWS[i].label, p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(app->width / 2, top, app->width / 2 - GROUP_PAD - IC_SP_3, ROW_H),
                    ROWS[i].note, p->label_tertiary, IC_ALIGN_RIGHT);

    switch (ROWS[i].kind) {
    case ROW_BUTTONS:
        draw_buttons(app, c, i);
        break;
    case ROW_TOGGLE:
        ic_ui_toggle(c, toggle_rect(app, i).x, toggle_rect(app, i).y, gd.toggle,
                     gd.hover_toggle ? IC_STATE_HOVER : IC_STATE_NORMAL);
        break;
    case ROW_SEGMENTED: {
        static const char *const opts[2] = { "Grid", "List" };
        ic_ui_segmented(c, segmented_rect(app, i), opts, 2, gd.segment, gd.hover_segment);
        break;
    }
    case ROW_SLIDER:
        ic_ui_slider(c, slider_rect(app, i), gd.slider,
                     gd.dragging_slider ? IC_STATE_PRESSED : IC_STATE_NORMAL);
        {
            char v[16];
            char *pct = v;
            int n = (int)(gd.slider * 100.0f);
            *pct++ = (char)('0' + (n / 100) % 10);
            *pct++ = (char)('0' + (n / 10) % 10);
            *pct++ = '%';
            *pct = 0;
            ic_text_draw_in(c, ic_font(IC_FONT_MONO_SMALL),
                            ic_rect_make(slider_rect(app, i).x + slider_rect(app, i).w + IC_SP_3,
                                         control_rect(app, i).y, 60, ROW_H),
                            v, p->label_secondary, IC_ALIGN_LEFT);
        }
        break;
    case ROW_PROGRESS:
        ic_ui_progress(c, progress_rect(app, i), gd.progress, p->accent);
        ic_ui_progress(c, ic_rect_make(progress_rect(app, i).x + progress_rect(app, i).w + IC_SP_5,
                                       progress_rect(app, i).y, progress_rect(app, i).w,
                                       progress_rect(app, i).h),
                       -1.0f, p->label_tertiary);
        break;
    case ROW_TEXTFIELD: {
        ic_textfield_t tf = gd.tf;
        tf.text = gd.field;
        tf.cursor = gd.field_cursor;
        tf.sel_start = tf.sel_end = gd.field_cursor;
        tf.focused = 1;
        tf.caret_on = ic_app_caret_visible(app);
        tf.placeholder = "Type here";
        tf.leading = IC_SYM_SEARCH;
        tf.scroll_px = gd.field_scroll;
        ic_ui_textfield(c, field_rect(app, i), &tf);
        break;
    }
    case ROW_LIST:
        for (int k = 0; k < 3; k++) {
            ic_rect_t lr = list_row_rect(app, i, k);
            ic_color_t text = ic_ui_list_row(c, lr, k == gd.selected_row, gd.list_focused,
                                             k == gd.hover_list ? 1.0f : 0.0f);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY), lr, LIST_ITEMS[k], text, IC_ALIGN_LEFT);
        }
        break;
    case ROW_TABLE:
        ic_ui_table_header(c, ic_rect_make(control_rect(app, i).x, control_rect(app, i).y,
                                           control_rect(app, i).w, IC_H_ROW),
                           TABLE_HEAD, TABLE_WIDTHS, 3);
        for (int k = 0; k < 2; k++) {
            ic_rect_t lr = list_row_rect(app, i, k + 1);
            for (int col = 0; col < 3; col++) {
                ic_text_draw_in(c, col == 1 ? ic_font(IC_FONT_MONO_SMALL) : ic_font(IC_FONT_BODY),
                                ic_rect_make(lr.x + TABLE_WIDTHS[0] * col, lr.y,
                                             TABLE_WIDTHS[0] - IC_SP_2, lr.h),
                                TABLE_ROWS[k][col],
                                col == 2 ? p->label_secondary : p->label,
                                col == 0 ? IC_ALIGN_LEFT : IC_ALIGN_RIGHT);
            }
        }
        break;
    case ROW_SIDEBAR: {
        ic_rect_t s = sidebar_rect_in(app, i);
        ic_ui_sidebar_bg(c, s);
        for (int k = 0; k < 3; k++) {
            ic_rect_t ir = ic_rect_make(s.x, s.y + IC_SP_1 + k * IC_H_ROW, s.w, IC_H_ROW);
            ic_ui_sidebar_item(c, ir, k == 2 ? IC_SYM_MUSIC : IC_SYM_FOLDER, SIDEBAR_ITEMS[k],
                               k == gd.selected_sidebar, 0.0f);
        }
        break;
    }
    case ROW_SYMBOLS: {
        int n = 0;
        int cx = control_rect(app, i).x + IC_SP_2;
        int cy = control_rect(app, i).y + ROW_H / 2;
        for (int s = SYM_FIRST; s <= SYM_LAST && n < 12; s += 2) {
            ic_symbol_draw(c, (ic_symbol_t)s, (float)cx, (float)cy, 18.0f, p->label);
            cx += 26;
            n++;
        }
        break;
    }
    case ROW_MENU: {
        ic_menu_model_t m;
        ic_rect_t mr = menu_rect_in(app, i);
        for (int k = 0; k < 3; k++) {
            m.labels[k] = MENU_LABELS[k];
            m.shortcuts[k] = MENU_SHORTCUTS[k];
            m.disabled[k] = 0;
        }
        m.count = 3;
        m.hover = gd.hover_menu;
        ic_ui_menu(c, &m, mr.x, mr.y, scratch, GD_SCRATCH_PX);
        break;
    }
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t v = view_rect(app);
    ic_rect_t saved;
    const ic_palette_t *p = ic_palette();

    layout(app);
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));

    
    ic_ui_toolbar(c, ic_rect_make(0, 0, app->width, IC_H_TITLEBAR));
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(GROUP_PAD, 0,
                                                                 app->width - 2 * GROUP_PAD,
                                                                 IC_H_TITLEBAR),
                    "ICDA Demo", p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(app->width / 2, 0, app->width / 2 - GROUP_PAD, IC_H_TITLEBAR),
                    "Every control, in the current appearance", p->label_tertiary,
                    IC_ALIGN_RIGHT);

    ic_canvas_push_clip(c, v.x, v.y, v.w, v.h, &saved);
    for (int i = 0; i < ROW_COUNT; i++) draw_group(app, c, i);
    ic_canvas_pop_clip(c, &saved);

    if (gd.content_h > gd.view_h) ic_ui_scrollbar(c, v, gd.scroll, gd.content_h, 1.0f);

    ic_ui_statusbar(c, status_rect(app), gd.status);

    for (int i = 0; i < ROW_COUNT; i++) {
        if (ic_tween_running(&gd.row_tw[i])) ic_app_animate(app);
    }
    if (ic_tween_running(&gd.toggle_tw) || ic_tween_running(&gd.segment_tw)) ic_app_animate(app);
    
    if (app->focused) ic_app_animate(app);
}



static int row_of_kind(row_kind_t kind) {
    for (int i = 0; i < ROW_COUNT; i++) {
        if (ROWS[i].kind == kind) return i;
    }
    return -1;
}

static void set_status(const char *text) {
    ic_strcpy(gd.status, text, sizeof(gd.status));
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    int g = -1;

    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        g = group_at(ev->y);
        gd.hover_toggle = 0;
        gd.hover_segment = 0;
        gd.hover_list = -1;
        gd.hover_menu = -1;
        gd.hover_button = -1;
        if (g >= 0) {
            switch (ROWS[g].kind) {
            case ROW_TOGGLE:
                gd.hover_toggle = ic_ui_hit(toggle_rect(app, g), ev->x, ev->y);
                break;
            case ROW_SEGMENTED:
                gd.hover_segment = ic_ui_segmented_hit(segmented_rect(app, g), 2, ev->x, ev->y);
                break;
            case ROW_LIST:
                for (int k = 0; k < 3; k++) {
                    if (ic_ui_hit(list_row_rect(app, g, k), ev->x, ev->y)) gd.hover_list = k;
                }
                break;
            case ROW_MENU: {
                ic_menu_model_t m;
                ic_rect_t mr = menu_rect_in(app, g);
                for (int k = 0; k < 3; k++) {
                    m.labels[k] = MENU_LABELS[k];
                    m.shortcuts[k] = 0;
                    m.disabled[k] = 0;
                }
                m.count = 3;
                gd.hover_menu = ic_ui_menu_hit(&m, mr.x, mr.y, ev->x, ev->y);
                break;
            }
            case ROW_BUTTONS: {
                ic_rect_t r = control_rect(app, g);
                int y = r.y + (r.h - IC_H_CONTROL) / 2;
                if (ev->x >= r.x && ev->x < r.x + 24 && ic_ui_hit(
                        ic_rect_make(r.x, y, ic_ui_button_width("Default", IC_SYM_NONE),
                                     IC_H_CONTROL), ev->x, ev->y)) {
                    gd.hover_button = 0;
                } else if (ev->x < r.x + 320) {
                    gd.hover_button = 5;
                } else if (ev->x > r.x + r.w - 32) {
                    gd.hover_button = 4;
                } else {
                    gd.hover_button = -1;
                }
                break;
            }
            default:
                break;
            }
        }
        if (gd.dragging_slider) {
            int si = row_of_kind(ROW_SLIDER);
            if (si >= 0) gd.slider = ic_ui_slider_value(slider_rect(app, si), ev->x);
        }
        break;
    case IC_EV_MOUSE_DOWN:
        g = group_at(ev->y);
        if (ev->button != GUI_BTN_LEFT || g < 0) break;
        switch (ROWS[g].kind) {
        case ROW_TOGGLE:
            if (ic_ui_hit(toggle_rect(app, g), ev->x, ev->y)) {
                ic_tween_to(&gd.toggle_tw, gd.toggle > 0.5f ? 0.0f : 1.0f, IC_DUR_BASE,
                            IC_EASE_MOVE);
                set_status("Switch toggled");
            }
            break;
        case ROW_SEGMENTED: {
            int s = ic_ui_segmented_hit(segmented_rect(app, g), 2, ev->x, ev->y);
            if (s >= 0) {
                ic_tween_to(&gd.segment_tw, (float)s, IC_DUR_BASE, IC_EASE_MOVE);
                set_status(s == 0 ? "Grid view" : "List view");
            }
            break;
        }
        case ROW_SLIDER:
            if (ic_ui_hit(control_rect(app, g), ev->x, ev->y)) {
                gd.dragging_slider = 1;
                gd.slider = ic_ui_slider_value(slider_rect(app, g), ev->x);
            }
            break;
        case ROW_TEXTFIELD:
            if (ic_ui_hit(field_rect(app, g), ev->x, ev->y)) {
                ic_textfield_t tf = gd.tf;
                tf.text = gd.field;
                tf.cursor = gd.field_cursor;
                tf.sel_start = tf.sel_end = gd.field_cursor;
                gd.field_cursor = ic_ui_textfield_index_at(field_rect(app, g), &tf, ev->x);
                ic_ui_textfield_scroll(field_rect(app, g), &gd.tf);
            }
            break;
        case ROW_LIST:
            for (int k = 0; k < 3; k++) {
                if (ic_ui_hit(list_row_rect(app, g, k), ev->x, ev->y)) {
                    gd.selected_row = k;
                    gd.list_focused = 1;
                    set_status("List row selected");
                }
            }
            break;
        case ROW_SIDEBAR: {
            ic_rect_t s = sidebar_rect_in(app, g);
            for (int k = 0; k < 3; k++) {
                ic_rect_t ir = ic_rect_make(s.x, s.y + IC_SP_1 + k * IC_H_ROW, s.w, IC_H_ROW);
                if (ic_ui_hit(ir, ev->x, ev->y)) {
                    gd.selected_sidebar = k;
                    set_status("Sidebar item selected");
                }
            }
            break;
        }
        case ROW_PROGRESS:
            if (ic_ui_hit(control_rect(app, g), ev->x, ev->y)) {
                gd.progress = (gd.progress > 0.5f) ? 0.0f : 1.0f;
                set_status("Progress reset");
            }
            break;
        default:
            break;
        }
        break;
    case IC_EV_MOUSE_UP:
        gd.dragging_slider = 0;
        break;
    case IC_EV_MOUSE_LEAVE:
        gd.hover_toggle = gd.hover_segment = gd.hover_button = 0;
        gd.hover_list = -1;
        gd.hover_menu = -1;
        break;
    case IC_EV_KEY: {
        int fi = row_of_kind(ROW_TEXTFIELD);
        ic_rect_t fr = fi >= 0 ? field_rect(app, fi) : ic_rect_make(0, 0, 0, 0);
        switch (ev->key) {
        case IC_KEY_PAGE_DOWN: gd.scroll += gd.view_h - 40; break;
        case IC_KEY_PAGE_UP:   gd.scroll -= gd.view_h - 40; break;
        case IC_KEY_DOWN:      gd.scroll += 20; break;
        case IC_KEY_UP:        gd.scroll -= 20; break;
        case IC_KEY_LEFT:
            if (gd.field_cursor > 0) gd.field_cursor--;
            break;
        case IC_KEY_RIGHT:
            if (gd.field_cursor < (int)ic_strlen(gd.field)) gd.field_cursor++;
            break;
        case IC_KEY_HOME: gd.field_cursor = 0; break;
        case IC_KEY_END:  gd.field_cursor = (int)ic_strlen(gd.field); break;
        case IC_KEY_BACKSPACE:
            if (gd.field_cursor > 0) {
                int len = (int)ic_strlen(gd.field);
                for (int k = gd.field_cursor; k < len; k++) gd.field[k - 1] = gd.field[k];
                gd.field[len - 1] = 0;
                gd.field_cursor--;
            }
            break;
        case IC_KEY_DELETE: {
            int len = (int)ic_strlen(gd.field);
            if (gd.field_cursor < len) {
                for (int k = gd.field_cursor; k < len - 1; k++) gd.field[k] = gd.field[k + 1];
                gd.field[len - 1] = 0;
            }
            break;
        }
        default:
            if (ev->key >= 32 && ev->key < 127) {
                int len = (int)ic_strlen(gd.field);
                if (len + 1 < (int)sizeof(gd.field)) {
                    for (int k = len; k > gd.field_cursor; k--) gd.field[k] = gd.field[k - 1];
                    gd.field[gd.field_cursor++] = (char)ev->key;
                    gd.field[len + 1] = 0;
                }
            }
            break;
        }
        if (fi >= 0) ic_ui_textfield_scroll(fr, &gd.tf);
        break;
    }
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_APPEARANCE:
        
        break;
    case IC_EV_BLUR:
        gd.list_focused = 0;
        break;
    case IC_EV_FOCUS:
        gd.list_focused = 1;
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    (void)app;
    gd.scroll = 0;
    gd.selected_row = 1;
    gd.hover_list = -1;
    gd.hover_toggle = 0;
    gd.hover_segment = -1;
    gd.hover_button = -1;
    gd.hover_menu = -1;
    gd.dragging_slider = 0;
    gd.list_focused = 1;
    gd.selected_sidebar = 0;
    gd.progress = 0.65f;
    ic_tween_set(&gd.toggle_tw, 1.0f);
    ic_tween_set(&gd.segment_tw, 0.0f);
    for (int i = 0; i < ROW_COUNT; i++) ic_tween_set(&gd.row_tw[i], 0.0f);
    gd.slider = 0.4f;
    ic_strcpy(gd.field, "Search the library", sizeof(gd.field));
    gd.field_cursor = (int)ic_strlen(gd.field);
    set_status("Interact with every control");
    layout(app);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "ICDA Demo", WIN_W, WIN_H, init, draw, event, 0 };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("the demo requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
