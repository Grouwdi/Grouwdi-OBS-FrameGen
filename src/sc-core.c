/*
 * Grouwdi-OBS-FrameGen - core logic with no OBS dependencies (unit-testable).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "sc-core.h"

#include <stdlib.h>
#include <string.h>

static bool luma_access(enum sc_layout layout, uint32_t *stride, uint32_t *offset)
{
	switch (layout) {
	case SC_PLANAR_420_3:
	case SC_PLANAR_420_2:
	case SC_PLANAR_FULL_3:
	case SC_PLANAR_1:
		*stride = 1;
		*offset = 0;
		return true;
	case SC_PACKED_Y0:
		*stride = 2;
		*offset = 0;
		return true;
	case SC_PACKED_Y1:
		*stride = 2;
		*offset = 1;
		return true;
	case SC_PACKED_RGB4:
		*stride = 4;
		*offset = 1;
		return true;
	case SC_PACKED_RGB3:
		*stride = 3;
		*offset = 1;
		return true;
	default:
		return false;
	}
}

bool sc_half_luma(uint8_t *dst, enum sc_layout layout, const uint8_t *plane0, uint32_t linesize0, uint32_t width,
		  uint32_t height)
{
	uint32_t stride, offset;
	if (!plane0 || width < 2 || height < 2 || !luma_access(layout, &stride, &offset))
		return false;
	const uint32_t hw = width / 2, hh = height / 2;
	for (uint32_t y = 0; y < hh; y++) {
		const uint8_t *r0 = plane0 + (size_t)(2 * y) * linesize0 + offset;
		const uint8_t *r1 = r0 + linesize0;
		uint8_t *d = dst + (size_t)y * hw;
		for (uint32_t x = 0; x < hw; x++) {
			size_t i0 = (size_t)(2 * x) * stride, i1 = i0 + stride;
			d[x] = (uint8_t)((r0[i0] + r0[i1] + r1[i0] + r1[i1] + 2u) >> 2);
		}
	}
	return true;
}

size_t sc_count_changed(const uint8_t *a, const uint8_t *b, size_t n, int min_delta, size_t stop_at)
{
	size_t count = 0;
	for (size_t i = 0; i < n; i++) {
		int d = (int)a[i] - (int)b[i];
		if (d >= min_delta || -d >= min_delta) {
			if (++count == stop_at)
				break;
		}
	}
	return count;
}

bool sc_same_picture(const uint8_t *a, const uint8_t *b, size_t n)
{
	return sc_count_changed(a, b, n, SC_SAME_MIN_DELTA, SC_SAME_MIN_PIXELS) < SC_SAME_MIN_PIXELS;
}

int sc_plane_rows(enum sc_layout layout, uint32_t height, uint32_t rows[3])
{
	uint32_t half = (height + 1) / 2;
	switch (layout) {
	case SC_PLANAR_420_3:
		rows[0] = height;
		rows[1] = half;
		rows[2] = half;
		return 3;
	case SC_PLANAR_420_2:
		rows[0] = height;
		rows[1] = half;
		rows[2] = 0;
		return 2;
	case SC_PLANAR_FULL_3:
		rows[0] = rows[1] = rows[2] = height;
		return 3;
	case SC_PLANAR_1:
	case SC_PACKED_Y0:
	case SC_PACKED_Y1:
	case SC_PACKED_RGB4:
	case SC_PACKED_RGB3:
		rows[0] = height;
		rows[1] = rows[2] = 0;
		return 1;
	default:
		return 0;
	}
}

void sc_blend_planes(uint8_t *const dst[3], const uint32_t dst_ls[3], const uint8_t *const a[3], const uint32_t a_ls[3],
		     const uint8_t *const b[3], const uint32_t b_ls[3], enum sc_layout layout, uint32_t height)
{
	uint32_t rows[3];
	int planes = sc_plane_rows(layout, height, rows);

	for (int p = 0; p < planes; p++) {
		uint32_t bytes = dst_ls[p];
		if (a_ls[p] < bytes)
			bytes = a_ls[p];
		if (b_ls[p] < bytes)
			bytes = b_ls[p];

		for (uint32_t y = 0; y < rows[p]; y++) {
			uint8_t *d = dst[p] + (size_t)y * dst_ls[p];
			const uint8_t *pa = a[p] + (size_t)y * a_ls[p];
			const uint8_t *pb = b[p] + (size_t)y * b_ls[p];
			for (uint32_t x = 0; x < bytes; x++)
				d[x] = (uint8_t)(((unsigned)pa[x] + (unsigned)pb[x] + 1u) >> 1);
		}
	}
}

/* ---------------- rhythm of events ---------------- */

void sc_events_clear(struct sc_events *e)
{
	e->count = 0;
}

void sc_events_add(struct sc_events *e, uint64_t tick)
{
	if (e->count < SC_MAX_EVENTS)
		e->tick[e->count++] = tick;
}

static int cmp_u64(const void *x, const void *y)
{
	uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
	return (a > b) - (a < b);
}

uint64_t sc_events_period(const struct sc_events *e)
{
	uint64_t gaps[SC_MAX_EVENTS], sorted[SC_MAX_EVENTS];
	int n = e->count - 1;
	if (e->count < 5)
		return 0;
	for (int i = 0; i < n; i++)
		gaps[i] = e->tick[i + 1] - e->tick[i];
	memcpy(sorted, gaps, sizeof(uint64_t) * (size_t)n);
	qsort(sorted, (size_t)n, sizeof(uint64_t), cmp_u64);
	uint64_t med = sorted[n / 2];
	if (med < 2)
		return 0;
	int close = 0;
	for (int i = 0; i < n; i++) {
		uint64_t g = gaps[i];
		if ((g >= med ? g - med : med - g) <= 1)
			close++;
	}
	return (close * 4 >= n * 3) ? med : 0;
}
