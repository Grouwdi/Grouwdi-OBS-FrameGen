/*
 * Grouwdi-OBS-FrameGen - retiming engine (no OBS or GPU dependencies).
 *
 * Fed once per OBS render tick with: was this a new frame, and how far did the
 * camera move since the previous new frame. For output tick k (= newest - lookahead)
 * it decides which frame to show, or which two frames to interpolate and by how
 * much, so that motion advances evenly. Three rules, in order:
 *   pan:     uniform camera motion -> even out the motion itself
 *   cadence: frames arrive slower than one per tick -> even out their timing
 *   fill:    a single repeated frame between two new ones -> fill it in
 *
 * Validated against reference/ref_smoother.py (same constants, same decisions).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCT_MAX_LOOKAHEAD 6
#define SCT_HISTORY 12
#define SCT_MIN_SPEED 0.6 /* px per tick at flow resolution */
#define SCT_MAX_RESID 0.45
#define SCT_MAX_BRACKET_DIST 3
#define SCT_MIN_FRAMES 5
#define SCT_CAD_MIN_TPF 1.0   /* cadence mode: frames arrive no faster than 1 per tick... */
#define SCT_CAD_MAX_TPF 3.0    /* ...but at least 1 per 3 ticks */
#define SCT_CAD_MAX_RESID 1.2  /* ticks; tuned on a real trace (bursty frame delivery) */

#define SCT_FRAMES 64 /* ring of distinct frames */
#define SCT_TICKS 64  /* ring of tick -> visible frame */

struct sct_frame {
	int64_t tick; /* tick when it first appeared; also its id */
	double pos;   /* content position along the pan direction */
	int64_t seq;  /* running count of new frames */
};

struct sct_engine {
	int lookahead;
	bool fill_only; /* only replace repeated frames; never retime real ones */

	struct sct_frame frames[SCT_FRAMES];
	int first, count; /* ring: frames[(first + i) % SCT_FRAMES] */

	int64_t tick_of[SCT_TICKS]; /* which tick each slot describes */
	int64_t visible[SCT_TICKS]; /* id of frame visible at that tick */

	double dirx, diry;
	int64_t break_tick;     /* scene cut: nothing before it is used */
	int64_t pan_break_tick; /* pan mode only: reversal or non-uniform motion */
	int64_t seq;
	bool have_last_target;
	double last_target;
	bool have_last_cad;
	double last_cad;
};

enum sct_kind { SCT_NONE = 0, SCT_PASS, SCT_INTERP };

/* which rule produced a decision (for diagnostics) */
enum sct_mode { SCT_MODE_NONE = 0, SCT_MODE_PAN, SCT_MODE_CADENCE, SCT_MODE_FILL, SCT_MODE_ASIS };

struct sct_decision {
	enum sct_kind kind;
	int64_t a;    /* frame id (tick) to show, or first frame of the pair */
	int64_t b;    /* second frame of the pair (INTERP only) */
	double alpha; /* 0..1 position between a and b (INTERP only) */
	enum sct_mode mode;
};

void sct_init(struct sct_engine *e, int lookahead);
void sct_reset(struct sct_engine *e);

/* Register tick. If new_content, (gx, gy) is the camera motion from the previous
 * new frame (engine units), confident says it is one uniform camera move (pan
 * mode may use it), and cut says the picture changed completely (scene cut). */
void sct_push(struct sct_engine *e, int64_t tick, bool new_content, double gx, double gy, bool confident, bool cut);

/* Decide the output for tick k given t = newest pushed tick. */
struct sct_decision sct_decide(struct sct_engine *e, int64_t k, int64_t t);

#ifdef __cplusplus
}
#endif
