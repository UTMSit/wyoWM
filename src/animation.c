#include "animation.h"
#include <math.h>
#include <time.h>
#include <stdio.h>

double clamp01(double v) {
	if (v < 0.0) return 0.0;
	if (v > 1.0) return 1.0;
	return v;
}

static double cubic_bezier(double t, double p1, double p2) {
	double u = 1.0 - t;
	return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
}

static double solve_bezier_x(double x, double p1x, double p2x) {
	double t = x;
	for (int i = 0; i < 8; i++) {
		double cx = cubic_bezier(t, p1x, p2x) - x;
		if (fabs(cx) < 1e-6) break;
		double dx = 3.0 * (1.0 - t) * (1.0 - t) * p1x + 6.0 * (1.0 - t) * t * (p2x - p1x) + 3.0 * t * t * (1.0 - p2x);
		if (fabs(dx) < 1e-6) break;
		t -= cx / dx;
	}
	return t;
}

static double easing_value(EasingCurve curve, double t, const BezierCurve *b) {
	switch (curve) {
		case EASING_LINEAR: return t;
		case EASING_EASE_IN: return t * t * t;
		case EASING_EASE_OUT: return 1.0 - pow(1.0 - t, 3.0);
		case EASING_EASE_IN_OUT:
			if (t < 0.5) return 4.0 * t * t * t;
			return 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
		case EASING_BEZIER:
			if (!b) return t;
			double st = solve_bezier_x(t, b->p1x, b->p2x);
			return cubic_bezier(st, b->p1y, b->p2y);
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
	value->bezier.p1x = 0.0; value->bezier.p1y = 0.0;
	value->bezier.p2x = 1.0; value->bezier.p2y = 1.0;
	value->active = false;
}

void animation_set_target(AnimatedValue *value, double target, int duration_ms, EasingCurve curve, const BezierCurve *bezier) {
	if (duration_ms <= 0 || value->current == target) {
		value->start = target;
		value->current = target;
		value->target = target;
		value->velocity = 0.0;
		value->duration_ms = 0;
		value->start_time = 0;
		value->curve = curve;
		if (bezier) value->bezier = *bezier;
		value->active = false;
		return;
	}
	value->start = value->current;
	value->target = target;
	value->duration_ms = duration_ms;
	value->start_time = animation_now_ms();
	value->curve = curve;
	if (bezier) value->bezier = *bezier;
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
	double progress = easing_value(value->curve, clamp01(t), &value->bezier);
	value->current = value->start + (value->target - value->start) * progress;
	value->velocity = value->current - previous;
}

bool animation_finished(AnimatedValue *value) {
	return !value->active;
}
