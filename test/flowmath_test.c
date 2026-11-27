/* Tests for sc-flowmath.c */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc-flowmath.h"

static int fails;
#define CHECK(c, ...)                                     \
	do {                                              \
		printf((c) ? "  ok:   " : "  FAIL: "); \
		printf(__VA_ARGS__);                      \
		printf("\n");                             \
		if (!(c))                                 \
			fails++;                          \
	} while (0)

static int cmpf(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

static void put(uint8_t *raw, size_t pitch, uint32_t x, uint32_t y, int16_t vx, int16_t vy)
{
	memcpy(raw + y * pitch + x * 4, &vx, 2);
	memcpy(raw + y * pitch + x * 4 + 2, &vy, 2);
}

int main(void)
{
	srand(7);

	/* median via quickselect vs sorting, many sizes incl. duplicates */
	int med_bad = 0;
	for (int trial = 0; trial < 3000; trial++) {
		uint32_t ow = 1 + (uint32_t)(rand() % 80), oh = 1 + (uint32_t)(rand() % 60);
		size_t pitch = ow * 4 + (size_t)(rand() % 3) * 4;
		uint8_t *raw = calloc(pitch * oh, 1);
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++)
				put(raw, pitch, x, y, (int16_t)((rand() % 41) - 20) * (trial % 3 ? 1 : 16),
				    (int16_t)((rand() % 7) - 3));
		float *scratch = malloc(sizeof(float) * ow * oh);
		struct sfm_motion m = sfm_global_motion(raw, pitch, ow, oh, 1.0f, 1.0, scratch);
		/* expected: same center region, sort, take n/2 */
		uint32_t x0 = (uint32_t)(ow * 0.2), x1 = (uint32_t)(ow * 0.8), y0 = (uint32_t)(oh * 0.2),
			 y1 = (uint32_t)(oh * 0.8);
		if (x1 <= x0)
			x1 = x0 + 1;
		if (x1 > ow)
			x1 = ow;
		if (y1 <= y0)
			y1 = y0 + 1;
		if (y1 > oh)
			y1 = oh;
		size_t n = 0;
		float *e = malloc(sizeof(float) * ow * oh);
		for (uint32_t y = y0; y < y1; y++)
			for (uint32_t x = x0; x < x1; x++) {
				int16_t v;
				memcpy(&v, raw + y * pitch + x * 4, 2);
				e[n++] = v / 32.0f;
			}
		qsort(e, n, sizeof(float), cmpf);
		if (fabs(m.x - e[n / 2]) > 1e-6)
			med_bad++;
		free(e);
		free(scratch);
		free(raw);
	}
	CHECK(med_bad == 0, "median matches sorted median in 3000 random grids (%d wrong)", med_bad);

	/* global motion + confidence on a clean pan with a static HUD at the edges */
	{
		uint32_t ow = 320, oh = 180;
		size_t pitch = ow * 4;
		uint8_t *raw = calloc(pitch * oh, 1);
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++) {
				bool hud = x < 40 || y < 20 || x > 300;
				put(raw, pitch, x, y, hud ? 0 : (int16_t)(12.5 * 32), hud ? 0 : (int16_t)(-2 * 32));
			}
		float *scratch = malloc(sizeof(float) * ow * oh);
		struct sfm_motion m = sfm_global_motion(raw, pitch, ow, oh, 1.0f, 1.0, scratch);
		CHECK(fabs(m.x - 12.5) < 1e-6 && fabs(m.y + 2.0) < 1e-6, "pan motion (12.5, -2) measured (%.3f, %.3f)",
		      m.x, m.y);
		CHECK(m.confidence > 0.99, "confidence %.3f with HUD outside the center region", m.confidence);
		struct sfm_motion mn = sfm_global_motion(raw, pitch, ow, oh, -1.0f, 1.0, scratch);
		CHECK(fabs(mn.x + 12.5) < 1e-6, "sign -1 flips motion (%.3f)", mn.x);

		float *uv = malloc(sizeof(float) * ow * oh * 2);
		sfm_to_uv(raw, pitch, ow, oh, 1.0f, 1280, 720, uv);
		size_t c = (90 * ow + 160) * 2;
		CHECK(fabsf(uv[c] - 12.5f / 1280.0f) < 1e-9f && fabsf(uv[c + 1] + 2.0f / 720.0f) < 1e-9f,
		      "uv conversion (%.7f, %.7f)", uv[c], uv[c + 1]);
		free(uv);
		free(scratch);
		free(raw);
	}

	/* sign calibration: cur is prev shifted; both NVIDIA conventions */
	{
		uint32_t w = 640, h = 360;
		uint8_t *prev = malloc(w * h), *cur = malloc(w * h);
		for (uint32_t i = 0; i < w * h; i++)
			prev[i] = (uint8_t)(rand() & 255);
		/* blur a little so it's image-like */
		for (int pass = 0; pass < 2; pass++)
			for (uint32_t y = 0; y < h; y++)
				for (uint32_t x = 1; x + 1 < w; x++)
					prev[y * w + x] = (uint8_t)((prev[y * w + x - 1] + 2 * prev[y * w + x] +
								     prev[y * w + x + 1]) /
								    4);
		/* camera pans: content moves left by 9 px -> cur(p) = prev(p + 9) */
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				cur[y * w + x] = prev[y * w + (x + 9 < w ? x + 9 : w - 1)];
		CHECK(sfm_sign_vote(prev, cur, w, w, h, 9.0, 0.0) == 1,
		      "vote +1 when cur(p) = prev(p + m)  [NVIDIA returns +9]");
		CHECK(sfm_sign_vote(prev, cur, w, w, h, -9.0, 0.0) == -1,
		      "vote -1 when cur(p) = prev(p - m)  [NVIDIA returns -9]");
		CHECK(sfm_sign_vote(prev, cur, w, w, h, 1.0, 0.5) == 0, "no vote when motion is tiny");
		CHECK(sfm_mean_absdiff(prev, prev, w, w, h) == 0.0, "absdiff of identical frames is 0");
		CHECK(sfm_mean_absdiff(prev, cur, w, w, h) > 5.0, "absdiff of shifted frames is large");
		free(prev);
		free(cur);
	}

	/* calibration vote accumulation */
	{
		struct sfm_calib c = {0};
		for (int i = 0; i < 4; i++)
			sfm_calib_add(&c, 1);
		CHECK(c.sign == 0, "undecided after 4 votes");
		sfm_calib_add(&c, 1);
		CHECK(c.sign == 1, "decided +1 after 5 agreeing votes");
		struct sfm_calib d = {0};
		for (int i = 0; i < 6; i++)
			sfm_calib_add(&d, i % 2 ? 1 : -1);
		CHECK(d.sign == 0, "stays undecided on mixed votes");
	}

	/* exact repeat detection on the gray image */
	{
		uint32_t w = 1280, h = 720;
		uint8_t *a = malloc(w * h), *b = malloc(w * h);
		for (uint32_t i = 0; i < w * h; i++)
			a[i] = b[i] = (uint8_t)(rand() & 255);
		CHECK(sfm_count_changed(a, b, w, w, h, 2) == 0, "identical frames: 0 changed pixels (repeat)");
		for (uint32_t y = 340; y < 364; y++)
			for (uint32_t x = 620; x < 644; x++)
				b[y * w + x] = (uint8_t)(a[y * w + x] + 40);
		size_t n = sfm_count_changed(a, b, w, w, h, 2);
		CHECK(n == 576, "24x24 px of motion (mouth/hand) is seen: %zu changed pixels", n);
		b[0] = (uint8_t)(a[0] + 1);
		CHECK(sfm_count_changed(a, b, w, w, h, 2) == 576, "1-level rounding change is ignored");
		free(a);
		free(b);
	}

	/* forward/backward consistency + scene cut detection */
	{
		uint32_t ow = 320, oh = 180;
		size_t pitch = ow * 4;
		uint8_t *fwd = calloc(pitch * oh, 1), *bwd = calloc(pitch * oh, 1);
		/* parallax: top half moves 24 px, bottom half 8 px; backward is the exact reverse */
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++) {
				int16_t v = (int16_t)((y < oh / 2 ? 24 : 8) * 32);
				put(fwd, pitch, x, y, v, 0);
				put(bwd, pitch, x, y, (int16_t)-v, 0);
			}
		double cons = sfm_fb_consistency(fwd, bwd, pitch, ow, oh, 1.0f, 4.0);
		CHECK(cons > 0.95, "parallax motion is consistent (%.2f)", cons);
		double consn = sfm_fb_consistency(fwd, bwd, pitch, ow, oh, -1.0f, 4.0);
		CHECK(consn > 0.9, "still consistent with the other sign convention (%.2f)", consn);
		/* scene cut: vectors are unrelated noise */
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++) {
				put(fwd, pitch, x, y, (int16_t)((rand() % 200 - 100) * 32), (int16_t)((rand() % 100 - 50) * 32));
				put(bwd, pitch, x, y, (int16_t)((rand() % 200 - 100) * 32), (int16_t)((rand() % 100 - 50) * 32));
			}
		double consc = sfm_fb_consistency(fwd, bwd, pitch, ow, oh, 1.0f, 4.0);
		CHECK(consc < 0.1, "scene cut is inconsistent (%.2f)", consc);
		free(fwd);
		free(bwd);

		struct sfm_cut c = {0};
		bool any = false;
		for (int i = 0; i < 10; i++)
			any |= sfm_cut_check(&c, 0.97);
		CHECK(!any, "no cut during normal motion");
		CHECK(sfm_cut_check(&c, 0.11), "cut flagged when consistency collapses");
		CHECK(!sfm_cut_check(&c, 0.98), "back to normal right after the cut");
		struct sfm_cut low = {0};
		for (int i = 0; i < 10; i++)
			sfm_cut_check(&low, 0.3);
		CHECK(!sfm_cut_check(&low, 0.1), "no cut when consistency is always low (hard-to-match content)");
	}

	printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
