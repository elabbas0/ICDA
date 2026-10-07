#ifndef MEDIA_MEDIA_H
#define MEDIA_MEDIA_H

#include <stdint.h>
#include <stddef.h>
#include "ic_app.h"
#include "ic_ui.h"

enum { MEDIA_AUDIO = 1, MEDIA_VIDEO, MEDIA_PHOTO };

int      media_kind_of(const char *name);
uint8_t *media_read_file(const char *path, size_t *len);

/* a byte source for the player: a file in memory, or a download still
 * growing (pump() adds to it from the tick) */
typedef struct vsrc vsrc_t;
struct vsrc {
    uint8_t *data;
    size_t   len, total;        /* bytes here, bytes expected (0 unknown) */
    int      done, failed;
    char     error[200];
    void   (*pump)(vsrc_t *s);
    void   (*close)(vsrc_t *s);  /* frees everything, the struct too */
    void    *user;
};
vsrc_t *vsrc_memory(uint8_t *data, size_t len);

/* video.c: the video player face */
enum { VIDEO_EV_NONE = 0, VIDEO_EV_LIBRARY };
int  video_open(const char *path, char *err, size_t errcap);
int  video_open_url(const char *url, const char *title, char *err, size_t errcap);
int  video_open_sources(vsrc_t **srcs, int n, const char *title, char *err, size_t errcap);
int  video_is_open(void);
void video_close(void);
void video_draw(ic_app_t *app, ic_canvas_t *c);
int  video_tick(ic_app_t *app);              /* 1 when a redraw is due */
int  video_event(ic_app_t *app, const ic_event_t *ev);

/* youtube.c: search and pick, inside the library's YouTube tab */
void        youtube_enter(void);
void        youtube_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
int         youtube_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);
int         youtube_tick(ic_app_t *app, char *play_url, size_t cap);
const char *youtube_title(void);

#endif
