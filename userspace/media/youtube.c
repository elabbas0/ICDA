/* YouTube tab of Media (placeholder). */
#include "media.h"

void youtube_enter(void) {}
void youtube_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area) {
    (void)app;
    ic_ui_empty_state(c, area, IC_SYM_PLAY, "YouTube", "Playing YouTube videos here is not available yet.");
}
int youtube_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area) { (void)app; (void)ev; (void)area; return 0; }
int youtube_tick(ic_app_t *app, char *play_url, size_t cap) { (void)app; if (cap) play_url[0] = 0; return 0; }
const char *youtube_title(void) { return ""; }
