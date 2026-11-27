/*
 * Grouwdi-OBS-FrameGen - core logic with no OBS dependencies (unit-testable).
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

/* Pixel layouts the core understands. Mirrors the 8-bit OBS video formats. */
enum sc_layout {
	SC_UNSUPPORTED = 0,
	SC_PLANAR_420_3,  /* I420: Y, U, V planes, chroma half height */
	SC_PLANAR_420_2,  /* NV12: Y plane, interleaved UV plane half height */
	SC_PLANAR_FULL_3, /* I422 / I444: three full-height planes */
	SC_PLANAR_1,      /* Y800: single luma plane */
	SC_PACKED_Y0,     /* YUY2 / YVYU: luma at byte 2x */
	SC_PACKED_Y1,     /* UYVY: luma at byte 2x+1 */
	SC_PACKED_RGB4,   /* RGBA / BGRA / BGRX: use green, byte 4x+1 */
	SC_PACKED_RGB3,   /* BGR3: use green, byte 3x+1 */
};

/* Repeat rule, identical to Motion Smoother's: on a half-size luma image, a frame is
 * new if at least SC_SAME_MIN_PIXELS pixels changed by at least SC_SAME_MIN_DELTA levels. */
#define SC_SAME_MIN_DELTA 2
#define SC_SAME_MIN_PIXELS 8

/* Fill dst ((width/2) x (height/2), pitch width/2) with the 2x2 average of the frame's
 * luma (green for RGB layouts). Returns false for unsupported layouts or tiny frames. */
bool sc_half_luma(uint8_t *dst, enum sc_layout layout, const uint8_t *plane0, uint32_t linesize0, uint32_t width,
		  uint32_t height);

/* Pixels where |a - b| >= min_delta, counting stops at stop_at (0 = count all). */
size_t sc_count_changed(const uint8_t *a, const uint8_t *b, size_t n, int min_delta, size_t stop_at);

/* True if b shows the same picture as a under the repeat rule above. */
bool sc_same_picture(const uint8_t *a, const uint8_t *b, size_t n);

/* Number of planes and the row count of each plane for a layout. */
int sc_plane_rows(enum sc_layout layout, uint32_t height, uint32_t rows[3]);

/* dst = average(a, b), byte by byte, for every plane. dst may not alias a or b.
 * Valid for all 8-bit layouts above (each byte is one 8-bit component). */
void sc_blend_planes(uint8_t *const dst[3], const uint32_t dst_ls[3], const uint8_t *const a[3], const uint32_t a_ls[3],
		     const uint8_t *const b[3], const uint32_t b_ls[3], enum sc_layout layout, uint32_t height);

/* A run of identical frames longer than this (half a second at 60 fps) is a still
 * picture (menu, pause, loading screen). Shorter runs are repeats or game hitches and
 * are counted, since those are what viewers see as stutter. */
#define SC_MAX_REPEAT_RUN 30

/* ---------------- rhythm of events ---------------- */

#define SC_MAX_EVENTS 512

struct sc_events {
	uint64_t tick[SC_MAX_EVENTS]; /* event positions, in OBS render ticks */
	int count;
};

void sc_events_clear(struct sc_events *e);
void sc_events_add(struct sc_events *e, uint64_t tick);

/*
 * If the events fall on a steady rhythm (at least 5 events and at least 75% of
 * gaps within +/-1 tick of the median gap) return the median gap in ticks,
 * otherwise 0.
 */
uint64_t sc_events_period(const struct sc_events *e);

#ifdef __cplusplus
}
#endif
