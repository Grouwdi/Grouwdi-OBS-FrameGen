/*
 * Grouwdi-OBS-FrameGen - CPU-side math on optical flow results. See sc-flowmath.h.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "sc-flowmath.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline int16_t rd16(const uint8_t *p)
{
	int16_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

void sfm_to_uv(const uint8_t *raw, size_t pitch, uint32_t ow, uint32_t oh, float sign, uint32_t flow_w, uint32_t flow_h,
	       float *out)
{
	const float sx = sign * SFM_S105_SCALE / (float)flow_w;
	const float sy = sign * SFM_S105_SCALE / (float)flow_h;
	for (uint32_t y = 0; y < oh; y++) {
		const uint8_t *row = raw + (size_t)y * pitch;
		float *o = out + (size_t)y * ow * 2;
		for (uint32_t x = 0; x < ow; x++) {
			o[x * 2 + 0] = (float)rd16(row + x * 4 + 0) * sx;
			o[x * 2 + 1] = (float)rd16(row + x * 4 + 2) * sy;
		}
	}
}

/* Hoare quickselect: k-th smallest of v[0..n-1] (reorders v). */
static float select_k(float *v, size_t n, size_t k)
{
	size_t lo = 0, hi = n - 1;
	while (lo < hi) {
		float pivot = v[(lo + hi) / 2];
		size_t i = lo, j = hi;
		while (i <= j) {
			while (v[i] < pivot)
				i++;
			while (v[j] > pivot)
				j--;
			if (i <= j) {
				float t = v[i];
				v[i] = v[j];
				v[j] = t;
				i++;
				if (j == 0)
					break;
				j--;
			}
		}
		if (k <= j)
			hi = j;
		else if (k >= i)
			lo = i;
		else
			return v[k];
	}
	return v[k];
}

static void center(uint32_t n, uint32_t *a, uint32_t *b)
{
	*a = (uint32_t)(n * 0.2);
	*b = (uint32_t)(n * 0.8);
	if (*b <= *a)
		*b = *a + 1;
	if (*b > n)
		*b = n;
}

struct sfm_motion sfm_global_motion(const uint8_t *raw, size_t pitch, uint32_t ow, uint32_t oh, float sign,
				    double tol_px, float *scratch)
{
	struct sfm_motion m = {0, 0, 0};
	uint32_t x0, x1, y0, y1;
	size_t n = 0;
	center(ow, &x0, &x1);
	center(oh, &y0, &y1);
	if (!ow || !oh)
		return m;

	/* median of x */
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			scratch[n++] = (float)rd16(raw + (size_t)y * pitch + x * 4) * SFM_S105_SCALE * sign;
	m.x = select_k(scratch, n, n / 2);
	/* median of y */
	n = 0;
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			scratch[n++] = (float)rd16(raw + (size_t)y * pitch + x * 4 + 2) * SFM_S105_SCALE * sign;
	m.y = select_k(scratch, n, n / 2);

	size_t agree = 0;
	for (uint32_t y = y0; y < y1; y++) {
		for (uint32_t x = x0; x < x1; x++) {
			const uint8_t *p = raw + (size_t)y * pitch + x * 4;
			double vx = rd16(p) * SFM_S105_SCALE * sign - m.x;
			double vy = rd16(p + 2) * SFM_S105_SCALE * sign - m.y;
			if (vx * vx + vy * vy < tol_px * tol_px)
				agree++;
		}
	}
	m.confidence = (double)agree / (double)n;
	return m;
}

static double sad_shift(const uint8_t *prev, const uint8_t *cur, size_t pitch, uint32_t w, uint32_t h, int dx, int dy)
{
	uint32_t x0, x1, y0, y1;
	uint64_t sum = 0, cnt = 0;
	center(w, &x0, &x1);
	center(h, &y0, &y1);
	for (uint32_t y = y0; y < y1; y += 2) {
		int py = (int)y + dy;
		if (py < 0 || py >= (int)h)
			continue;
		for (uint32_t x = x0; x < x1; x += 2) {
			int px = (int)x + dx;
			if (px < 0 || px >= (int)w)
				continue;
			int d = (int)cur[(size_t)y * pitch + x] - (int)prev[(size_t)py * pitch + (size_t)px];
			sum += (uint64_t)(d < 0 ? -d : d);
			cnt++;
		}
	}
	return cnt ? (double)sum / (double)cnt : 1e9;
}

int sfm_sign_vote(const uint8_t *prev, const uint8_t *cur, size_t pitch, uint32_t w, uint32_t h, double mx, double my)
{
	if (mx * mx + my * my < 3.0 * 3.0)
		return 0; /* too little motion to tell the directions apart */
	int dx = (int)lround(mx), dy = (int)lround(my);
	double plus = sad_shift(prev, cur, pitch, w, h, dx, dy);
	double minus = sad_shift(prev, cur, pitch, w, h, -dx, -dy);
	if (plus < 0.8 * minus)
		return 1;
	if (minus < 0.8 * plus)
		return -1;
	return 0;
}

size_t sfm_count_changed(const uint8_t *a, const uint8_t *b, size_t pitch, uint32_t w, uint32_t h, int min_delta)
{
	size_t n = 0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *ra = a + (size_t)y * pitch, *rb = b + (size_t)y * pitch;
		for (uint32_t x = 0; x < w; x++) {
			int d = (int)ra[x] - (int)rb[x];
			if (d >= min_delta || -d >= min_delta)
				n++;
		}
	}
	return n;
}

double sfm_mean_absdiff(const uint8_t *a, const uint8_t *b, size_t pitch, uint32_t w, uint32_t h)
{
	uint64_t sum = 0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *ra = a + (size_t)y * pitch, *rb = b + (size_t)y * pitch;
		for (uint32_t x = 0; x < w; x++) {
			int d = (int)ra[x] - (int)rb[x];
			sum += (uint64_t)(d < 0 ? -d : d);
		}
	}
	return (w && h) ? (double)sum / ((double)w * (double)h) : 0.0;
}

void sfm_calib_add(struct sfm_calib *c, int vote)
{
	if (vote > 0)
		c->plus++;
	else if (vote < 0)
		c->minus++;
	/* decide after 5 votes with a clear (4:1 or better) majority */
	if (c->plus + c->minus >= 5) {
		if (c->plus >= 4 * c->minus)
			c->sign = 1;
		else if (c->minus >= 4 * c->plus)
			c->sign = -1;
	}
}

double sfm_fb_consistency(const uint8_t *fwd, const uint8_t *bwd, size_t pitch, uint32_t ow, uint32_t oh, float sign,
			  double tol_px)
{
	uint32_t x0, x1, y0, y1;
	size_t n = 0, ok = 0;
	center(ow, &x0, &x1);
	center(oh, &y0, &y1);
	for (uint32_t y = y0; y < y1; y++) {
		for (uint32_t x = x0; x < x1; x++) {
			const uint8_t *p = fwd + (size_t)y * pitch + (size_t)x * 4;
			double fx = rd16(p) * SFM_S105_SCALE * sign, fy = rd16(p + 2) * SFM_S105_SCALE * sign;
			/* grid cells are 4x4 pixels */
			long tx = lround((double)x + fx / 4.0), ty = lround((double)y + fy / 4.0);
			if (tx < 0)
				tx = 0;
			if (ty < 0)
				ty = 0;
			if (tx >= (long)ow)
				tx = (long)ow - 1;
			if (ty >= (long)oh)
				ty = (long)oh - 1;
			const uint8_t *q = bwd + (size_t)ty * pitch + (size_t)tx * 4;
			double ex = fx + rd16(q) * SFM_S105_SCALE * sign, ey = fy + rd16(q + 2) * SFM_S105_SCALE * sign;
			if (ex * ex + ey * ey < tol_px * tol_px)
				ok++;
			n++;
		}
	}
	return n ? (double)ok / (double)n : 1.0;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

bool sfm_cut_check(struct sfm_cut *c, double consistency)
{
	bool cut = false;
	if (c->n >= 3) {
		double tmp[SFM_CUT_HIST];
		memcpy(tmp, c->hist, sizeof(double) * (size_t)c->n);
		qsort(tmp, (size_t)c->n, sizeof(double), cmp_d);
		double normal = (c->n % 2) ? tmp[c->n / 2] : 0.5 * (tmp[c->n / 2 - 1] + tmp[c->n / 2]);
		cut = normal >= SFM_CUT_NORMAL_MIN && consistency < SFM_CUT_CONSISTENCY;
	}
	if (!cut) {
		c->hist[c->pos] = consistency;
		c->pos = (c->pos + 1) % SFM_CUT_HIST;
		if (c->n < SFM_CUT_HIST)
			c->n++;
	}
	return cut;
}
