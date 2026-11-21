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
/* app -> WM: a request was just written to /dev/dialog (look now);
 * WM -> app: its dialog was answered (dialog.button, -1 none) */
#define GUI_MSG_DIALOG        11
/* app -> WM: how a window wants its input (games).  raw_keys: every key
 * press and release also comes as GUI_KEY_RAW | its PC set-1 code (| 0x100
 * for an E0-prefixed key), with key.pressed 1 or 0, besides the characters.
 * pointer_lock: while the window has the focus the pointer is hidden and
 * stays put, and the mouse's motion comes as relative events (mouse.relative,
 * mouse.x/y the motion). */
#define GUI_MSG_INPUT_MODE    12

#define GUI_KEY_RAW     0x2000000u


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
            uint64_t font_shm;
            uint8_t  scale;
            uint8_t  _pad[7];
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
            uint8_t  relative;    /* pointer lock: x, y are the motion */
            uint8_t  _pad[29];
        } mouse;          

        
        struct {
            uint8_t  focused;     
            uint8_t  _pad[39];
        } focus;          

        struct {
            uint8_t  shape;
            uint8_t  _pad[39];
        } cursor;

        /* GUI_MSG_DIALOG: the request's id in /dev/dialog, and the answer */
        struct {
            uint32_t id;
            int32_t  button;
            uint8_t  _pad[32];
        } dialog;

        /* GUI_MSG_INPUT_MODE */
        struct {
            uint8_t  raw_keys;
            uint8_t  pointer_lock;
            uint8_t  _pad[38];
        } input_mode;

        /* GUI_MSG_FLUSH: changed area in window coordinates; w == 0 means everything */
        struct {
            int32_t  x, y, w, h;
            uint8_t  _pad[24];
        } damage;

        
        struct {
            uint64_t shm_handle;  
            int32_t  w, h;
            uint8_t  scale;
            uint8_t  _pad[23];
        } resize;         

        
        

        uint8_t raw[40];
    };
    /* GUI_MSG_OPEN_WINDOW: the asking program's pid, so the window manager
     * knows whose window it is (a program that stopped takes its windows) */
    uint32_t sender_pid;
    uint8_t _tail[12];
} __attribute__((packed)) gui_msg_t;


typedef char _gui_msg_size_check[
    (sizeof(gui_msg_t) == 64) ? 1 : -1
];

#endif 
