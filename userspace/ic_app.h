















#ifndef USERSPACE_IC_APP_H
#define USERSPACE_IC_APP_H

#include <stdint.h>
#include "ic_gfx.h"



enum {
    IC_KEY_BACKSPACE = 8,
    IC_KEY_TAB       = 9,
    IC_KEY_ENTER     = 13,
    IC_KEY_ESCAPE    = 27,
    IC_KEY_UP        = 0x100,
    IC_KEY_DOWN,
    IC_KEY_LEFT,
    IC_KEY_RIGHT,
    IC_KEY_DELETE,
    IC_KEY_HOME,
    IC_KEY_END,
    IC_KEY_PAGE_UP,
    IC_KEY_PAGE_DOWN
};

typedef enum {
    IC_EV_MOUSE_MOVE = 1,   
    IC_EV_MOUSE_DOWN,       
    IC_EV_MOUSE_UP,         
    IC_EV_MOUSE_LEAVE,      
    IC_EV_KEY,              
    IC_EV_FOCUS,            
    IC_EV_BLUR,             
    IC_EV_RESIZE,           
    IC_EV_APPEARANCE,       
    IC_EV_SCROLL            
} ic_event_type_t;

typedef struct {
    ic_event_type_t type;
    int             x, y;
    uint8_t         button;   
    uint32_t        key;
    int             wheel;    
} ic_event_t;

typedef struct ic_app ic_app_t;

typedef struct {
    const char *title;
    int         width, height;
    
    void (*init)(ic_app_t *app);
    
    void (*draw)(ic_app_t *app, ic_canvas_t *c);
    
    void (*event)(ic_app_t *app, const ic_event_t *ev);
    

    void (*tick)(ic_app_t *app);
} ic_app_desc_t;

struct ic_app {
    const ic_app_desc_t *desc;
    void    *user;
    int      width, height;
    int      focused;
    int      mouse_x, mouse_y;  
    int      mouse_inside;
    uint8_t  buttons;           
    
    int      dirty;
    int      animating;
    int      wants_caret;
    int      quit;
    uint64_t caret_epoch_ms;
};

int  ic_app_run(const ic_app_desc_t *desc, void *user);


void ic_app_invalidate(ic_app_t *app);


void ic_app_animate(ic_app_t *app);

int  ic_app_caret_visible(ic_app_t *app);

void ic_app_caret_reset(ic_app_t *app);

void ic_app_quit(ic_app_t *app);

#endif 
