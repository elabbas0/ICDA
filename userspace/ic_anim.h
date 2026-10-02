







#ifndef USERSPACE_IC_ANIM_H
#define USERSPACE_IC_ANIM_H

#include <stdint.h>



typedef enum {
    IC_EASE_LINEAR = 0,
    IC_EASE_STANDARD,     
    IC_EASE_DECELERATE,   
    IC_EASE_ACCELERATE,   
    IC_EASE_EMPHASIZED    
} ic_ease_t;


float ic_cubic_bezier(float x1, float y1, float x2, float y2, float t);
float ic_ease(ic_ease_t ease, float t);

static inline float ic_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float ic_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}



typedef struct {
    float     from;
    float     to;
    uint64_t  start_ns;
    uint32_t  duration_ms;
    ic_ease_t ease;
    int       active;
} ic_tween_t;


void  ic_tween_set(ic_tween_t *tw, float value);


void  ic_tween_to(ic_tween_t *tw, float to, uint32_t duration_ms, ic_ease_t ease);
float ic_tween_value(ic_tween_t *tw);

float ic_tween_progress(const ic_tween_t *tw);
int   ic_tween_running(ic_tween_t *tw);







typedef struct {
    float    value;
    float    velocity;
    float    target;
    float    stiffness;
    float    friction;
    uint64_t last_ns;
    int      settled;
} ic_spring_t;

void  ic_spring_init(ic_spring_t *s, float value, float response, float damping);
void  ic_spring_to(ic_spring_t *s, float target);
void  ic_spring_snap(ic_spring_t *s, float value);

float ic_spring_value(ic_spring_t *s);
int   ic_spring_running(ic_spring_t *s);

#endif 
