#include "animation.h"
#include <math.h>
#include <time.h>

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double easing_value(EasingCurve curve, double t) {
    switch (curve) {
    case EASING_LINEAR:
        return t;
    case EASING_EASE_IN:
        return t * t * t;
    case EASING_EASE_OUT:
        return 1.0 - pow(1.0 - t, 3.0);
    case EASING_EASE_IN_OUT:
        if (t < 0.5) return 4.0 * t * t * t;
        return 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
    }
    return t;
}

int64_t animation_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void animation_init(AnimatedValue *value, double initial) {
    value->start = initial;
    value->current = initial;
    value->target = initial;
    value->velocity = 0.0;
    value->duration_ms = 0;
    value->start_time = 0;
    value->curve = EASING_LINEAR;
    value->active = false;
}

void animation_set_target(AnimatedValue *value, double target, int duration_ms, EasingCurve curve) {
    if (duration_ms <= 0 || value->current == target) {
        value->start = target;
        value->current = target;
        value->target = target;
        value->velocity = 0.0;
        value->duration_ms = 0;
        value->start_time = 0;
        value->curve = curve;
        value->active = false;
        return;
    }

    value->start = value->current;
    value->target = target;
    value->duration_ms = duration_ms;
    value->start_time = animation_now_ms();
    value->curve = curve;
    value->active = true;
}

void animation_update(AnimatedValue *value, int64_t now_ms) {
    if (!value || !value->active) return;

    if (value->duration_ms <= 0) {
        value->start = value->target;
        value->current = value->target;
        value->velocity = 0.0;
        value->active = false;
        return;
    }

    int64_t elapsed = now_ms - value->start_time;
    if (elapsed >= value->duration_ms) {
        value->start = value->target;
        value->current = value->target;
        value->velocity = 0.0;
        value->active = false;
        return;
    }

    double previous = value->current;
    double t = (double)elapsed / (double)value->duration_ms;
    double progress = easing_value(value->curve, clamp01(t));

    value->current = value->start + (value->target - value->start) * progress;
    value->velocity = value->current - previous;
}

bool animation_finished(AnimatedValue *value) {
    return !value->active;
}
