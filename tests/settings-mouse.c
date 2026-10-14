#include <assert.h>
#include <string.h>
#include "settings_store.h"
static void line(icda_settings_t *s, const char *text) {
    icda_settings_apply_line(s, text, strlen(text));
}
int main(void) {
    icda_settings_t s;
    icda_settings_defaults(&s);
    assert(s.mouse_speed == 100 && s.mouse_smooth == 0 && s.mouse_accel == 0);
    line(&s, "mouse_speed=25"); assert(s.mouse_speed == 25);
    line(&s, "mouse_speed=300"); assert(s.mouse_speed == 300);
    line(&s, "mouse_speed=0"); assert(s.mouse_speed == 300);
    line(&s, "mouse_speed=301"); assert(s.mouse_speed == 300);
    line(&s, "mouse_speed=100oops"); assert(s.mouse_speed == 300);
    line(&s, "mouse_speed=150\r"); assert(s.mouse_speed == 150);
    line(&s, "mouse_smooth=3"); assert(s.mouse_smooth == 3);
    line(&s, "mouse_smooth=4"); assert(s.mouse_smooth == 3);
    line(&s, "mouse_accel=1"); assert(s.mouse_accel == 1);
    line(&s, "mouse_accel=2"); assert(s.mouse_accel == 1);
    line(&s, "mouse_accel="); assert(s.mouse_accel == 1);
    return 0;
}
