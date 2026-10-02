/*
 * ic_anim.h - time-based motion: easing curves, tweens and springs.
 *
 * Everything runs on ic_time_ns(), so an animation takes the same wall
 * time regardless of frame rate.  Durations and curves used by the
 * shell and apps come from the motion tokens in ic_theme.h; code should
 * not invent its own timings.
 */
#ifndef USERSPACE_IC_ANIM_H
#define USERSPACE_IC_ANIM_H

#include <stdint.h>

/* ------------------------------------------------------------ easing */

typedef enum {
    IC_EASE_LINEAR = 0,
    IC_EASE_STANDARD,     /* cubic-bezier(0.25, 0.10, 0.25, 1.00) */
    IC_EASE_DECELERATE,   /* cubic-bezier(0.00, 0.00, 0.20, 1.00) - things arriving */
    IC_EASE_ACCELERATE,   /* cubic-bezier(0.40, 0.00, 1.00, 1.00) - things leaving */
    IC_EASE_EMPHASIZED    /* cubic-bezier(0.20, 0.00, 0.00, 1.00) - large moves */
} ic_ease_t;

/* Evaluate a CSS-style cubic-bezier timing curve at progress t (0..1). */
float ic_cubic_bezier(float x1, float y1, float x2, float y2, float t);
float ic_ease(ic_ease_t ease, float t);

static inline float ic_lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float ic_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------- tween */

typedef struct {
    float     from;
    float     to;
    uint64_t  start_ns;
    uint32_t  duration_ms;
    ic_ease_t ease;
    int       active;
} ic_tween_t;

/* Jump to a value with no animation. */
void  ic_tween_set(ic_tween_t *tw, float value);
/* Animate from the tween's current value (so interrupting a running
 * tween never jumps) to `to`. */
void  ic_tween_to(ic_tween_t *tw, float to, uint32_t duration_ms, ic_ease_t ease);
float ic_tween_value(ic_tween_t *tw);
/* Linear progress 0..1 (1 when idle). */
float ic_tween_progress(const ic_tween_t *tw);
int   ic_tween_running(ic_tween_t *tw);

/* ------------------------------------------------------------ spring */
/* Damped harmonic oscillator, parameterised like modern UI toolkits:
 * `response` is the period of the undamped spring in seconds (smaller =
 * snappier) and `damping` the damping ratio (1 = critically damped, no
 * overshoot; 0.7-0.85 = a gentle settle). */

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
/* Advance to now and return the current value. */
float ic_spring_value(ic_spring_t *s);
int   ic_spring_running(ic_spring_t *s);

#endif /* USERSPACE_IC_ANIM_H */
