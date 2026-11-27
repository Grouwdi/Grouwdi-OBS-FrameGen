/*
 * Grouwdi-OBS-FrameGen - retiming engine. See sc-timing.h.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "sc-timing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline struct sct_frame *frame_at(struct sct_engine *e, int i)
{
	return &e->frames[(e->first + i) % SCT_FRAMES];
}

void sct_init(struct sct_engine *e, int lookahead)
{
	memset(e, 0, sizeof(*e));
	if (lookahead < 1)
		lookahead = 1;
	if (lookahead > SCT_MAX_LOOKAHEAD)
		lookahead = SCT_MAX_LOOKAHEAD;
	e->lookahead = lookahead;
	sct_reset(e);
}

void sct_reset(struct sct_engine *e)
{
	e->first = 0;
	e->count = 0;
	for (int i = 0; i < SCT_TICKS; i++) {
		e->tick_of[i] = INT64_MIN;
		e->visible[i] = 0;
	}
	e->dirx = e->diry = 0.0;
	e->break_tick = INT64_MIN / 2;
	e->pan_break_tick = INT64_MIN / 2;
	e->seq = 0;
	e->have_last_target = false;
	e->last_target = 0.0;
	e->have_last_cad = false;
	e->last_cad = 0.0;
}

static void set_visible(struct sct_engine *e, int64_t tick, int64_t id)
{
	int s = (int)(((tick % SCT_TICKS) + SCT_TICKS) % SCT_TICKS);
	e->tick_of[s] = tick;
	e->visible[s] = id;
}

static bool get_visible(const struct sct_engine *e, int64_t tick, int64_t *id)
{
	int s = (int)(((tick % SCT_TICKS) + SCT_TICKS) % SCT_TICKS);
	if (e->tick_of[s] != tick)
		return false;
	*id = e->visible[s];
	return true;
}

static void append_frame(struct sct_engine *e, int64_t tick, double pos, int64_t seq)
{
	if (e->count == SCT_FRAMES) {
		e->first = (e->first + 1) % SCT_FRAMES;
		e->count--;
	}
	struct sct_frame *f = frame_at(e, e->count);
	f->tick = tick;
	f->pos = pos;
	f->seq = seq;
	e->count++;
}

void sct_push(struct sct_engine *e, int64_t tick, bool new_content, double gx, double gy, bool confident, bool cut)
{
	if (!new_content && e->count) {
		set_visible(e, tick, frame_at(e, e->count - 1)->tick);
		return;
	}
	e->seq++;
	if (!e->count) {
		append_frame(e, tick, 0.0, e->seq);
		set_visible(e, tick, tick);
		return;
	}
	if (cut) {
		e->break_tick = tick;
		e->pan_break_tick = tick;
		e->have_last_target = false;
		e->have_last_cad = false;
	}
	if (!confident) {
		/* not a uniform pan: pan mode stands down, timing mode keeps working */
		e->pan_break_tick = tick;
		e->have_last_target = false;
		gx = gy = 0.0;
	}
	double mag = hypot(gx, gy);
	if (mag > 1e-6) {
		if (e->dirx * gx + e->diry * gy < -0.5 * mag * hypot(e->dirx, e->diry)) {
			/* direction reversed: start a new pan segment */
			e->pan_break_tick = tick;
			e->have_last_target = false;
			e->dirx = gx;
			e->diry = gy;
		} else {
			e->dirx = 0.8 * e->dirx + 0.2 * gx;
			e->diry = 0.8 * e->diry + 0.2 * gy;
		}
	}
	double dn = hypot(e->dirx, e->diry);
	double step = dn > 1e-9 ? (gx * e->dirx + gy * e->diry) / dn : 0.0;
	double pos = frame_at(e, e->count - 1)->pos + step;
	append_frame(e, tick, pos, e->seq);
	set_visible(e, tick, tick);

	/* forget old history */
	int64_t cutoff = tick - SCT_HISTORY - e->lookahead - 4;
	while (e->count && frame_at(e, 0)->tick < cutoff) {
		e->first = (e->first + 1) % SCT_FRAMES;
		e->count--;
	}
}

/* Window = frames with lo <= tick <= t, as index range [*i0, *i1). */
static void window(struct sct_engine *e, int64_t lo, int64_t t, int *i0, int *i1)
{
	int a = 0, b;
	while (a < e->count && frame_at(e, a)->tick < lo)
		a++;
	b = a;
	while (b < e->count && frame_at(e, b)->tick <= t)
		b++;
	*i0 = a;
	*i1 = b;
}

static bool fit(struct sct_engine *e, int i0, int i1, int64_t k, double *at_k)
{
	int n = i1 - i0;
	if (n < SCT_MIN_FRAMES)
		return false;
	double sw = 0, swt = 0, swp = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		sw += w;
		swt += w * ts;
		swp += w * f->pos;
	}
	double mt = swt / sw, mp = swp / sw, var = 0, cov = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		var += w * (ts - mt) * (ts - mt);
		cov += w * (ts - mt) * (f->pos - mp);
	}
	if (var <= 0)
		return false;
	double b = cov / var;
	double a = mp - b * mt;
	if (b < SCT_MIN_SPEED)
		return false;
	double r2 = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		double r = f->pos - (a + b * ts);
		r2 += w * r * r;
	}
	if (sqrt(r2 / sw) > SCT_MAX_RESID * b)
		return false;
	*at_k = a;
	return true;
}

static bool bracket(struct sct_engine *e, int i0, int i1, double target, int64_t k, struct sct_decision *d)
{
	for (int i = i0; i + 1 < i1; i++) {
		struct sct_frame *f0 = frame_at(e, i), *f1 = frame_at(e, i + 1);
		if (f0->pos <= target && target <= f1->pos && f1->pos > f0->pos) {
			if (llabs(f0->tick - k) > SCT_MAX_BRACKET_DIST || llabs(f1->tick - k) > SCT_MAX_BRACKET_DIST)
				return false;
			double alpha = (target - f0->pos) / (f1->pos - f0->pos);
			if (alpha < 0.03) {
				d->kind = SCT_PASS;
				d->a = f0->tick;
			} else if (alpha > 0.97) {
				d->kind = SCT_PASS;
				d->a = f1->tick;
			} else {
				d->kind = SCT_INTERP;
				d->a = f0->tick;
				d->b = f1->tick;
				d->alpha = alpha;
			}
			d->mode = SCT_MODE_PAN;
			return true;
		}
	}
	return false;
}

/* Timing-based retiming for any motion (see reference/ref_smoother.py _cadence). */
static bool cadence(struct sct_engine *e, int i0, int i1, int64_t k, struct sct_decision *d)
{
	int n = i1 - i0;
	if (n < SCT_MIN_FRAMES)
		return false;
	for (int i = i0; i + 1 < i1; i++)
		if (frame_at(e, i + 1)->seq - frame_at(e, i)->seq != 1)
			return false; /* a frame is missing from the window */

	double sw = 0, sws = 0, swt = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		sw += w;
		sws += w * (double)f->seq;
		swt += w * ts;
	}
	double ms = sws / sw, mt = swt / sw, var = 0, cov = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		double ds = (double)f->seq - ms;
		var += w * ds * ds;
		cov += w * ds * (ts - mt);
	}
	if (var <= 0)
		return false;
	double tpf = cov / var; /* ticks per frame */
	if (tpf < SCT_CAD_MIN_TPF || tpf > SCT_CAD_MAX_TPF)
		return false;
	double c = mt - tpf * ms;
	double r2 = 0;
	for (int i = i0; i < i1; i++) {
		struct sct_frame *f = frame_at(e, i);
		double ts = (double)(f->tick - k);
		double w = fmax(1.0, (double)(SCT_HISTORY + 1) - fabs(ts));
		double r = ts - (c + tpf * (double)f->seq);
		r2 += w * r * r;
	}
	if (sqrt(r2 / sw) > SCT_CAD_MAX_RESID)
		return false;

	double u = (0.0 - c) / tpf; /* fractional frame number that belongs at tick k */
	if (e->have_last_cad && u < e->last_cad)
		u = e->last_cad;
	for (int i = i0; i + 1 < i1; i++) {
		struct sct_frame *f0 = frame_at(e, i), *f1 = frame_at(e, i + 1);
		if ((double)f0->seq <= u && u <= (double)f1->seq) {
			if (llabs(f0->tick - k) > SCT_MAX_BRACKET_DIST || llabs(f1->tick - k) > SCT_MAX_BRACKET_DIST)
				return false;
			e->have_last_cad = true;
			e->last_cad = u;
			double alpha = u - (double)f0->seq;
			if (alpha < 0.03) {
				d->kind = SCT_PASS;
				d->a = f0->tick;
			} else if (alpha > 0.97) {
				d->kind = SCT_PASS;
				d->a = f1->tick;
			} else {
				d->kind = SCT_INTERP;
				d->a = f0->tick;
				d->b = f1->tick;
				d->alpha = alpha;
			}
			d->mode = SCT_MODE_CADENCE;
			return true;
		}
	}
	return false;
}

static struct sct_decision repeat_fill(struct sct_engine *e, int64_t k, int64_t visible)
{
	struct sct_decision d = {SCT_PASS, visible, 0, 0.0, SCT_MODE_ASIS};
	if (visible == k)
		return d;
	for (int i = 0; i < e->count; i++) {
		if (frame_at(e, i)->tick != visible)
			continue;
		if (i + 1 < e->count) {
			struct sct_frame *a = frame_at(e, i), *b = frame_at(e, i + 1);
			if (b->tick - a->tick <= 3) {
				d.kind = SCT_INTERP;
				d.a = a->tick;
				d.b = b->tick;
				d.alpha = (double)(k - a->tick) / (double)(b->tick - a->tick);
				d.mode = SCT_MODE_FILL;
			}
		}
		break;
	}
	return d;
}

struct sct_decision sct_decide(struct sct_engine *e, int64_t k, int64_t t)
{
	struct sct_decision d = {SCT_NONE, 0, 0, 0.0, SCT_MODE_NONE};
	int64_t visible;
	if (!get_visible(e, k, &visible))
		return d;
	if (e->fill_only)
		return repeat_fill(e, k, visible);

	int64_t lo = k - SCT_HISTORY;
	if (e->break_tick > lo)
		lo = e->break_tick;
	int i0, i1;
	window(e, lo, t, &i0, &i1);
	int64_t lo_pan = lo > e->pan_break_tick ? lo : e->pan_break_tick;
	int p0, p1;
	window(e, lo_pan, t, &p0, &p1);

	double target;
	if (fit(e, p0, p1, k, &target)) {
		if (e->have_last_target && target < e->last_target)
			target = e->last_target;
		if (bracket(e, p0, p1, target, k, &d)) {
			e->have_last_target = true;
			e->last_target = target;
			return d;
		}
	}
	e->have_last_target = false;
	if (cadence(e, i0, i1, k, &d))
		return d;
	e->have_last_cad = false;
	return repeat_fill(e, k, visible);
}
