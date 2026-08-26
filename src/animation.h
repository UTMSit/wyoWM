#ifndef ANIMATION_H
#define ANIMATION_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EASING_LINEAR,
    EASING_EASE_IN,
    EASING_EASE_OUT,
    EASING_EASE_IN_OUT
} EasingCurve;

typedef struct {
    double start;
    double current;
    double target;
    double velocity;
    int duration_ms;
    int64_t start_time;
    EasingCurve curve;
    bool active;
} AnimatedValue;

void animation_init(AnimatedValue *value, double initial);
void animation_set_target(AnimatedValue *value, double target, int duration_ms, EasingCurve curve);
void animation_update(AnimatedValue *value, int64_t now_ms);
bool animation_finished(AnimatedValue *value);
int64_t animation_now_ms(void);

#endif
