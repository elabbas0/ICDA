








#ifndef USERSPACE_IC_THEME_H
#define USERSPACE_IC_THEME_H

#include <stdint.h>
#include "ic_gfx.h"
#include "ic_anim.h"



#define IC_SP_1   4
#define IC_SP_2   8
#define IC_SP_3  12
#define IC_SP_4  16
#define IC_SP_5  20
#define IC_SP_6  24
#define IC_SP_8  32


#define IC_R_WINDOW     10.0f   
#define IC_R_PANEL      12.0f   
#define IC_R_MENU        8.0f   
#define IC_R_MENU_ITEM   5.0f   
#define IC_R_GROUP       9.0f   
#define IC_R_CONTROL     6.0f   
#define IC_R_ROW         6.0f   
#define IC_R_TILE       12.0f   


#define IC_H_TITLEBAR   34      
#define IC_H_TASKBAR    48      
#define IC_H_CONTROL    26      
#define IC_H_CONTROL_SM 22      
#define IC_H_ROW        28      
#define IC_H_ROW_TALL   44      
#define IC_H_MENU_ITEM  24
#define IC_H_TOOLBAR    44      
#define IC_W_SIDEBAR   180
#define IC_TOGGLE_W     38
#define IC_TOGGLE_H     22
#define IC_ICON_SM      16
#define IC_ICON_MD      20
#define IC_ICON_LG      32
#define IC_ICON_XL      48


#define IC_DUR_INSTANT   80     
#define IC_DUR_FAST     140     
#define IC_DUR_BASE     220     
#define IC_DUR_SLOW     340     
#define IC_EASE_ENTER   IC_EASE_DECELERATE
#define IC_EASE_EXIT    IC_EASE_ACCELERATE
#define IC_EASE_MOVE    IC_EASE_EMPHASIZED

#define IC_SPRING_WINDOW_RESPONSE 0.34f
#define IC_SPRING_WINDOW_DAMPING  0.86f
#define IC_SPRING_SNAPPY_RESPONSE 0.24f
#define IC_SPRING_SNAPPY_DAMPING  0.92f


typedef struct {
    int      blur;   
    int      dy;     
    uint32_t alpha;  
} ic_shadow_spec_t;

typedef enum {
    IC_ELEV_CONTROL = 0,   
    IC_ELEV_MENU,          
    IC_ELEV_WINDOW,        
    IC_ELEV_WINDOW_IDLE,   
    IC_ELEV_COUNT
} ic_elevation_t;



void ic_theme_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     ic_elevation_t level);

void ic_theme_shadow_faded(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                           ic_elevation_t level, float opacity);



typedef enum {
    IC_ACCENT_BLUE = 0,
    IC_ACCENT_PURPLE,
    IC_ACCENT_PINK,
    IC_ACCENT_RED,
    IC_ACCENT_ORANGE,
    IC_ACCENT_GREEN,
    IC_ACCENT_GRAPHITE,
    IC_ACCENT_COUNT
} ic_accent_t;

#define IC_TINT_INDIGO  0x5E5CE6
#define IC_TINT_ORANGE  0xFF9F0A
#define IC_TINT_TEAL    0x30B0C7
#define IC_TINT_PINK    0xFF375F

typedef struct {
    int        dark;

    
    ic_color_t desktop;          
    ic_color_t window;           
    ic_color_t content;          
    ic_color_t sidebar;          
    ic_color_t group;            
    ic_color_t titlebar;         
    ic_color_t material_menu;    
    ic_color_t material_bar;     

    
    ic_color_t control;          
    ic_color_t control_hover;
    ic_color_t control_pressed;
    ic_color_t control_stroke;   
    ic_color_t field;            
    ic_color_t toggle_off;       
    ic_color_t knob;             
    ic_color_t fill_hover;       
    ic_color_t fill_pressed;
    ic_color_t fill_selected_idle; 
    ic_color_t segment_track;    
    ic_color_t segment_selected; 
    ic_color_t scroller;         

    
    ic_color_t label;
    ic_color_t label_secondary;
    ic_color_t label_tertiary;
    ic_color_t label_disabled;
    ic_color_t label_on_accent;

    
    ic_color_t separator;        
    ic_color_t frame;            
    ic_color_t highlight;        
    ic_color_t group_stroke;     
    ic_color_t bar_edge;         

    
    ic_color_t accent;
    ic_color_t accent_hover;
    ic_color_t accent_pressed;
    ic_color_t accent_soft;      
    ic_color_t focus_ring;
    ic_color_t success;
    ic_color_t warning;
    ic_color_t danger;
    ic_color_t close_hover;      
} ic_palette_t;



const ic_palette_t *ic_palette(void);

int  ic_palette_reload(void);

void ic_palette_build(ic_palette_t *p, int dark, ic_accent_t accent);

ic_color_t ic_accent_swatch(ic_accent_t accent);

#endif 
