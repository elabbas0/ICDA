


#include "ic_anim.h"
#include "ic_time.h"



static float ic_bezier_axis(float p1, float p2, float u) {
    
    float v = 1.0f - u;
    return 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u;
}

static float ic_bezier_axis_slope(float p1, float p2, float u) {
    float v = 1.0f - u;
    return 3.0f * v * v * p1 + 6.0f * v * u * (p2 - p1) + 3.0f * u * u * (1.0f - p2);
}

float ic_cubic_bezier(float x1, float y1, float x2, float y2, float t) {
    float u = t;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    
    for (int i = 0; i < 6; i++) {
        float err = ic_bezier_axis(x1, x2, u) - t;
        float d = ic_bezier_axis_slope(x1, x2, u);
        if (err < 1e-4f && err > -1e-4f) return ic_bezier_axis(y1, y2, u);
        if (d < 1e-5f && d > -1e-5f) break;
        u -= err / d;
    }
    {
        float lo = 0.0f, hi = 1.0f;
        u = t;
        for (int i = 0; i < 20; i++) {
            float x = ic_bezier_axis(x1, x2, u);
            if (x < t) lo = u; else hi = u;
            u = 0.5f * (lo + hi);
        }
    }
    return ic_bezier_axis(y1, y2, u);
}

float ic_ease(ic_ease_t ease, float t) {
    t = ic_clampf(t, 0.0f, 1.0f);
    switch (ease) {
    case IC_EASE_STANDARD:   return ic_cubic_bezier(0.25f, 0.10f, 0.25f, 1.00f, t);
    case IC_EASE_DECELERATE: return ic_cubic_bezier(0.00f, 0.00f, 0.20f, 1.00f, t);
    case IC_EASE_ACCELERATE: return ic_cubic_bezier(0.40f, 0.00f, 1.00f, 1.00f, t);
    case IC_EASE_EMPHASIZED: return ic_cubic_bezier(0.20f, 0.00f, 0.00f, 1.00f, t);
    case IC_EASE_LINEAR:
    default:                 return t;
    }
}



void ic_tween_set(ic_tween_t *tw, float value) {
    if (!tw) return;
    tw->from = value;
    tw->to = value;
    tw->start_ns = 0;
    tw->duration_ms = 0;
    tw->ease = IC_EASE_LINEAR;
    tw->active = 0;
}

float ic_tween_progress(const ic_tween_t *tw) {
    uint64_t elapsed;
    if (!tw || !tw->active || tw->duration_ms == 0) return 1.0f;
    elapsed = ic_time_ns() - tw->start_ns;
    return ic_clampf((float)elapsed / ((float)tw->duration_ms * 1e6f), 0.0f, 1.0f);
}

float ic_tween_value(ic_tween_t *tw) {
    float p;
    if (!tw) return 0.0f;
    if (!tw->active) return tw->to;
    p = ic_tween_progress(tw);
    if (p >= 1.0f) {
        tw->active = 0;
        return tw->to;
    }
    return ic_lerpf(tw->from, tw->to, ic_ease(tw->ease, p));
}

void ic_tween_to(ic_tween_t *tw, float to, uint32_t duration_ms, ic_ease_t ease) {
    if (!tw) return;
    if (duration_ms == 0) {
        ic_tween_set(tw, to);
        return;
    }
    tw->from = ic_tween_value(tw);
    tw->to = to;
    tw->start_ns = ic_time_ns();
    tw->duration_ms = duration_ms;
    tw->ease = ease;
    tw->active = tw->from != to;
}

int ic_tween_running(ic_tween_t *tw) {
    if (!tw || !tw->active) return 0;
    (void)ic_tween_value(tw);
    return tw->active;
}



#define IC_SPRING_STEP_S  0.004f   
#define IC_SPRING_MAX_DT  0.100f   
#define IC_SPRING_REST_X  0.0015f  
#define IC_SPRING_REST_V  0.02f

void ic_spring_init(ic_spring_t *s, float value, float response, float damping) {
    float omega;
    if (!s) return;
    if (response < 0.05f) response = 0.05f;
    omega = 6.2831853f / response;
    s->stiffness = omega * omega;
    s->friction = 2.0f * damping * omega;
    s->value = value;
    s->target = value;
    s->velocity = 0.0f;
    s->last_ns = 0;
    s->settled = 1;
}

void ic_spring_snap(ic_spring_t *s, float value) {
    if (!s) return;
    s->value = value;
    s->target = value;
    s->velocity = 0.0f;
    s->settled = 1;
}

void ic_spring_to(ic_spring_t *s, float target) {
    if (!s) return;
    if (s->settled) s->last_ns = ic_time_ns();
    s->target = target;
    s->settled = 0;
}

float ic_spring_value(ic_spring_t *s) {
    uint64_t now;
    float dt;
    if (!s) return 0.0f;
    if (s->settled) return s->value;
    now = ic_time_ns();
    dt = (float)(now - s->last_ns) * 1e-9f;
    s->last_ns = now;
    if (dt > IC_SPRING_MAX_DT) dt = IC_SPRING_MAX_DT;
    while (dt > 0.0f) {
        float h = dt < IC_SPRING_STEP_S ? dt : IC_SPRING_STEP_S;
        
        float accel = -s->stiffness * (s->value - s->target) - s->friction * s->velocity;
        s->velocity += accel * h;
        s->value += s->velocity * h;
        dt -= h;
    }
    {
        float scale = s->target != 0.0f ? (s->target < 0 ? -s->target : s->target) : 1.0f;
        float dx = s->value - s->target;
        if (scale < 1.0f) scale = 1.0f;
        if (dx < 0) dx = -dx;
        if (dx < IC_SPRING_REST_X * scale &&
            (s->velocity < 0 ? -s->velocity : s->velocity) < IC_SPRING_REST_V * scale) {
            s->value = s->target;
            s->velocity = 0.0f;
            s->settled = 1;
        }
    }
    return s->value;
}

int ic_spring_running(ic_spring_t *s) {
    if (!s) return 0;
    (void)ic_spring_value(s);
    return !s->settled;
}
