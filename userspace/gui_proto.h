#ifndef GUI_PROTO_H
#define GUI_PROTO_H

#include <stdint.h>


#define WM_QUEUE_NAME  "/wm/events"


#define GUI_MSG_OPEN_WINDOW    1   
#define GUI_MSG_OPEN_OK        2   
#define GUI_MSG_OPEN_FAIL      3   
#define GUI_MSG_CLOSE_WINDOW   4   
#define GUI_MSG_FLUSH          5   
#define GUI_MSG_KEY_EVENT      6   
#define GUI_MSG_MOUSE_EVENT    7   
#define GUI_MSG_RESIZE         8   
#define GUI_MSG_FOCUS          9   
#define GUI_MSG_SET_CURSOR    10   


#define GUI_BTN_LEFT    0x01
#define GUI_BTN_RIGHT   0x02
#define GUI_BTN_MIDDLE  0x04





typedef struct {
    uint32_t type;        
    uint32_t window_id;   
    union {
        
        struct {
            int32_t  w, h;
            char     title[32];
        } open_req;       

        
        struct {
            uint64_t shm_handle;  
            int32_t  w, h;
            uint64_t reply_queue; 
            uint8_t  _pad[16];
        } open_ok;        

        
        struct {
            uint32_t keycode;     
            uint8_t  pressed;     
            uint8_t  _pad[35];
        } key;            

        
        struct {
            int32_t  x, y;        
            uint8_t  buttons;     
            int8_t   wheel;       
            uint8_t  _pad[30];
        } mouse;          

        
        struct {
            uint8_t  focused;     
            uint8_t  _pad[39];
        } focus;          

        struct {
            uint8_t  shape;
            uint8_t  _pad[39];
        } cursor;

        
        struct {
            uint64_t shm_handle;  
            int32_t  w, h;
            uint8_t  _pad[24];
        } resize;         

        
        

        uint8_t raw[40];
    };
    uint8_t _tail[16]; 
} __attribute__((packed)) gui_msg_t;


typedef char _gui_msg_size_check[
    (sizeof(gui_msg_t) == 64) ? 1 : -1
];

#endif 
