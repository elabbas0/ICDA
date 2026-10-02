


#include "wm_frame.h"

#define WM_BTN_W       28
#define WM_BTN_H       22
#define WM_BTN_GAP      2
#define WM_BTN_MARGIN   7
#define WM_BTN_SYMBOL  13.0f
#define WM_TITLE_PAD   14
#define WM_GRIP_OUT     5    
#define WM_GRIP_IN      3    
#define WM_GRIP_CORNER 16

ic_rect_t wm_frame_rect(const wm_frame_t *f) {
    return ic_rect_make(f->x, f->y - WM_TITLE_H, f->w, f->h + WM_TITLE_H);
}

ic_rect_t wm_frame_damage_rect(int x, int y, int w, int h) {
    return ic_rect_make(x - WM_SHADOW_REACH, y - WM_TITLE_H - WM_SHADOW_REACH,
                        w + 2 * WM_SHADOW_REACH, h + WM_TITLE_H + 2 * WM_SHADOW_REACH);
}

float wm_frame_radius(const wm_frame_t *f) {
    return f->maximized ? 0.0f : IC_R_WINDOW;
}

ic_rect_t wm_frame_button_rect(const wm_frame_t *f, wm_hit_t which) {
    int right = f->x + f->w - WM_BTN_MARGIN;
    int y = f->y - WM_TITLE_H + (WM_TITLE_H - WM_BTN_H) / 2;
    int slot;
    switch (which) {
    case WM_HIT_CLOSE:    slot = 0; break;
    case WM_HIT_MAXIMIZE: slot = 1; break;
    case WM_HIT_MINIMIZE: slot = 2; break;
    default:              return ic_rect_make(0, 0, 0, 0);
    }
    return ic_rect_make(right - (slot + 1) * WM_BTN_W - slot * WM_BTN_GAP, y, WM_BTN_W, WM_BTN_H);
}

wm_hit_t wm_frame_hit(const wm_frame_t *f, int mx, int my) {
    ic_rect_t fr = wm_frame_rect(f);
    int in_frame = ic_ui_hit(fr, mx, my);
    if (!f->maximized) {
        int left = mx >= fr.x - WM_GRIP_OUT && mx < fr.x + WM_GRIP_IN;
        int right = mx >= fr.x + fr.w - WM_GRIP_IN && mx < fr.x + fr.w + WM_GRIP_OUT;
        int bottom = my >= fr.y + fr.h - WM_GRIP_IN && my < fr.y + fr.h + WM_GRIP_OUT;
        int in_x = mx >= fr.x - WM_GRIP_OUT && mx < fr.x + fr.w + WM_GRIP_OUT;
        int in_y = my >= fr.y + WM_TITLE_H && my < fr.y + fr.h + WM_GRIP_OUT;
        int corner_r = mx >= fr.x + fr.w - WM_GRIP_CORNER && mx < fr.x + fr.w + WM_GRIP_OUT;
        int corner_l = mx >= fr.x - WM_GRIP_OUT && mx < fr.x + WM_GRIP_CORNER;
        int corner_b = my >= fr.y + fr.h - WM_GRIP_CORNER && my < fr.y + fr.h + WM_GRIP_OUT;
        if (in_x && in_y) {
            if (corner_b && corner_r && (right || bottom)) return WM_HIT_RESIZE_BR;
            if (corner_b && corner_l && (left || bottom)) return WM_HIT_RESIZE_BL;
            if (bottom) return WM_HIT_RESIZE_B;
            if (right) return WM_HIT_RESIZE_R;
            if (left) return WM_HIT_RESIZE_L;
        }
    }
    if (!in_frame) return WM_HIT_NONE;
    if (my < f->y) {
        if (ic_ui_hit(wm_frame_button_rect(f, WM_HIT_CLOSE), mx, my)) return WM_HIT_CLOSE;
        if (ic_ui_hit(wm_frame_button_rect(f, WM_HIT_MAXIMIZE), mx, my)) return WM_HIT_MAXIMIZE;
        if (ic_ui_hit(wm_frame_button_rect(f, WM_HIT_MINIMIZE), mx, my)) return WM_HIT_MINIMIZE;
        return WM_HIT_TITLE;
    }
    return WM_HIT_CLIENT;
}

void wm_frame_draw_shadow(ic_canvas_t *c, const wm_frame_t *f, float opacity) {
    ic_rect_t fr = wm_frame_rect(f);
    if (f->maximized) return;
    ic_theme_shadow_faded(c, fr.x, fr.y, fr.w, fr.h, IC_R_WINDOW,
                          f->focused ? IC_ELEV_WINDOW : IC_ELEV_WINDOW_IDLE, opacity);
}

static void wm_frame_caption_button(ic_canvas_t *c, const wm_frame_t *f, wm_hit_t which,
                                    ic_symbol_t sym) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t r = wm_frame_button_rect(f, which);
    int hover = f->hover == which;
    int pressed = f->pressed == which;
    ic_color_t fg = f->focused ? p->label_secondary : p->label_tertiary;
    if (which == WM_HIT_CLOSE && (hover || pressed)) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL,
                     pressed ? ic_color_mix(p->close_hover, IC_RGB(0x000000), 0.15f) : p->close_hover);
        fg = IC_RGB(0xFFFFFF);
    } else if (pressed) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->fill_pressed);
        fg = p->label;
    } else if (hover) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, p->fill_hover);
        fg = p->label;
    }
    ic_symbol_draw(c, sym, (float)r.x + (float)r.w * 0.5f, (float)r.y + (float)r.h * 0.5f,
                   WM_BTN_SYMBOL, fg);
}

void wm_frame_draw(ic_canvas_t *c, const wm_frame_t *f,
                   const uint32_t *px, int pw, int ph) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t fr = wm_frame_rect(f);
    float r = wm_frame_radius(f);
    int top = fr.y;

    
    ic_gfx_rrect4(c, fr.x, top, fr.w, WM_TITLE_H, r, r, 0.0f, 0.0f, p->titlebar);
    if (p->dark && r > 0.0f) {
        ic_gfx_hline(c, fr.x + (int)r, top + 1, fr.w - 2 * (int)r, p->highlight);
    }
    ic_gfx_hline(c, fr.x, f->y - 1, fr.w, p->bar_edge);

    
    if (f->title && f->title[0]) {
        const ic_face_t *face = ic_font(IC_FONT_HEADLINE);
        ic_rect_t minr = wm_frame_button_rect(f, WM_HIT_MINIMIZE);
        int avail_l = fr.x + WM_TITLE_PAD;
        int avail_r = minr.x - WM_TITLE_PAD / 2;
        int tw = ic_text_measure(face, f->title);
        int cx = fr.x + (fr.w - tw) / 2;
        ic_color_t fg = f->focused ? p->label : p->label_tertiary;
        if (cx < avail_l) cx = avail_l;
        if (cx + tw > avail_r) cx = avail_l;
        if (avail_r > avail_l) {
            ic_text_draw_in(c, face, ic_rect_make(cx, top, avail_r - cx, WM_TITLE_H - 1),
                            f->title, fg, IC_ALIGN_LEFT);
        }
    }
    wm_frame_caption_button(c, f, WM_HIT_MINIMIZE, IC_SYM_MINIMIZE);
    wm_frame_caption_button(c, f, WM_HIT_MAXIMIZE, f->maximized ? IC_SYM_RESTORE : IC_SYM_MAXIMIZE);
    wm_frame_caption_button(c, f, WM_HIT_CLOSE, IC_SYM_CLOSE);

    

    {
        int cw = pw < f->w ? pw : f->w;
        int ch = ph < f->h ? ph : f->h;
        if (!px || cw <= 0 || ch <= 0 || cw < f->w || ch < f->h) {
            ic_gfx_rrect4(c, f->x, f->y, f->w, f->h, 0.0f, 0.0f, r, r, p->window);
        }
        if (px && cw > 0 && ch > 0) {
            float bl = ch == f->h ? r : 0.0f;
            float br = (ch == f->h && cw == f->w) ? r : 0.0f;
            ic_gfx_blit_rrect4(c, f->x, f->y, px, cw, ch, pw, 0.0f, 0.0f, br, bl, 255);
        }
    }

    

    if (r > 0.0f) {
        ic_gfx_rrect_stroke(c, fr.x - 1, fr.y - 1, fr.w + 2, fr.h + 2, r + 1.0f, 1.0f, p->frame);
    }
}
