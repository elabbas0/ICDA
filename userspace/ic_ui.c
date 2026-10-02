






#include "ic_ui.h"

static const ic_palette_t *P(void) { return ic_palette(); }

ic_state_t ic_ui_state(int enabled, int hover, int pressed) {
    if (!enabled) return IC_STATE_DISABLED;
    if (pressed) return IC_STATE_PRESSED;
    if (hover) return IC_STATE_HOVER;
    return IC_STATE_NORMAL;
}



#define IC_BTN_PAD_X     14
#define IC_BTN_SYM_GAP    6
#define IC_BTN_MIN_W     72

int ic_ui_button_width(const char *label, ic_symbol_t sym) {
    int w = IC_BTN_PAD_X * 2;
    int has_label = label && label[0];
    if (has_label) w += ic_text_measure(ic_font(IC_FONT_BODY), label);
    if (sym != IC_SYM_NONE) w += IC_ICON_SM + (has_label ? IC_BTN_SYM_GAP : 0);
    if (has_label && w < IC_BTN_MIN_W) w = IC_BTN_MIN_W;
    return w;
}

static void ic_ui_button_content(ic_canvas_t *c, ic_rect_t r, const char *label,
                                 ic_symbol_t sym, ic_color_t fg) {
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int has_label = label && label[0];
    int tw = has_label ? ic_text_measure(f, label) : 0;
    int cw = tw + (sym != IC_SYM_NONE ? IC_ICON_SM + (has_label ? IC_BTN_SYM_GAP : 0) : 0);
    int x = r.x + (r.w - cw) / 2;
    if (cw > r.w - 8) x = r.x + 4;
    if (sym != IC_SYM_NONE) {
        ic_symbol_draw(c, sym, (float)x + IC_ICON_SM * 0.5f, (float)r.y + (float)r.h * 0.5f,
                       (float)IC_ICON_SM, fg);
        x += IC_ICON_SM + (has_label ? IC_BTN_SYM_GAP : 0);
    }
    if (has_label) {
        ic_rect_t tr = ic_rect_make(x, r.y, r.x + r.w - x - 4, r.h);
        ic_text_draw_in(c, f, tr, label, fg, IC_ALIGN_LEFT);
    }
}

void ic_ui_button(ic_canvas_t *c, ic_rect_t r, const char *label, ic_symbol_t sym,
                  ic_button_style_t style, ic_state_t state) {
    const ic_palette_t *p = P();
    float rad = IC_R_CONTROL;
    ic_color_t fg = p->label;
    int disabled = state == IC_STATE_DISABLED;

    if (style == IC_BUTTON_PLAIN) {
        if (state == IC_STATE_HOVER) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, rad, p->fill_hover);
        else if (state == IC_STATE_PRESSED) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, rad, p->fill_pressed);
        ic_ui_button_content(c, r, label, sym, disabled ? p->label_disabled : p->label);
        return;
    }

    if (style == IC_BUTTON_PRIMARY && !disabled) {
        ic_color_t base = state == IC_STATE_PRESSED ? p->accent_pressed :
                          state == IC_STATE_HOVER ? p->accent_hover : p->accent;
        if (!p->dark) ic_theme_shadow(c, r.x, r.y, r.w, r.h, rad, IC_ELEV_CONTROL);
        ic_gfx_rrect_gradient_v(c, r.x, r.y, r.w, r.h, rad,
                                ic_color_mix(base, IC_RGB(0xFFFFFF), 0.08f), base);
        ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, 1.0f, IC_RGBA(0x000000, 0x1C));
        ic_gfx_hline(c, r.x + (int)rad, r.y + 1, r.w - 2 * (int)rad, IC_RGBA(0xFFFFFF, 0x2C));
        fg = p->label_on_accent;
    } else {
        ic_color_t face = state == IC_STATE_PRESSED ? p->control_pressed :
                          state == IC_STATE_HOVER ? p->control_hover : p->control;
        if (disabled) face = ic_color_fade(face, 0.55f);
        if (!p->dark && !disabled) ic_theme_shadow(c, r.x, r.y, r.w, r.h, rad, IC_ELEV_CONTROL);
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, rad, face);
        ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, rad, 1.0f, p->control_stroke);
        if (p->dark && !disabled) {
            ic_gfx_hline(c, r.x + (int)rad, r.y + 1, r.w - 2 * (int)rad, p->highlight);
        }
        fg = style == IC_BUTTON_DESTRUCTIVE ? p->danger : p->label;
    }
    if (disabled) fg = p->label_disabled;
    ic_ui_button_content(c, r, label, sym, fg);
}

void ic_ui_icon_button(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, ic_state_t state) {
    const ic_palette_t *p = P();
    ic_color_t fg = p->label_secondary;
    if (state == IC_STATE_HOVER) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->fill_hover);
        fg = p->label;
    } else if (state == IC_STATE_PRESSED) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->fill_pressed);
        fg = p->label;
    } else if (state == IC_STATE_DISABLED) {
        fg = p->label_disabled;
    }
    ic_symbol_draw(c, sym, (float)r.x + (float)r.w * 0.5f, (float)r.y + (float)r.h * 0.5f,
                   (float)IC_ICON_SM, fg);
}

void ic_ui_focus_ring(ic_canvas_t *c, ic_rect_t r, float radius) {
    ic_gfx_rrect_stroke(c, r.x - 3, r.y - 3, r.w + 6, r.h + 6, radius + 3.0f, 3.0f,
                        P()->focus_ring);
}



void ic_ui_toggle(ic_canvas_t *c, int x, int y, float on, ic_state_t state) {
    const ic_palette_t *p = P();
    float t = ic_clampf(on, 0.0f, 1.0f);
    float fade = state == IC_STATE_DISABLED ? 0.5f : 1.0f;
    float kr = (float)IC_TOGGLE_H * 0.5f - 2.0f;
    float kx = (float)x + (float)IC_TOGGLE_H * 0.5f + t * (float)(IC_TOGGLE_W - IC_TOGGLE_H);
    float ky = (float)y + (float)IC_TOGGLE_H * 0.5f;
    ic_color_t track = ic_color_mix(p->toggle_off, p->accent, t);
    if (state == IC_STATE_PRESSED) track = ic_color_mix(track, IC_RGB(0x000000), 0.10f);
    ic_gfx_rrect(c, x, y, IC_TOGGLE_W, IC_TOGGLE_H, (float)IC_TOGGLE_H * 0.5f,
                 ic_color_fade(track, fade));
    
    ic_gfx_circle(c, kx, ky + 0.75f, kr + 0.5f, IC_RGBA(0x000000, (uint32_t)(0x40 * fade)));
    ic_gfx_circle(c, kx, ky, kr, ic_color_fade(p->knob, fade));
}



int ic_ui_segmented_hit(ic_rect_t r, int count, int x, int y) {
    if (count <= 0 || !ic_ui_hit(r, x, y)) return -1;
    return (x - r.x) * count / r.w;
}

void ic_ui_segmented(ic_canvas_t *c, ic_rect_t r, const char *const *labels, int count,
                     float slide, int hover_index) {
    const ic_palette_t *p = P();
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int seg_w;
    float px;
    if (count <= 0) return;
    seg_w = r.w / count;
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->segment_track);
    for (int i = 1; i < count; i++) {
        float d0 = slide - (float)i, d1 = slide - (float)(i - 1);
        if ((d0 > -1.0f && d0 < 0.5f) || (d1 > -0.5f && d1 < 1.0f)) continue;
        ic_gfx_vline(c, r.x + i * seg_w, r.y + 6, r.h - 12, p->separator);
    }
    if (hover_index >= 0 && hover_index < count &&
        (slide - (float)hover_index > 0.5f || slide - (float)hover_index < -0.5f)) {
        ic_gfx_rrect(c, r.x + hover_index * seg_w + 2, r.y + 2, seg_w - 4, r.h - 4,
                     IC_R_CONTROL - 2.0f, p->fill_hover);
    }
    px = (float)r.x + 2.0f + slide * (float)seg_w;
    {
        ic_rect_t pill = ic_rect_make((int)(px + 0.5f), r.y + 2, seg_w - 4, r.h - 4);
        ic_theme_shadow(c, pill.x, pill.y, pill.w, pill.h, IC_R_CONTROL - 2.0f, IC_ELEV_CONTROL);
        ic_gfx_rrect(c, pill.x, pill.y, pill.w, pill.h, IC_R_CONTROL - 2.0f, p->segment_selected);
    }
    for (int i = 0; i < count; i++) {
        ic_rect_t cell = ic_rect_make(r.x + i * seg_w + 6, r.y, seg_w - 12, r.h);
        ic_text_draw_in(c, f, cell, labels[i], p->label, IC_ALIGN_CENTER);
    }
}



float ic_ui_slider_value(ic_rect_t r, int x) {
    float inner = (float)(r.w - IC_TOGGLE_H);
    if (inner <= 0.0f) return 0.0f;
    return ic_clampf(((float)(x - r.x) - (float)IC_TOGGLE_H * 0.5f) / inner, 0.0f, 1.0f);
}

void ic_ui_slider(ic_canvas_t *c, ic_rect_t r, float value, ic_state_t state) {
    const ic_palette_t *p = P();
    float v = ic_clampf(value, 0.0f, 1.0f);
    float kr = (float)IC_TOGGLE_H * 0.5f - 2.0f;
    float kx = (float)r.x + (float)IC_TOGGLE_H * 0.5f + v * (float)(r.w - IC_TOGGLE_H);
    float ky = (float)r.y + (float)r.h * 0.5f;
    int ty = r.y + r.h / 2 - 2;
    ic_color_t fill = state == IC_STATE_DISABLED ? p->label_disabled : p->accent;
    ic_gfx_rrect(c, r.x, ty, r.w, 4, 2.0f, p->toggle_off);
    ic_gfx_rrect(c, r.x, ty, (int)(kx - (float)r.x), 4, 2.0f, fill);
    ic_gfx_circle(c, kx, ky + 0.75f, kr + 0.5f, IC_RGBA(0x000000, 0x40));
    ic_gfx_circle(c, kx, ky, kr, state == IC_STATE_PRESSED ?
                  ic_color_mix(p->knob, IC_RGB(0x000000), 0.08f) : p->knob);
}

void ic_ui_progress(ic_canvas_t *c, ic_rect_t r, float value, ic_color_t tint) {
    const ic_palette_t *p = P();
    int h = r.h < 6 ? r.h : 6;
    int y = r.y + (r.h - h) / 2;
    ic_color_t fill = IC_ALPHA(tint) ? tint : p->accent;
    ic_gfx_rrect(c, r.x, y, r.w, h, (float)h * 0.5f, p->separator);
    if (value > 0.0f) {
        int w = (int)((float)r.w * ic_clampf(value, 0.0f, 1.0f) + 0.5f);
        if (w < h) w = h;
        ic_gfx_rrect(c, r.x, y, w, h, (float)h * 0.5f, fill);
    }
}



#define IC_FIELD_PAD 8

static int ic_field_text_x(ic_rect_t r, const ic_textfield_t *tf) {
    int x = r.x + IC_FIELD_PAD;
    if (tf->leading != IC_SYM_NONE) x += IC_ICON_SM + 4;
    return x;
}

void ic_ui_textfield_scroll(ic_rect_t r, ic_textfield_t *tf) {
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int avail = r.x + r.w - IC_FIELD_PAD - ic_field_text_x(r, tf);
    int caret = tf->text ? ic_text_measure_n(f, tf->text, tf->cursor) : 0;
    if (caret - tf->scroll_px > avail - 2) tf->scroll_px = caret - avail + 2;
    if (caret - tf->scroll_px < 0) tf->scroll_px = caret;
    if (tf->scroll_px < 0) tf->scroll_px = 0;
}

int ic_ui_textfield_index_at(ic_rect_t r, const ic_textfield_t *tf, int x) {
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int rel = x - ic_field_text_x(r, tf) + tf->scroll_px;
    int best = 0, i = 0;
    if (!tf->text) return 0;
    while (tf->text[i]) {
        int next = i + 1;
        int mid;
        while (((unsigned char)tf->text[next] & 0xC0) == 0x80) next++;
        mid = (ic_text_measure_n(f, tf->text, i) + ic_text_measure_n(f, tf->text, next)) / 2;
        if (rel < mid) return i;
        best = next;
        i = next;
    }
    return best;
}

void ic_ui_textfield(ic_canvas_t *c, ic_rect_t r, const ic_textfield_t *tf) {
    const ic_palette_t *p = P();
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int tx = ic_field_text_x(r, tf);
    int base = ic_text_center_baseline(f, r.y, r.h);
    ic_rect_t saved;
    if (tf->focused) ic_ui_focus_ring(c, r, IC_R_CONTROL);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->field);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, 1.0f,
                        tf->focused ? ic_color_fade(p->accent, 0.9f) : p->control_stroke);
    if (tf->leading != IC_SYM_NONE) {
        ic_symbol_draw(c, tf->leading, (float)(r.x + IC_FIELD_PAD) + IC_ICON_SM * 0.5f,
                       (float)r.y + (float)r.h * 0.5f, 14.0f, p->label_secondary);
    }
    ic_canvas_push_clip(c, tx, r.y + 1, r.x + r.w - IC_FIELD_PAD - tx, r.h - 2, &saved);
    if (tf->text && tf->text[0]) {
        if (tf->focused && tf->sel_start != tf->sel_end) {
            int a = tf->sel_start < tf->sel_end ? tf->sel_start : tf->sel_end;
            int b = tf->sel_start < tf->sel_end ? tf->sel_end : tf->sel_start;
            int x0 = tx - tf->scroll_px + ic_text_measure_n(f, tf->text, a);
            int x1 = tx - tf->scroll_px + ic_text_measure_n(f, tf->text, b);
            ic_gfx_rrect(c, x0, r.y + 4, x1 - x0, r.h - 8, 2.0f, p->accent_soft);
        }
        ic_text_draw(c, f, tx - tf->scroll_px, base, tf->text, p->label);
    } else if (tf->placeholder) {
        ic_text_draw(c, f, tx, base, tf->placeholder, p->label_tertiary);
    }
    if (tf->focused && tf->caret_on) {
        int cx = tx - tf->scroll_px + (tf->text ? ic_text_measure_n(f, tf->text, tf->cursor) : 0);
        ic_gfx_fill(c, cx, base - f->ascent + 2, 2, f->ascent + f->descent - 1, p->accent);
    }
    ic_canvas_pop_clip(c, &saved);
}



void ic_ui_window_bg(ic_canvas_t *c, ic_rect_t r) {
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, P()->window);
}

void ic_ui_group(ic_canvas_t *c, ic_rect_t r) {
    const ic_palette_t *p = P();
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_GROUP, p->group);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_GROUP, 1.0f,
                        p->group_stroke);
}

void ic_ui_group_row(ic_canvas_t *c, ic_rect_t row, int index, int count, float hover) {
    const ic_palette_t *p = P();
    if (hover > 0.0f) {
        float top = index == 0 ? IC_R_GROUP : 0.0f;
        float bot = index == count - 1 ? IC_R_GROUP : 0.0f;
        ic_gfx_rrect4(c, row.x, row.y, row.w, row.h, top, top, bot, bot,
                      ic_color_fade(p->fill_hover, hover));
    }
    if (index > 0) ic_gfx_hline(c, row.x + IC_SP_3, row.y, row.w - 2 * IC_SP_3, p->separator);
}

void ic_ui_row_text(ic_canvas_t *c, ic_rect_t row, ic_symbol_t sym, ic_color_t sym_tint,
                    const char *title, const char *subtitle) {
    const ic_palette_t *p = P();
    const ic_face_t *ft = ic_font(IC_FONT_BODY);
    const ic_face_t *fs = ic_font(IC_FONT_FOOTNOTE);
    int x = row.x + IC_SP_3;
    if (sym != IC_SYM_NONE) {
        int b = 24;
        int by = row.y + (row.h - b) / 2;
        ic_gfx_rrect(c, x, by, b, b, 6.0f, IC_ALPHA(sym_tint) ? sym_tint : p->accent);
        ic_symbol_draw(c, sym, (float)x + (float)b * 0.5f, (float)by + (float)b * 0.5f, 15.0f,
                       IC_RGB(0xFFFFFF));
        x += b + IC_SP_3 - 2;
    }
    if (subtitle && subtitle[0]) {
        int block = ft->cap_h + 5 + fs->line_h - fs->descent;
        int top = row.y + (row.h - block) / 2;
        ic_text_draw(c, ft, x, top + ft->cap_h, title, p->label);
        ic_text_draw(c, fs, x, top + ft->cap_h + 5 + fs->ascent, subtitle, p->label_secondary);
    } else {
        ic_text_draw(c, ft, x, ic_text_center_baseline(ft, row.y, row.h), title, p->label);
    }
}

void ic_ui_section_header(ic_canvas_t *c, int x, int y, const char *text) {
    const ic_face_t *f = ic_font(IC_FONT_HEADLINE);
    ic_text_draw(c, f, x + 2, y + f->ascent, text, P()->label_secondary);
}

void ic_ui_sidebar_bg(ic_canvas_t *c, ic_rect_t r) {
    const ic_palette_t *p = P();
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, p->sidebar);
    ic_gfx_vline(c, r.x + r.w - 1, r.y, r.h, p->separator);
}

void ic_ui_sidebar_item(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *label,
                        int selected, float hover) {
    const ic_palette_t *p = P();
    ic_rect_t pill = ic_rect_inset(r, IC_SP_2, 1);
    int x = pill.x + IC_SP_2;
    if (selected) ic_gfx_rrect(c, pill.x, pill.y, pill.w, pill.h, IC_R_ROW, p->fill_selected_idle);
    else if (hover > 0.0f) ic_gfx_rrect(c, pill.x, pill.y, pill.w, pill.h, IC_R_ROW,
                                        ic_color_fade(p->fill_hover, hover));
    if (sym != IC_SYM_NONE) {
        ic_symbol_draw(c, sym, (float)x + IC_ICON_SM * 0.5f, (float)pill.y + (float)pill.h * 0.5f,
                       (float)IC_ICON_SM, p->accent);
        x += IC_ICON_SM + IC_SP_2;
    }
    ic_text_draw_in(c, ic_font(IC_FONT_BODY),
                    ic_rect_make(x, pill.y, pill.x + pill.w - x - IC_SP_2, pill.h),
                    label, p->label, IC_ALIGN_LEFT);
}

ic_color_t ic_ui_list_row(ic_canvas_t *c, ic_rect_t r, int selected, int list_focused,
                          float hover) {
    const ic_palette_t *p = P();
    ic_rect_t pill = ic_rect_inset(r, IC_SP_1 + 1, 0);
    if (selected) {
        ic_gfx_rrect(c, pill.x, pill.y, pill.w, pill.h, IC_R_ROW,
                     list_focused ? p->accent : p->fill_selected_idle);
        return list_focused ? p->label_on_accent : p->label;
    }
    if (hover > 0.0f) {
        ic_gfx_rrect(c, pill.x, pill.y, pill.w, pill.h, IC_R_ROW, ic_color_fade(p->fill_hover, hover));
    }
    return p->label;
}

void ic_ui_table_header(ic_canvas_t *c, ic_rect_t r, const char *const *titles,
                        const int *widths, int count) {
    const ic_palette_t *p = P();
    const ic_face_t *f = ic_font(IC_FONT_CAPTION_EMPH);
    int x = r.x + IC_SP_3;
    for (int i = 0; i < count; i++) {
        int w = widths[i] < 0 ? r.x + r.w - x - IC_SP_3 : widths[i];
        ic_text_draw_in(c, f, ic_rect_make(x, r.y, w - IC_SP_2, r.h), titles[i],
                        p->label_secondary, IC_ALIGN_LEFT);
        x += w;
    }
    ic_gfx_hline(c, r.x, r.y + r.h - 1, r.w, p->separator);
}

void ic_ui_toolbar(ic_canvas_t *c, ic_rect_t r) {
    const ic_palette_t *p = P();
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, p->titlebar);
    ic_gfx_hline(c, r.x, r.y + r.h - 1, r.w, p->bar_edge);
}

void ic_ui_statusbar(ic_canvas_t *c, ic_rect_t r, const char *text) {
    const ic_palette_t *p = P();
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, p->window);
    ic_gfx_hline(c, r.x, r.y, r.w, p->separator);
    if (text) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(r.x + IC_SP_3, r.y, r.w - 2 * IC_SP_3, r.h), text,
                        p->label_secondary, IC_ALIGN_LEFT);
    }
}

ic_rect_t ic_ui_scrollbar_thumb(ic_rect_t view, int offset, int content_h) {
    int track = view.h - 4;
    int th, ty;
    if (content_h <= view.h || view.h <= 0) return ic_rect_make(0, 0, 0, 0);
    th = track * view.h / content_h;
    if (th < 28) th = 28;
    ty = view.y + 2 + (int)((int64_t)(track - th) * offset / (content_h - view.h));
    return ic_rect_make(view.x + view.w - 9, ty, 7, th);
}

void ic_ui_scrollbar(ic_canvas_t *c, ic_rect_t view, int offset, int content_h, float alpha) {
    const ic_palette_t *p = P();
    ic_rect_t t = ic_ui_scrollbar_thumb(view, offset, content_h);
    if (t.h <= 0 || alpha <= 0.0f) return;
    ic_gfx_rrect(c, t.x + 1, t.y, t.w - 2, t.h, 2.5f, ic_color_fade(p->scroller, alpha));
}

void ic_ui_empty_state(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                       const char *message) {
    const ic_palette_t *p = P();
    int cy = r.y + r.h / 2 - 30;
    ic_symbol_draw(c, sym, (float)r.x + (float)r.w * 0.5f, (float)cy, 36.0f, p->label_tertiary);
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(r.x + 16, cy + 26, r.w - 32, 22),
                    title, p->label_secondary, IC_ALIGN_CENTER);
    if (message) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 16, cy + 48, r.w - 32, 18),
                        message, p->label_tertiary, IC_ALIGN_CENTER);
    }
}



void ic_ui_panel(ic_canvas_t *c, ic_rect_t r, float radius, ic_elevation_t elevation,
                 uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = P();
    ic_theme_shadow(c, r.x, r.y, r.w, r.h, radius, elevation);
    ic_gfx_backdrop(c, r.x, r.y, r.w, r.h, radius, 24, p->material_menu, scratch, scratch_len);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, radius, 1.0f, p->frame);
    ic_gfx_rrect_stroke(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, radius - 1.0f, 1.0f, p->highlight);
}



#define IC_MENU_PAD_Y   5
#define IC_MENU_PAD_X   5
#define IC_MENU_SEP_H   9
#define IC_MENU_TEXT_X  12
#define IC_MENU_MIN_W  180

static int ic_menu_item_h(const ic_menu_model_t *m, int i) {
    return m->labels[i] == IC_MENU_SEPARATOR ? IC_MENU_SEP_H : IC_H_MENU_ITEM;
}

int ic_ui_menu_width(const ic_menu_model_t *m) {
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    const ic_face_t *fs = ic_font(IC_FONT_BODY);
    int w = IC_MENU_MIN_W;
    for (int i = 0; i < m->count; i++) {
        int iw;
        if (m->labels[i] == IC_MENU_SEPARATOR || !m->labels[i]) continue;
        iw = 2 * IC_MENU_PAD_X + 2 * IC_MENU_TEXT_X + ic_text_measure(f, m->labels[i]);
        if (m->shortcuts[i]) iw += IC_SP_6 + ic_text_measure(fs, m->shortcuts[i]);
        if (iw > w) w = iw;
    }
    return w;
}

int ic_ui_menu_height(const ic_menu_model_t *m) {
    int h = 2 * IC_MENU_PAD_Y;
    for (int i = 0; i < m->count; i++) h += ic_menu_item_h(m, i);
    return h;
}

int ic_ui_menu_hit(const ic_menu_model_t *m, int mx, int my, int x, int y) {
    int w = ic_ui_menu_width(m);
    int yy = my + IC_MENU_PAD_Y;
    if (x < mx + IC_MENU_PAD_X || x >= mx + w - IC_MENU_PAD_X) return -1;
    for (int i = 0; i < m->count; i++) {
        int h = ic_menu_item_h(m, i);
        if (y >= yy && y < yy + h) {
            if (m->labels[i] == IC_MENU_SEPARATOR || m->disabled[i]) return -1;
            return i;
        }
        yy += h;
    }
    return -1;
}

void ic_ui_menu(ic_canvas_t *c, const ic_menu_model_t *m, int mx, int my,
                uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = P();
    const ic_face_t *f = ic_font(IC_FONT_BODY);
    int w = ic_ui_menu_width(m);
    int h = ic_ui_menu_height(m);
    int y = my + IC_MENU_PAD_Y;
    ic_ui_panel(c, ic_rect_make(mx, my, w, h), IC_R_MENU, IC_ELEV_MENU, scratch, scratch_len);
    for (int i = 0; i < m->count; i++) {
        int ih = ic_menu_item_h(m, i);
        if (m->labels[i] == IC_MENU_SEPARATOR) {
            ic_gfx_hline(c, mx + IC_MENU_TEXT_X, y + ih / 2, w - 2 * IC_MENU_TEXT_X, p->separator);
        } else if (m->labels[i]) {
            ic_rect_t row = ic_rect_make(mx + IC_MENU_PAD_X, y, w - 2 * IC_MENU_PAD_X, ih);
            int hot = i == m->hover && !m->disabled[i];
            ic_color_t fg = m->disabled[i] ? p->label_disabled : (hot ? p->label_on_accent : p->label);
            if (hot) ic_gfx_rrect(c, row.x, row.y, row.w, row.h, IC_R_MENU_ITEM, p->accent);
            ic_text_draw_in(c, f, ic_rect_make(row.x + IC_MENU_TEXT_X - IC_MENU_PAD_X, row.y,
                                               row.w - 2 * IC_MENU_TEXT_X, row.h),
                            m->labels[i], fg, IC_ALIGN_LEFT);
            if (m->shortcuts[i]) {
                ic_text_draw_in(c, f, ic_rect_make(row.x, row.y, row.w - IC_MENU_TEXT_X + IC_MENU_PAD_X,
                                                   row.h),
                                m->shortcuts[i], hot ? fg : p->label_tertiary, IC_ALIGN_RIGHT);
            }
        }
        y += ih;
    }
}



void ic_ui_alert(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                 const char *message, const char *const *buttons, int count,
                 int hover_button, ic_rect_t *button_rects) {
    const ic_palette_t *p = P();
    int y = r.y + IC_SP_5;
    int cx = r.x + r.w / 2;
    ic_theme_shadow(c, r.x, r.y, r.w, r.h, IC_R_PANEL, IC_ELEV_MENU);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_PANEL, ic_color_over(p->window, p->material_menu));
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_PANEL, 1.0f, p->frame);
    if (sym != IC_SYM_NONE) {
        ic_symbol_draw(c, sym, (float)cx, (float)y + 20.0f, 40.0f, p->accent);
        y += 48;
    }
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(r.x + IC_SP_4, y, r.w - 2 * IC_SP_4, 20),
                    title, p->label, IC_ALIGN_CENTER);
    y += 24;
    if (message) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(r.x + IC_SP_4, y, r.w - 2 * IC_SP_4, 18), message,
                        p->label_secondary, IC_ALIGN_CENTER);
    }
    if (count > 0) {
        int gap = IC_SP_2;
        int bw = (r.w - 2 * IC_SP_4 - gap * (count - 1)) / count;
        int by = r.y + r.h - IC_SP_4 - IC_H_CONTROL;
        for (int i = 0; i < count; i++) {
            ic_rect_t b = ic_rect_make(r.x + IC_SP_4 + i * (bw + gap), by, bw, IC_H_CONTROL);
            ic_ui_button(c, b, buttons[i], IC_SYM_NONE,
                         i == count - 1 ? IC_BUTTON_PRIMARY : IC_BUTTON_DEFAULT,
                         i == hover_button ? IC_STATE_HOVER : IC_STATE_NORMAL);
            if (button_rects) button_rects[i] = b;
        }
    }
}
