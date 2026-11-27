/*
 * Grouwdi-OBS-FrameGen - CPU-side math on optical flow results (no OBS / GPU deps).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVIDIA flow vectors are S10.5 fixed point: value / 32 = pixels. */
#define SFM_S105_SCALE (1.0f / 32.0f)

/* Width the retiming engine's speed constants are expressed in (see reference). */
#define SFM_ENGINE_WIDTH 320.0

/*
 * Convert a grid of raw NVIDIA flow vectors (int16 x,y pairs, row pitch in
 * bytes) into float texture offsets in UV units (x / flow_w, y / flow_h),
 * multiplied by sign (+1 or -1, from calibration). out holds ow*oh*2 floats.
 */
void sfm_to_uv(const uint8_t *raw, size_t pitch, uint32_t ow, uint32_t oh, float sign, uint32_t flow_w, uint32_t flow_h,
	       float *out);

struct sfm_motion {
	double x, y;       /* median camera motion in flow-resolution pixels */
	double confidence; /* fraction of vectors that agree with the median */
};

/*
 * Median motion over the center 60% of the grid (ignores HUD at the edges),
 * and how many vectors agree with it within tol_px. scratch must hold ow*oh floats.
 */
struct sfm_motion sfm_global_motion(const uint8_t *raw, size_t pitch, uint32_t ow, uint32_t oh, float sign,
				    double tol_px, float *scratch);

/*
 * Flow direction calibration. raw_cur_to_prev = the flow NVIDIA returned with
 * input = cur, reference = prev, and (mx, my) is its median in pixels.
 * Tests whether cur(p) matches prev(p + m) (returns +1) or prev(p - m)
 * (returns -1). Returns 0 if motion is too small or the test is not decisive.
 */
int sfm_sign_vote(const uint8_t *prev, const uint8_t *cur, size_t pitch, uint32_t w, uint32_t h, double mx, double my);

/* Number of pixels that differ by at least min_delta between two 8-bit images. */
size_t sfm_count_changed(const uint8_t *a, const uint8_t *b, size_t pitch, uint32_t w, uint32_t h, int min_delta);

/* Mean absolute difference of two 8-bit images (with row pitch). */
double sfm_mean_absdiff(const uint8_t *a, const uint8_t *b, size_t pitch, uint32_t w, uint32_t h);

/*
 * Fraction of the center region where forward and backward flow agree:
 * f01(p) + f10(p + f01(p)) ~ 0 within tol_px. Any real motion (pans, depth,
 * moving objects) stays consistent; across a scene cut it collapses.
 * fwd/bwd are raw NVIDIA grids (4x4 grid), multiplied by sign.
 */
double sfm_fb_consistency(const uint8_t *fwd, const uint8_t *bwd, size_t pitch, uint32_t ow, uint32_t oh, float sign,
			  double tol_px);

#define SFM_CUT_CONSISTENCY 0.25 /* scene cut below this ... */
#define SFM_CUT_NORMAL_MIN 0.5   /* ... while normally at least this agree */
#define SFM_CUT_HIST 15

struct sfm_cut {
	double hist[SFM_CUT_HIST];
	int n, pos;
};
/* Returns true if this consistency value marks a scene cut. */
bool sfm_cut_check(struct sfm_cut *c, double consistency);

/* Running vote for the flow sign. */
struct sfm_calib {
	int plus, minus;
	int sign; /* 0 = unknown, else +1 / -1 */
};
void sfm_calib_add(struct sfm_calib *c, int vote);

#ifdef __cplusplus
}
#endif
