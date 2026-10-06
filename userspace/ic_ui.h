












#ifndef USERSPACE_IC_UI_H
#define USERSPACE_IC_UI_H

#include <stdint.h>
#include "ic_gfx.h"
#include "ic_font.h"
#include "ic_theme.h"

static inline int ic_ui_hit(ic_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline ic_rect_t ic_rect_make(int x, int y, int w, int h) {
    ic_rect_t r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static inline ic_rect_t ic_rect_inset(ic_rect_t r, int dx, int dy) {
    r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy;
    return r;
}






typedef enum {
    IC_SYM_NONE = 0,
    IC_SYM_CLOSE,
    IC_SYM_MINIMIZE,
    IC_SYM_MAXIMIZE,
    IC_SYM_RESTORE,
    IC_SYM_CHEVRON_LEFT,
    IC_SYM_CHEVRON_RIGHT,
    IC_SYM_CHEVRON_DOWN,
    IC_SYM_CHEVRON_UP,
    IC_SYM_CHECK,
    IC_SYM_PLUS,
    IC_SYM_MINUS,
    IC_SYM_SEARCH,
    IC_SYM_RELOAD,
    IC_SYM_POWER,
    IC_SYM_RESTART,
    IC_SYM_PLAY,
    IC_SYM_PAUSE,
    IC_SYM_STOP,
    IC_SYM_SPEAKER,
    IC_SYM_GEAR,
    IC_SYM_GRID,
    IC_SYM_FOLDER,
    IC_SYM_DOCUMENT,
    IC_SYM_DISK,
    IC_SYM_TERMINAL,
    IC_SYM_GLOBE,
    IC_SYM_MUSIC,
    IC_SYM_ACTIVITY,
    IC_SYM_SUN,
    IC_SYM_MOON,
    IC_SYM_INFO,
    IC_SYM_WARNING,
    IC_SYM_TRASH,
    IC_SYM_WIFI,
    IC_SYM_LOCK,
    IC_SYM_KEYBOARD,
    IC_SYM_COUNT
} ic_symbol_t;


void ic_symbol_draw(ic_canvas_t *c, ic_symbol_t sym, float cx, float cy, float size,
                    ic_color_t color);



typedef enum {
    IC_STATE_NORMAL = 0,
    IC_STATE_HOVER,
    IC_STATE_PRESSED,
    IC_STATE_DISABLED
} ic_state_t;


ic_state_t ic_ui_state(int enabled, int hover, int pressed);

typedef enum {
    IC_BUTTON_DEFAULT = 0,  
    IC_BUTTON_PRIMARY,      
    IC_BUTTON_DESTRUCTIVE,  
    IC_BUTTON_PLAIN         
} ic_button_style_t;

void ic_ui_button(ic_canvas_t *c, ic_rect_t r, const char *label, ic_symbol_t sym,
                  ic_button_style_t style, ic_state_t state);

int  ic_ui_button_width(const char *label, ic_symbol_t sym);


void ic_ui_icon_button(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, ic_state_t state);



void ic_ui_toggle(ic_canvas_t *c, int x, int y, float on, ic_state_t state);
static inline ic_rect_t ic_ui_toggle_rect(int x, int y) {
    return ic_rect_make(x, y, IC_TOGGLE_W, IC_TOGGLE_H);
}



void ic_ui_segmented(ic_canvas_t *c, ic_rect_t r, const char *const *labels, int count,
                     float slide, int hover_index);
int  ic_ui_segmented_hit(ic_rect_t r, int count, int x, int y);


void ic_ui_slider(ic_canvas_t *c, ic_rect_t r, float value, ic_state_t state);
float ic_ui_slider_value(ic_rect_t r, int x);


void ic_ui_progress(ic_canvas_t *c, ic_rect_t r, float value, ic_color_t tint);




typedef struct {
    const char *text;
    int         cursor;        
    int         sel_start;     
    int         sel_end;
    int         focused;
    int         caret_on;      
    const char *placeholder;
    ic_symbol_t leading;       
    int         scroll_px;
} ic_textfield_t;

void ic_ui_textfield(ic_canvas_t *c, ic_rect_t r, const ic_textfield_t *tf);
void ic_ui_textfield_scroll(ic_rect_t r, ic_textfield_t *tf);

int  ic_ui_textfield_index_at(ic_rect_t r, const ic_textfield_t *tf, int x);


void ic_ui_focus_ring(ic_canvas_t *c, ic_rect_t r, float radius);




void ic_ui_window_bg(ic_canvas_t *c, ic_rect_t r);



void ic_ui_group(ic_canvas_t *c, ic_rect_t r);


void ic_ui_group_row(ic_canvas_t *c, ic_rect_t row, int index, int count, float hover);

void ic_ui_row_text(ic_canvas_t *c, ic_rect_t row, ic_symbol_t sym, ic_color_t sym_tint,
                    const char *title, const char *subtitle);

void ic_ui_section_header(ic_canvas_t *c, int x, int y, const char *text);


void ic_ui_sidebar_bg(ic_canvas_t *c, ic_rect_t r);
void ic_ui_sidebar_item(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *label,
                        int selected, float hover);



ic_color_t ic_ui_list_row(ic_canvas_t *c, ic_rect_t r, int selected, int list_focused,
                          float hover);

void ic_ui_table_header(ic_canvas_t *c, ic_rect_t r, const char *const *titles,
                        const int *widths, int count);


void ic_ui_toolbar(ic_canvas_t *c, ic_rect_t r);

void ic_ui_statusbar(ic_canvas_t *c, ic_rect_t r, const char *text);



void ic_ui_scrollbar(ic_canvas_t *c, ic_rect_t view, int offset, int content_h,
                     float alpha);

ic_rect_t ic_ui_scrollbar_thumb(ic_rect_t view, int offset, int content_h);


void ic_ui_empty_state(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                       const char *message);



#define IC_MENU_ITEMS_MAX 16
#define IC_MENU_SEPARATOR ((const char *)1)
#define IC_MENU_CHECK_OFF 1
#define IC_MENU_CHECK_ON  2

typedef struct {
    const char *labels[IC_MENU_ITEMS_MAX];   
    const char *shortcuts[IC_MENU_ITEMS_MAX];
    uint8_t     disabled[IC_MENU_ITEMS_MAX];
    uint8_t     checked[IC_MENU_ITEMS_MAX];   
    uint8_t     submenu[IC_MENU_ITEMS_MAX];   
    int         count;
    int         hover;                        
} ic_menu_model_t;

int  ic_ui_menu_width(const ic_menu_model_t *m);
int  ic_ui_menu_height(const ic_menu_model_t *m);
ic_rect_t ic_ui_menu_item_rect(const ic_menu_model_t *m, int mx, int my, int i);


int  ic_ui_menu_hit(const ic_menu_model_t *m, int mx, int my, int x, int y);



void ic_ui_menu(ic_canvas_t *c, const ic_menu_model_t *m, int mx, int my,
                uint32_t *scratch, int scratch_len);


void ic_ui_panel(ic_canvas_t *c, ic_rect_t r, float radius, ic_elevation_t elevation,
                 uint32_t *scratch, int scratch_len);



int ic_ui_alert_height(ic_symbol_t sym, const char *message);
ic_rect_t ic_ui_alert_button_rect(ic_rect_t r, int index, int count);
void ic_ui_alert(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, const char *title,
                 const char *message, const char *const *buttons, int count,
                 int hover_button, ic_rect_t *button_rects);

#endif 
