#ifndef GUI_PROTO_H
#define GUI_PROTO_H

#include <stdint.h>

/* Well-known queue name the WM listens on */
#define WM_QUEUE_NAME  "/wm/events"

/* GUI message types */
#define GUI_MSG_OPEN_WINDOW    1   /* app -> WM: request a new window */
#define GUI_MSG_OPEN_OK        2   /* WM -> app: window created */
#define GUI_MSG_OPEN_FAIL      3   /* WM -> app: window creation failed */
#define GUI_MSG_CLOSE_WINDOW   4   /* app -> WM or WM -> app */
#define GUI_MSG_FLUSH          5   /* app -> WM: mark window dirty, repaint */
#define GUI_MSG_KEY_EVENT      6   /* WM -> app: key press/release */
#define GUI_MSG_MOUSE_EVENT    7   /* WM -> app: mouse in window coords */
#define GUI_MSG_RESIZE         8   /* WM -> app: window was resized */
#define GUI_MSG_FOCUS          9   /* WM -> app: gained/lost focus */

/* Mouse button bits */
#define GUI_BTN_LEFT    0x01
#define GUI_BTN_RIGHT   0x02
#define GUI_BTN_MIDDLE  0x04

/*
 * Fixed-size 64-byte message used in every queue transfer.
 * All fields are little-endian.
 */
typedef struct {
    uint32_t type;        /* GUI_MSG_* */
    uint32_t window_id;   /* identifies which window */
    union {
        /* GUI_MSG_OPEN_WINDOW: app -> WM */
        struct {
            int32_t  w, h;
            char     title[32];
        } open_req;       /* 40 bytes */

        /* GUI_MSG_OPEN_OK: WM -> app */
        struct {
            uint64_t shm_handle;  /* kernel SHM handle for pixel buffer */
            int32_t  w, h;
            uint64_t reply_queue; /* WM-assigned queue handle for this window */
            uint8_t  _pad[16];
        } open_ok;        /* 40 bytes */

        /* GUI_MSG_KEY_EVENT */
        struct {
            uint32_t keycode;     /* ASCII or special key code */
            uint8_t  pressed;     /* 1=press, 0=release */
            uint8_t  _pad[35];
        } key;            /* 40 bytes */

        /* GUI_MSG_MOUSE_EVENT */
        struct {
            int32_t  x, y;        /* position in window-local coords */
            uint8_t  buttons;     /* GUI_BTN_* bitmask */
            uint8_t  _pad[31];
        } mouse;          /* 40 bytes */

        /* GUI_MSG_FOCUS: WM -> app */
        struct {
            uint8_t  focused;     /* 1 = gained focus, 0 = lost */
            uint8_t  _pad[39];
        } focus;          /* 40 bytes */

        /* GUI_MSG_RESIZE: WM -> app */
        struct {
            uint64_t shm_handle;  /* new window pixel buffer */
            int32_t  w, h;
            uint8_t  _pad[24];
        } resize;         /* 40 bytes */

        /* GUI_MSG_FLUSH: app -> WM (no extra payload) */
        /* GUI_MSG_CLOSE_WINDOW: no extra payload */

        uint8_t raw[40];
    };
    uint8_t _tail[16]; /* padding to 64 bytes total */
} __attribute__((packed)) gui_msg_t;

/* Compile-time size check: sizeof(gui_msg_t) must equal 64 */
typedef char _gui_msg_size_check[
    (sizeof(gui_msg_t) == 64) ? 1 : -1
];

#endif /* GUI_PROTO_H */
