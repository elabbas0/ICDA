/* WebKit (WPE) in an ICDA window.
 *
 * WPE renders web pages without a toolkit: the web process draws frames
 * into shared-memory buffers (WPEBackend-fdo's SHM mode, with Mesa's
 * software renderer underneath) and hands them to this process, which
 * copies each into an ICDA desktop window.  Mouse, wheel and keyboard
 * events from the window go back to WebKit.
 *
 * It runs as a Linux program on ICDA's Linux personality; the window comes
 * from ICDA's own window manager through the kernel's gateway (gui.c built
 * with ICDA_SYS_BASE).
 *
 * usage: wpe-icda [url] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wayland-server.h>
#include "gui.h"

#define WIN_W 1024
#define WIN_H 700

static struct wpe_view_backend_exportable_fdo *exportable;
static struct wpe_view_backend *view_backend;
static WebKitWebView *view;
static int win_w = WIN_W, win_h = WIN_H;
static uint32_t pointer_buttons;
static int pointer_x, pointer_y;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ---- frames: web process -> window ------------------------------------------ */

static void on_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer) {
    struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    uint32_t *dst = gui_pixel_buffer();
    (void)data;
    if (shm && dst) {
        int w = wl_shm_buffer_get_width(shm), h = wl_shm_buffer_get_height(shm);
        int stride = wl_shm_buffer_get_stride(shm);
        int dw = gui_window_width(), dh = gui_window_height();
        wl_shm_buffer_begin_access(shm);
        {
            const uint8_t *src = wl_shm_buffer_get_data(shm);
            int cw = w < dw ? w : dw, ch = h < dh ? h : dh;
            for (int y = 0; y < ch; y++) {
                const uint32_t *s = (const uint32_t *)(src + (size_t)y * (size_t)stride);
                uint32_t *d = dst + (size_t)y * (size_t)dw;
                for (int x = 0; x < cw; x++) d[x] = s[x] | 0xFF000000u;
            }
        }
        wl_shm_buffer_end_access(shm);
        gui_flush();
    }
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(exportable);
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(exportable, buffer);
}

/* ---- input: window -> web view --------------------------------------------------- */

/* ICDA sends keys as a terminal-style byte stream (ic_app.c decodes the same) */
static struct { int state, p1, p2; } kd;

static void send_key(uint32_t keysym, uint32_t mods) {
    struct wpe_input_keyboard_event ev;
    uint32_t wm = 0;
    if (mods & 1) wm |= wpe_input_keyboard_modifier_shift;
    if (mods & 2) wm |= wpe_input_keyboard_modifier_alt;
    if (mods & 4) wm |= wpe_input_keyboard_modifier_control;
    memset(&ev, 0, sizeof ev);
    ev.time = now_ms();
    ev.key_code = keysym;
    ev.modifiers = wm;
    ev.pressed = true;
    wpe_view_backend_dispatch_keyboard_event(view_backend, &ev);
    ev.pressed = false;
    wpe_view_backend_dispatch_keyboard_event(view_backend, &ev);
}

static uint32_t keysym_of(uint32_t c) {
    switch (c) {
    case 8: case 127: return 0xff08;      /* BackSpace */
    case 9: return 0xff09;                /* Tab */
    case 13: case 10: return 0xff0d;      /* Return */
    case 27: return 0xff1b;               /* Escape */
    }
    if (c < 0x100) return c;              /* Latin-1 keysyms are the code points */
    return 0x01000000u | c;
}

static void plain_key(uint32_t c, uint32_t mods) {
    if (c >= 1 && c <= 26 && c != 8 && c != 9 && c != 13) send_key('a' + c - 1, mods | 4);   /* Ctrl+letter */
    else send_key(keysym_of(c), mods | ((c >= 'A' && c <= 'Z') ? 1 : 0));
}

static void feed_key(uint32_t c) {
    if (kd.state == 0) {
        if (c == 27) { kd.state = 1; return; }
        plain_key(c, 0);
        return;
    }
    if (kd.state == 1) {
        if (c == '[') { kd.state = 2; kd.p1 = kd.p2 = 0; return; }
        kd.state = 0;
        if (c >= 32 && c < 127) { plain_key(c, 2); return; }   /* Alt+key */
        send_key(0xff1b, 0);
        feed_key(c);
        return;
    }
    if (c >= '0' && c <= '9') {
        if (kd.state == 3) kd.p2 = kd.p2 * 10 + (int)(c - '0');
        else kd.p1 = kd.p1 * 10 + (int)(c - '0');
        return;
    }
    if (c == ';') { kd.state = 3; return; }
    {
        uint32_t mods = kd.p2 > 1 ? (uint32_t)(kd.p2 - 1) : 0;
        kd.state = 0;
        switch (c) {
        case 'A': send_key(0xff52, mods); return;   /* Up */
        case 'B': send_key(0xff54, mods); return;   /* Down */
        case 'C': send_key(0xff53, mods); return;   /* Right */
        case 'D': send_key(0xff51, mods); return;   /* Left */
        case 'H': send_key(0xff50, mods); return;   /* Home */
        case 'F': send_key(0xff57, mods); return;   /* End */
        case 'Z': send_key(0xff09, mods | 1); return;
        case '~':
            if (kd.p1 == 3) send_key(0xffff, mods);              /* Delete */
            else if (kd.p1 == 2) send_key(0xff63, mods);         /* Insert */
            else if (kd.p1 == 1 || kd.p1 == 7) send_key(0xff50, mods);
            else if (kd.p1 == 4 || kd.p1 == 8) send_key(0xff57, mods);
            else if (kd.p1 == 5) send_key(0xff55, mods);         /* Page Up */
            else if (kd.p1 == 6) send_key(0xff56, mods);         /* Page Down */
            return;
        }
    }
}

static void pointer(int x, int y, uint32_t buttons, int wheel) {
    struct wpe_input_pointer_event ev;
    memset(&ev, 0, sizeof ev);
    ev.time = now_ms();
    ev.x = x;
    ev.y = y;
    if (x != pointer_x || y != pointer_y) {
        ev.type = wpe_input_pointer_event_type_motion;
        ev.modifiers = pointer_buttons & GUI_BTN_LEFT ? wpe_input_pointer_modifier_button1 : 0;
        wpe_view_backend_dispatch_pointer_event(view_backend, &ev);
        pointer_x = x;
        pointer_y = y;
    }
    for (int i = 0; i < 3; i++) {
        static const uint32_t bit[3] = { GUI_BTN_LEFT, GUI_BTN_RIGHT, GUI_BTN_MIDDLE };
        static const uint32_t wpe_button[3] = { 1, 3, 2 };
        if ((buttons & bit[i]) == (pointer_buttons & bit[i])) continue;
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = wpe_button[i];
        ev.state = (buttons & bit[i]) ? 1 : 0;
        wpe_view_backend_dispatch_pointer_event(view_backend, &ev);
    }
    pointer_buttons = buttons;
    if (wheel) {
        struct wpe_input_axis_2d_event ax;
        memset(&ax, 0, sizeof ax);
        ax.base.type = (enum wpe_input_axis_event_type)(wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
        ax.base.time = now_ms();
        ax.base.x = x;
        ax.base.y = y;
        ax.y_axis = -wheel * 53.0;
        wpe_view_backend_dispatch_axis_event(view_backend, &ax.base);
    }
}

static gboolean poll_window(gpointer data) {
    gui_msg_t m;
    (void)data;
    while (gui_poll_event(&m)) {
        switch (m.type) {
        case GUI_MSG_KEY_EVENT:
            if (m.key.pressed) feed_key(m.key.keycode);
            break;
        case GUI_MSG_MOUSE_EVENT:
            pointer(m.mouse.x, m.mouse.y, m.mouse.buttons, m.mouse.wheel);
            break;
        case GUI_MSG_RESIZE:
            win_w = gui_window_width();
            win_h = gui_window_height();
            wpe_view_backend_dispatch_set_size(view_backend, (uint32_t)win_w, (uint32_t)win_h);
            break;
        case GUI_MSG_FOCUS:
            if (m.focus.focused) wpe_view_backend_add_activity_state(view_backend, wpe_view_activity_state_focused);
            else wpe_view_backend_remove_activity_state(view_backend, wpe_view_activity_state_focused);
            break;
        case GUI_MSG_CLOSE_WINDOW:
            exit(0);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_title(WebKitWebView *v, GParamSpec *ps, gpointer data) {
    (void)ps; (void)data;
    fprintf(stderr, "wpe-icda: %s\n", webkit_web_view_get_title(v) ? webkit_web_view_get_title(v) : "");
}

static void on_load(WebKitWebView *v, WebKitLoadEvent ev, gpointer data) {
    (void)data;
    if (ev == WEBKIT_LOAD_FINISHED) fprintf(stderr, "wpe-icda: loaded %s\n", webkit_web_view_get_uri(v));
}

static gboolean on_fail(WebKitWebView *v, WebKitLoadEvent ev, char *uri, GError *err, gpointer data) {
    (void)v; (void)ev; (void)data;
    fprintf(stderr, "wpe-icda: failed %s: %s\n", uri, err ? err->message : "?");
    return FALSE;
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://example.com";
    struct wpe_view_backend_exportable_fdo_client client;
    WebKitWebViewBackend *backend;
    WebKitSettings *settings;
    GMainLoop *loop;

    if (gui_open_window("WebKit", WIN_W, WIN_H) != 0) {
        fprintf(stderr, "wpe-icda: no desktop window\n");
        return 1;
    }
    win_w = gui_window_width();
    win_h = gui_window_height();

    wpe_loader_init("libWPEBackend-fdo-1.0.so.1");
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "wpe-icda: WPEBackend-fdo shared memory mode failed\n");
        return 1;
    }
    memset(&client, 0, sizeof client);
    client.export_shm_buffer = on_export_shm_buffer;
    exportable = wpe_view_backend_exportable_fdo_create(&client, NULL, (uint32_t)win_w, (uint32_t)win_h);
    view_backend = wpe_view_backend_exportable_fdo_get_view_backend(exportable);
    backend = webkit_web_view_backend_new(view_backend, NULL, NULL);

    settings = webkit_settings_new();
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15");
    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "settings", settings, NULL));
    wpe_view_backend_add_activity_state(view_backend, wpe_view_activity_state_visible | wpe_view_activity_state_focused |
                                                     wpe_view_activity_state_in_window);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_fail), NULL);
    webkit_web_view_load_uri(view, url);
    fprintf(stderr, "wpe-icda: loading %s in %dx%d\n", url, win_w, win_h);

    g_timeout_add(8, poll_window, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
