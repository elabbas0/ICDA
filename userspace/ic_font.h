















#ifndef USERSPACE_IC_FONT_H
#define USERSPACE_IC_FONT_H

#include <stdint.h>
#include "ic_gfx.h"

typedef enum {
    IC_FONT_CAPTION = 0,   
    IC_FONT_CAPTION_EMPH,  
    IC_FONT_FOOTNOTE,      
    IC_FONT_BODY,          
    IC_FONT_BODY_EMPH,     
    IC_FONT_HEADLINE,      
    IC_FONT_SUBHEAD,       
    IC_FONT_TITLE3,        
    IC_FONT_TITLE2,        
    IC_FONT_TITLE1,        
    IC_FONT_LARGE_TITLE,   
    IC_FONT_MONO,          
    IC_FONT_MONO_SMALL,    
    IC_FONT_STYLE_COUNT
} ic_font_style_t;

typedef struct {
    uint8_t  w, h;      
    int8_t   ox, oy;    
    uint32_t off;       
} ic_fglyph_t;

typedef struct {
    uint8_t  l, r;      
    int16_t  units;     
} ic_fkern_t;

typedef struct {
    uint8_t            px;        
    uint8_t            ascent;
    uint8_t            descent;
    uint8_t            line_h;
    uint8_t            cap_h;
    uint8_t            x_h;
    uint16_t           upem;
    const uint16_t    *adv;       
    const ic_fglyph_t *glyphs;    
    const uint8_t     *alpha;
    const ic_fkern_t  *kern;      
    uint16_t           nkern;
} ic_face_t;

typedef enum {
    IC_ALIGN_LEFT = 0,
    IC_ALIGN_CENTER,
    IC_ALIGN_RIGHT
} ic_align_t;

const ic_face_t *ic_font(ic_font_style_t style);
int  ic_font_attach_2x(const void *blob, uint64_t len);


int  ic_text_measure(const ic_face_t *f, const char *s);

int  ic_text_measure_n(const ic_face_t *f, const char *s, int n);

int  ic_text_fit(const ic_face_t *f, const char *s, int max_w);


int  ic_text_draw(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                  ic_color_t color);
int  ic_text_draw_n(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                    int n, ic_color_t color);


int  ic_text_center_baseline(const ic_face_t *f, int y, int h);



void ic_text_draw_in(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                     ic_color_t color, ic_align_t align);




int  ic_text_draw_wrapped(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                          ic_color_t color, int line_gap);

#endif 
