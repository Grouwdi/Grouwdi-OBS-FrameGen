/*
 * Grouwdi-OBS-FrameGen - Motion Smoother filter (Windows, NVIDIA).
 *
 * Re-times camera motion so it advances evenly, and builds the in-between
 * frames with GPU optical flow. Fixes repeated frames, short steps and long
 * steps (game fps vs. capture rate) and OBS re-shows, before encoding.
 *
 * Per OBS tick (rendered once, reused for every view/canvas):
 *   1. collect last tick's GPU results: repeat check, flow, camera motion
 *   2. capture the source into a history texture, downscale to gray + thumbnail,
 *      queue optical flow on the GPU (results next tick, so nothing waits)
 *   3. retiming engine (sc-timing.c) decides the output for tick k = newest - lookahead
 *   4. draw that frame, or warp two frames together with the flow
 *
 * Render structure follows OBS's own gpu-delay filter (plugins/obs-filters).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#if defined(_WIN32) || defined(SC_TEST_HARNESS)

#include <obs-module.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "plugin-support.h"
#include "sc-flowmath.h"
#include "sc-gpu.h"
#include "sc-timing.h"

#define S_DELAY "delay_frames"
#define S_QUALITY "flow_quality"
#define S_FLOW_W "flow_width"
#define S_STATUS "status"
#define S_GHOST "ghost_vs_smooth"
#define S_TRACE "record_trace"
#define S_RECALIB "recalibrate"
#define CALIB_FILE "calibration.json"
#define TRACE_TICKS 3600 /* 60 s at 60 fps */

#define NSLOTS 16
/* Repeat detection on the full flow-resolution gray image. A true repeat (card or
 * OBS) is pixel-identical, so only a handful of changed pixels is needed to call
 * a frame new: small motion like hands or a mouth changes thousands. */
#define DUP_MIN_DELTA 2
#define DUP_MIN_PIXELS 8
#define STATS_SECONDS 10

struct slot {
	gs_texrender_t *tr;
	gs_texture_t *f01, *f10; /* flow from the previous new frame to this one (UV units) */
	int64_t id;              /* tick it first appeared */
	bool used;
	bool has_flow;
};

/* one row of a diagnostic trace: what the engine was given, and what it decided */
struct trace_row {
	int64_t tick;
	bool is_new, conf, cut;
	double gx, gy;
	uint32_t changed;
	struct sct_decision d;
	bool has_d;
};

struct msm {
	obs_source_t *context;

	/* diagnostic trace (button in the filter properties) */
	volatile bool trace_request;
	volatile bool recalib_request;
	bool tracing;
	struct trace_row *trace;
	int trace_n;
	uint32_t last_changed;

	/* settings (UI thread) -> applied on the graphics thread */
	volatile bool reinit;
	int delay_frames;
	int quality;
	bool fill_only; /* Fill missing frames only */
	double snap; /* show a real frame when the in-between point is this close to it */
	int flow_w_setting;

	/* state (graphics thread) */
	bool ready, failed, active, processed;
	uint32_t cx, cy, fw, fh, ow, oh;
	struct sc_gpu *gpu;
	gs_effect_t *effect;
	gs_texture_t *gray, *thumb;
	gs_texrender_t *out;
	struct slot slots[NSLOTS];

	int64_t tick, last_render_tick;
	bool pending;
	int pending_slot;
	int64_t pending_tick;
	uint8_t *prev_gray; /* fw x fh */
	bool have_prev_gray;
	bool have_output;
	bool have_pushed;
	int64_t last_pushed; /* newest tick given to the engine */

	struct sct_engine eng;
	struct sfm_calib calib;
	struct sfm_cut cut;
	float *uv, *scratch;

	/* stats */
	uint64_t stats_start;
	uint32_t n_ticks, n_new, n_repeat, n_interp, n_pass;
	uint32_t n_pan, n_cad, n_fill, n_cuts, n_snapped;
	double wait_sum, wait_max;

	pthread_mutex_t status_mutex;
	struct dstr status;
};

static void set_status(struct msm *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	pthread_mutex_lock(&s->status_mutex);
	dstr_vprintf(&s->status, fmt, ap);
	pthread_mutex_unlock(&s->status_mutex);
	va_end(ap);
	/* refresh the filter's settings window if it is open, so the status is live */
	obs_source_update_properties(s->context);
}

/* ---------------- resources (graphics thread) ---------------- */

static void free_gpu(struct msm *s)
{
	if (s->pending && s->gpu) {
		struct sc_gpu_result r;
		sc_gpu_collect(s->gpu, &r);
	}
	s->pending = false;
	sc_gpu_destroy(s->gpu);
	s->gpu = NULL;
	gs_texture_destroy(s->gray);
	gs_texture_destroy(s->thumb);
	s->gray = s->thumb = NULL;
	gs_texrender_destroy(s->out);
	s->out = NULL;
	for (int i = 0; i < NSLOTS; i++) {
		gs_texrender_destroy(s->slots[i].tr);
		gs_texture_destroy(s->slots[i].f01);
		gs_texture_destroy(s->slots[i].f10);
		memset(&s->slots[i], 0, sizeof(s->slots[i]));
	}
	bfree(s->uv);
	bfree(s->scratch);
	bfree(s->prev_gray);
	s->uv = s->scratch = NULL;
	s->prev_gray = NULL;
	s->ready = false;
}

static void reset_pipeline(struct msm *s)
{
	if (s->pending && s->gpu) {
		struct sc_gpu_result r;
		sc_gpu_collect(s->gpu, &r);
	}
	s->pending = false;
	if (s->gpu)
		sc_gpu_drop_reference(s->gpu);
	for (int i = 0; i < NSLOTS; i++) {
		s->slots[i].used = false;
		s->slots[i].has_flow = false;
	}
	s->have_prev_gray = false;
	s->have_output = false;
	s->have_pushed = false;
	memset(&s->cut, 0, sizeof(s->cut));
	sct_init(&s->eng, s->delay_frames - 1);
	s->eng.fill_only = s->fill_only;
}

/* ---------------- saved calibration ----------------
 * The flow direction sign is a property of NVIDIA's optical flow output on this PC,
 * not of the game, so it is saved once and reused (all filters, all scenes). */

static int load_saved_sign(void)
{
	int sign = 0;
	char *path = obs_module_config_path(CALIB_FILE);
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : NULL;
	if (d) {
		long long v = obs_data_get_int(d, "flow_sign");
		sign = (v == 1 || v == -1) ? (int)v : 0;
		obs_data_release(d);
	}
	bfree(path);
	return sign;
}

static void save_sign(int sign)
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path(CALIB_FILE);
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "flow_sign", sign);
		if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
			obs_log(LOG_WARNING, "could not save calibration to %s", path);
		obs_data_release(d);
	}
	bfree(path);
	bfree(dir);
}

static void gpu_fail_status(struct msm *s, enum sc_gpu_fail why, const char *err)
{
	const char *name = obs_source_get_name(s->context);
	switch (why) {
	case SC_GPU_FAIL_NOT_NVIDIA:
		set_status(s,
			   "Motion Smoother needs an NVIDIA GPU, but OBS is running on %s. The video passes through "
			   "unchanged. Capture Smoother still works on any GPU.",
			   err);
		obs_log(LOG_WARNING, "[%s] needs an NVIDIA GPU; OBS renders on %s. Passing video through.", name,
			err);
		break;
	case SC_GPU_FAIL_NO_OF:
		set_status(s,
			   "Motion Smoother needs an NVIDIA GPU with an optical flow engine (Turing generation or newer, "
			   "for example GeForce RTX 20-series and later) and a current NVIDIA driver. The video passes "
			   "through unchanged. Details: %s",
			   err);
		obs_log(LOG_WARNING, "[%s] NVIDIA optical flow unavailable (%s). Passing video through.", name, err);
		break;
	default:
		set_status(s, "GPU optical flow unavailable: %s. The video passes through unchanged.", err);
		obs_log(LOG_WARNING, "[%s] %s", name, err);
		break;
	}
}

static bool init_gpu(struct msm *s)
{
	char err[256] = {0};

	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
		set_status(s, "Needs OBS's Direct3D 11 renderer (Settings > Advanced > Renderer).");
		return false;
	}
	if (!s->effect) {
		set_status(s, "Shader file motion-smoother.effect failed to load. Reinstall the plugin.");
		return false;
	}

	uint32_t fw = (uint32_t)s->flow_w_setting;
	if (fw > s->cx)
		fw = s->cx;
	fw &= ~1u;
	uint32_t fh = (uint32_t)((double)s->cy * fw / s->cx + 0.5) & ~1u;
	if (fw < 64 || fh < 64) {
		set_status(s, "Source is too small (%ux%u).", s->cx, s->cy);
		return false;
	}
	s->fw = fw;
	s->fh = fh;
	s->ow = (fw + 3) / 4;
	s->oh = (fh + 3) / 4;

	s->gray = gs_texture_create(fw, fh, GS_R8, 1, NULL, GS_RENDER_TARGET);
	s->thumb = gs_texture_create(SC_THUMB_W, SC_THUMB_H, GS_R8, 1, NULL, GS_RENDER_TARGET);
	s->out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!s->gray || !s->thumb || !s->out) {
		set_status(s, "Could not create GPU textures.");
		return false;
	}
	for (int i = 0; i < NSLOTS; i++) {
		s->slots[i].tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		s->slots[i].f01 = gs_texture_create(s->ow, s->oh, GS_RG32F, 1, NULL, GS_DYNAMIC);
		s->slots[i].f10 = gs_texture_create(s->ow, s->oh, GS_RG32F, 1, NULL, GS_DYNAMIC);
		if (!s->slots[i].tr || !s->slots[i].f01 || !s->slots[i].f10) {
			set_status(s, "Could not create GPU textures.");
			return false;
		}
	}
	s->uv = bmalloc(sizeof(float) * s->ow * s->oh * 2);
	s->scratch = bmalloc(sizeof(float) * s->ow * s->oh);
	s->prev_gray = bmalloc((size_t)fw * fh);

	enum sc_gpu_fail why = SC_GPU_FAIL_OTHER;
	s->gpu = sc_gpu_create(gs_get_device_obj(), gs_texture_get_obj(s->gray), gs_texture_get_obj(s->thumb), fw, fh,
			       (enum sc_gpu_quality)s->quality, err, sizeof(err), &why);
	if (!s->gpu) {
		gpu_fail_status(s, why, err);
		return false;
	}

	memset(&s->calib, 0, sizeof(s->calib));
	s->calib.sign = load_saved_sign();
	reset_pipeline(s);
	s->stats_start = os_gettime_ns();
	s->n_ticks = s->n_new = s->n_repeat = s->n_interp = s->n_pass = 0;
	s->n_pan = s->n_cad = s->n_fill = s->n_cuts = s->n_snapped = 0;
	s->wait_sum = s->wait_max = 0;

	obs_log(LOG_INFO, "[%s] GPU optical flow ready: source %ux%u, flow %ux%u, delay %d frames",
		obs_source_get_name(s->context), s->cx, s->cy, fw, fh, s->delay_frames);
	if (s->calib.sign)
		set_status(s, "Running. Using the saved flow direction calibration.");
	else
		set_status(s, "Running. Calibrating flow direction (one time only): pan the camera for a few "
			      "seconds.");
	obs_log(LOG_INFO, "[%s] flow direction: %s", obs_source_get_name(s->context),
		s->calib.sign ? "loaded from saved calibration" : "not calibrated yet, waiting for camera motion");
	return true;
}

/* ---------------- slots ---------------- */

static int find_slot(struct msm *s, int64_t id)
{
	for (int i = 0; i < NSLOTS; i++)
		if (s->slots[i].used && s->slots[i].id == id)
			return i;
	return -1;
}

static int alloc_slot(struct msm *s)
{
	for (int i = 0; i < NSLOTS; i++)
		if (!s->slots[i].used && !(s->pending && s->pending_slot == i))
			return i;
	return -1;
}

static void free_old_slots(struct msm *s, int64_t k)
{
	int64_t keep = INT64_MIN;
	for (int i = 0; i < NSLOTS; i++)
		if (s->slots[i].used && s->slots[i].id <= k && s->slots[i].id > keep)
			keep = s->slots[i].id;
	for (int i = 0; i < NSLOTS; i++)
		if (s->slots[i].used && s->slots[i].id < k - (SCT_MAX_BRACKET_DIST + 2) && s->slots[i].id != keep)
			s->slots[i].used = false;
}

/* ---------------- drawing helpers ---------------- */

static void capture_source(struct msm *s, obs_source_t *target, obs_source_t *parent, gs_texrender_t *tr)
{
	gs_texrender_reset(tr);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(tr, s->cx, s->cy, GS_CS_SRGB)) {
		uint32_t flags = obs_source_get_output_flags(target);
		bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)s->cx, 0.0f, (float)s->cy, -100.0f, 100.0f);
		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
		gs_texrender_end(tr);
	}
	gs_blend_state_pop();
}

static void draw_gray(struct msm *s, gs_texture_t *src, gs_texture_t *dst, uint32_t w, uint32_t h)
{
	gs_texture_t *prev_rt = gs_get_render_target();
	gs_zstencil_t *prev_zs = gs_get_zstencil_target();
	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	gs_set_render_target(dst, NULL);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)s->cx, 0.0f, (float)s->cy, -100.0f, 100.0f);

	struct vec2 texel;
	vec2_set(&texel, 1.0f / (float)s->cx, 1.0f / (float)s->cy);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "image"), src);
	gs_effect_set_vec2(gs_effect_get_param_by_name(s->effect, "src_texel"), &texel);
	while (gs_effect_loop(s->effect, "Gray"))
		gs_draw_sprite(src, 0, s->cx, s->cy);

	gs_enable_framebuffer_srgb(prev_srgb);
	gs_blend_state_pop();
	gs_set_render_target(prev_rt, prev_zs);
	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

static void render_output(struct msm *s, const struct sct_decision *d, gs_texture_t *fallback)
{
	gs_texture_t *t0 = NULL, *t1 = NULL;
	struct slot *sb = NULL;
	bool interp = false;
	float alpha = 0.0f;

	if (d->kind == SCT_INTERP) {
		int a = find_slot(s, d->a), b = find_slot(s, d->b);
		if (a >= 0 && b >= 0 && d->mode != SCT_MODE_PAN && (d->alpha < s->snap || d->alpha > 1.0 - s->snap)) {
			/* close enough to a real frame: show it instead of generating one. Not in
			 * pan mode: there the frames are a tick apart and barely ghost. */
			t0 = gs_texrender_get_texture(s->slots[d->alpha < 0.5 ? a : b].tr);
			s->n_snapped++;
		} else if (a >= 0 && b >= 0) {
			t0 = gs_texrender_get_texture(s->slots[a].tr);
			t1 = gs_texrender_get_texture(s->slots[b].tr);
			sb = &s->slots[b];
			alpha = (float)d->alpha;
			interp = s->calib.sign != 0 && sb->has_flow;
			if (!interp) /* no flow yet: show the nearer frame */
				t0 = alpha < 0.5f ? t0 : t1;
		}
	} else if (d->kind == SCT_PASS) {
		int a = find_slot(s, d->a);
		if (a >= 0)
			t0 = gs_texrender_get_texture(s->slots[a].tr);
	}
	if (!t0)
		t0 = fallback;
	if (!t0)
		return;

	gs_texrender_reset(s->out);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(s->out, s->cx, s->cy, GS_CS_SRGB)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)s->cx, 0.0f, (float)s->cy, -100.0f, 100.0f);
		const bool prev_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		if (interp) {
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(s->effect, "image0"), t0);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(s->effect, "image1"), t1);
			gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "flow01"), sb->f01);
			gs_effect_set_texture(gs_effect_get_param_by_name(s->effect, "flow10"), sb->f10);
			gs_effect_set_float(gs_effect_get_param_by_name(s->effect, "alpha"), alpha);
			while (gs_effect_loop(s->effect, "Interp"))
				gs_draw_sprite(t0, 0, s->cx, s->cy);
			s->n_interp++;
			if (d->mode == SCT_MODE_PAN)
				s->n_pan++;
			else if (d->mode == SCT_MODE_CADENCE)
				s->n_cad++;
			else if (d->mode == SCT_MODE_FILL)
				s->n_fill++;
		} else {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(def, "image"), t0);
			gs_effect_set_float(gs_effect_get_param_by_name(def, "multiplier"), 1.0f);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(t0, 0, s->cx, s->cy);
			s->n_pass++;
		}
		gs_enable_framebuffer_srgb(prev_srgb);
		gs_texrender_end(s->out);
		s->have_output = true;
	}
	gs_blend_state_pop();
}

/* same color handling as OBS gpu-delay draw_frame(), for an SRGB frame */
static void draw_out(struct msm *s)
{
	gs_texture_t *tex = gs_texrender_get_texture(s->out);
	if (!tex)
		return;
	const enum gs_color_space current_space = gs_get_color_space();
	const char *tech = "Draw";
	float multiplier = 1.0f;
	if (current_space == GS_CS_709_SCRGB) {
		tech = "DrawMultiply";
		multiplier = obs_get_video_sdr_white_level() / 80.0f;
	}
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(tex, 0, s->cx, s->cy);
	gs_enable_framebuffer_srgb(previous);
}

/* ---------------- per tick ---------------- */

static void log_stats(struct msm *s, uint64_t now)
{
	double secs = (double)(now - s->stats_start) / 1e9;
	double avg_wait = s->n_new + s->n_repeat ? s->wait_sum / (double)(s->n_new + s->n_repeat) : 0.0;
	const char *cal = s->calib.sign ? "done" : "waiting for camera motion";
	struct dstr msg = {0};
	dstr_printf(&msg,
		    "Last %.0f s: %u new frames, %u repeated, %u smoothed (pan %u, timing %u, fill %u), "
		    "%u shown as-is (%u of them real frames chosen to avoid ghosting), %u scene cuts. "
		    "GPU wait avg %.2f ms, max %.2f ms. Flow direction calibration: %s.",
		    secs, s->n_new, s->n_repeat, s->n_interp, s->n_pan, s->n_cad, s->n_fill, s->n_pass, s->n_snapped,
		    s->n_cuts, avg_wait, s->wait_max, cal);
	obs_log(LOG_INFO, "[%s] %s", obs_source_get_name(s->context), msg.array);
	set_status(s, "%s", msg.array);
	dstr_free(&msg);
	s->stats_start = now;
	s->n_ticks = s->n_new = s->n_repeat = s->n_interp = s->n_pass = 0;
	s->n_pan = s->n_cad = s->n_fill = s->n_cuts = s->n_snapped = 0;
	s->wait_sum = s->wait_max = 0;
}

static bool collect_pending(struct msm *s)
{
	struct sc_gpu_result r;
	if (!s->pending)
		return true;
	s->pending = false;
	if (!sc_gpu_collect(s->gpu, &r))
		return false;

	s->wait_sum += r.wait_ms;
	if (r.wait_ms > s->wait_max)
		s->wait_max = r.wait_ms;

	size_t changed = s->have_prev_gray
				 ? sfm_count_changed(r.gray_cur, s->prev_gray, s->fw, s->fw, s->fh, DUP_MIN_DELTA)
				 : (size_t)s->fw * s->fh;
	s->last_changed = (uint32_t)changed;
	bool is_new = changed >= DUP_MIN_PIXELS;
	memcpy(s->prev_gray, r.gray_cur, (size_t)s->fw * s->fh);
	s->have_prev_gray = true;

	double gx = 0, gy = 0;
	bool conf = true, cut = false;
	struct slot *sl = &s->slots[s->pending_slot];
	sl->has_flow = false;

	if (is_new && r.has_flow) {
		float sign = s->calib.sign ? (float)s->calib.sign : 1.0f;
		double tol = 1.0 * (double)s->fw / SFM_ENGINE_WIDTH;
		struct sfm_motion m = sfm_global_motion(r.fwd, r.flow_pitch, r.ow, r.oh, sign, tol, s->scratch);
		double scale = SFM_ENGINE_WIDTH / (double)s->fw;
		gx = m.x * scale;
		gy = m.y * scale;
		conf = m.confidence > 0.5;

		if (!s->calib.sign && r.has_gray) {
			struct sfm_motion mb =
				sfm_global_motion(r.bwd, r.flow_pitch, r.ow, r.oh, 1.0f, tol, s->scratch);
			/* the vote itself requires one direction to match clearly better, so
			 * mostly-uniform motion is enough (depth-heavy games rarely reach 50%) */
			if (mb.confidence > 0.3) {
				sfm_calib_add(&s->calib,
					      sfm_sign_vote(r.gray_prev, r.gray_cur, s->fw, s->fw, s->fh, mb.x, mb.y));
				if (s->calib.sign) {
					obs_log(LOG_INFO, "[%s] flow direction calibrated (%+d after %d/%d votes), saved",
						obs_source_get_name(s->context), s->calib.sign, s->calib.plus,
						s->calib.minus);
					save_sign(s->calib.sign);
				}
			}
		}
		if (s->calib.sign) {
			double cons =
				sfm_fb_consistency(r.fwd, r.bwd, r.flow_pitch, r.ow, r.oh, (float)s->calib.sign, tol);
			cut = sfm_cut_check(&s->cut, cons);
			if (cut)
				s->n_cuts++;
			sfm_to_uv(r.fwd, r.flow_pitch, r.ow, r.oh, (float)s->calib.sign, s->fw, s->fh, s->uv);
			gs_texture_set_image(sl->f01, (const uint8_t *)s->uv, s->ow * 2 * sizeof(float), false);
			sfm_to_uv(r.bwd, r.flow_pitch, r.ow, r.oh, (float)s->calib.sign, s->fw, s->fh, s->uv);
			gs_texture_set_image(sl->f10, (const uint8_t *)s->uv, s->ow * 2 * sizeof(float), false);
			sl->has_flow = true;
		}
	}

	if (is_new) {
		sl->used = true;
		sl->id = s->pending_tick;
		s->n_new++;
	} else {
		s->n_repeat++;
	}
	sc_gpu_commit(s->gpu, is_new);
	sct_push(&s->eng, s->pending_tick, is_new, gx, gy, conf, cut);
	if (s->tracing && s->trace_n < TRACE_TICKS) {
		struct trace_row *row = &s->trace[s->trace_n++];
		memset(row, 0, sizeof(*row));
		row->tick = s->pending_tick;
		row->is_new = is_new;
		row->conf = conf;
		row->cut = cut;
		row->gx = gx;
		row->gy = gy;
		row->changed = s->last_changed;
	}
	s->have_pushed = true;
	s->last_pushed = s->pending_tick;
	return true;
}

static void write_trace(struct msm *s)
{
	struct dstr csv = {0};
	char name[64];
#ifdef _WIN32
	SYSTEMTIME lt; /* KERNEL32, always present */
	GetLocalTime(&lt);
	snprintf(name, sizeof(name), "trace-%04u%02u%02u-%02u%02u%02u.csv", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
		 lt.wMinute, lt.wSecond);
#else
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	strftime(name, sizeof(name), "trace-%Y%m%d-%H%M%S.csv", &lt);
#endif

	dstr_printf(&csv, "# Grouwdi-OBS-FrameGen %s trace: source %ux%u, flow %ux%u, delay %d frames, lookahead %d\n",
		    PLUGIN_VERSION, s->cx, s->cy, s->fw, s->fh, s->delay_frames, s->eng.lookahead);
	dstr_cat(&csv, "tick,new,gx,gy,conf,cut,kind,a,b,alpha,mode,changed_pixels\n");
	for (int i = 0; i < s->trace_n; i++) {
		const struct trace_row *r = &s->trace[i];
		dstr_catf(&csv, "%lld,%d,%.9g,%.9g,%d,%d,%d,%lld,%lld,%.9g,%d,%u\n", (long long)r->tick, r->is_new,
			  r->gx, r->gy, r->conf, r->cut, r->has_d ? (int)r->d.kind : 0,
			  r->has_d ? (long long)r->d.a : 0LL, r->has_d ? (long long)r->d.b : 0LL,
			  r->has_d ? r->d.alpha : 0.0, r->has_d ? (int)r->d.mode : 0, r->changed);
	}

	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path(name);
	bool ok = false;
	if (dir && path) {
		os_mkdirs(dir);
		ok = os_quick_write_utf8_file(path, csv.array, csv.len, false);
	}
	if (ok) {
		obs_log(LOG_INFO, "[%s] diagnostic trace saved: %s", obs_source_get_name(s->context), path);
		set_status(s, "Diagnostic trace saved: %s", path);
	} else {
		obs_log(LOG_WARNING, "[%s] could not save diagnostic trace to %s", obs_source_get_name(s->context),
			path ? path : "(no path)");
		set_status(s, "Could not save the diagnostic trace (see the OBS log).");
	}
	bfree(dir);
	bfree(path);
	dstr_free(&csv);
	s->tracing = false;
	s->trace_n = 0;
}

static void msm_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct msm *s = data;
	obs_source_t *target = obs_filter_get_target(s->context);
	obs_source_t *parent = obs_filter_get_parent(s->context);

	s->active = false;
	if (!target || !parent || !s->cx || !s->cy || s->failed) {
		obs_source_skip_video_filter(s->context);
		return;
	}
	const enum gs_color_space pref[] = {GS_CS_SRGB};
	if (obs_source_get_color_space(target, 1, pref) != GS_CS_SRGB) {
		set_status(s, "Paused: the source is HDR. Only SDR sources are supported.");
		obs_source_skip_video_filter(s->context);
		return;
	}
	if (s->processed) {
		if (s->have_output) {
			s->active = true;
			draw_out(s);
		} else {
			obs_source_skip_video_filter(s->context);
		}
		return;
	}
	s->processed = true;

	if (!s->ready) {
		if (!init_gpu(s)) {
			free_gpu(s);
			s->failed = true;
			obs_source_skip_video_filter(s->context);
			return;
		}
		s->ready = true;
		s->last_render_tick = s->tick - 1;
	}
	if (s->tick != s->last_render_tick + 1) /* source was hidden for a while */
		reset_pipeline(s);
	s->last_render_tick = s->tick;

	if (s->trace_request && !s->tracing) {
		s->trace_request = false;
		if (!s->trace)
			s->trace = bmalloc(sizeof(struct trace_row) * TRACE_TICKS);
		s->trace_n = 0;
		s->tracing = true;
		set_status(s, "Recording a 60-second diagnostic trace... keep playing normally.");
		obs_log(LOG_INFO, "[%s] diagnostic trace started", obs_source_get_name(s->context));
	}
	if (s->tracing && s->trace_n >= TRACE_TICKS)
		write_trace(s);

	if (s->recalib_request) {
		s->recalib_request = false;
		save_sign(0);
		memset(&s->calib, 0, sizeof(s->calib));
		set_status(s, "Recalibrating flow direction: pan the camera for a few seconds.");
		obs_log(LOG_INFO, "[%s] recalibration requested, waiting for camera motion",
			obs_source_get_name(s->context));
	}

	/* 1. results from last tick */
	if (!collect_pending(s)) {
		set_status(s, "GPU error: %s", sc_gpu_error(s->gpu));
		obs_log(LOG_ERROR, "[%s] %s", obs_source_get_name(s->context), sc_gpu_error(s->gpu));
		free_gpu(s);
		s->failed = true;
		obs_source_skip_video_filter(s->context);
		return;
	}

	/* 2. capture this tick and queue its GPU work */
	int slot = alloc_slot(s);
	if (slot < 0) { /* should not happen; start over rather than drop frames silently */
		reset_pipeline(s);
		slot = alloc_slot(s);
	}
	capture_source(s, target, parent, s->slots[slot].tr);
	gs_texture_t *cur_tex = gs_texrender_get_texture(s->slots[slot].tr);
	if (cur_tex) {
		draw_gray(s, cur_tex, s->gray, s->fw, s->fh);
		draw_gray(s, cur_tex, s->thumb, SC_THUMB_W, SC_THUMB_H);
		gs_flush();
		if (!sc_gpu_submit(s->gpu, s->calib.sign == 0)) {
			set_status(s, "GPU error: %s", sc_gpu_error(s->gpu));
			obs_log(LOG_ERROR, "[%s] %s", obs_source_get_name(s->context), sc_gpu_error(s->gpu));
			free_gpu(s);
			s->failed = true;
			obs_source_skip_video_filter(s->context);
			return;
		}
		s->pending = true;
		s->pending_slot = slot;
		s->pending_tick = s->tick;
	}

	/* 3. decide and 4. draw */
	struct sct_decision d = {SCT_NONE, 0, 0, 0.0};
	if (s->have_pushed) {
		int64_t newest = s->last_pushed;
		int64_t k = newest - s->eng.lookahead;
		d = sct_decide(&s->eng, k, newest);
		free_old_slots(s, k);
		if (s->tracing && s->trace_n > 0 && s->trace[s->trace_n - 1].tick == newest) {
			s->trace[s->trace_n - 1].d = d;
			s->trace[s->trace_n - 1].has_d = true;
		}
	}
	render_output(s, &d, cur_tex);

	s->n_ticks++;
	uint64_t now = os_gettime_ns();
	if (now - s->stats_start >= (uint64_t)STATS_SECONDS * 1000000000ULL)
		log_stats(s, now);

	if (s->have_output) {
		s->active = true;
		draw_out(s);
	} else {
		obs_source_skip_video_filter(s->context);
	}
}

static void msm_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct msm *s = data;
	obs_source_t *target = obs_filter_get_target(s->context);

	s->processed = false;
	s->tick++;

	uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	uint32_t cy = target ? obs_source_get_base_height(target) : 0;
	bool resized = cx != s->cx || cy != s->cy;
	if (resized || s->reinit) {
		s->reinit = false;
		s->cx = cx;
		s->cy = cy;
		obs_enter_graphics();
		free_gpu(s);
		obs_leave_graphics();
		s->failed = false;
	}
}

static enum gs_color_space msm_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	struct msm *s = data;
	if (!s->active)
		return count > 0 ? preferred[0] : GS_CS_SRGB;
	enum gs_color_space space = GS_CS_SRGB;
	for (size_t i = 0; i < count; ++i) {
		space = preferred[i];
		if (space == GS_CS_SRGB)
			break;
	}
	return space;
}

/* ---------------- OBS boilerplate ---------------- */

static const char *msm_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("MotionSmoother");
}

static void msm_update(void *data, obs_data_t *settings)
{
	struct msm *s = data;
	s->delay_frames = (int)obs_data_get_int(settings, S_DELAY);
	if (s->delay_frames < 3)
		s->delay_frames = 3;
	if (s->delay_frames > SCT_MAX_LOOKAHEAD + 1)
		s->delay_frames = SCT_MAX_LOOKAHEAD + 1;
	s->quality = (int)obs_data_get_int(settings, S_QUALITY);
	s->flow_w_setting = (int)obs_data_get_int(settings, S_FLOW_W);
	long long g = obs_data_get_int(settings, S_GHOST);
	s->snap = g == 0 ? 0.03 : (g == 2 ? 0.20 : 0.10);
	s->fill_only = g == 3;
	s->reinit = true;
}

static void *msm_create(obs_data_t *settings, obs_source_t *context)
{
	struct msm *s = bzalloc(sizeof(*s));
	s->context = context;
	pthread_mutex_init(&s->status_mutex, NULL);
	dstr_copy(&s->status, obs_module_text("MotionSmoother.Starting"));

	char *path = obs_module_file("motion-smoother.effect");
	obs_enter_graphics();
	s->effect = path ? gs_effect_create_from_file(path, NULL) : NULL;
	obs_leave_graphics();
	if (!s->effect)
		obs_log(LOG_ERROR, "could not load %s", path ? path : "motion-smoother.effect");
	bfree(path);

	msm_update(s, settings);
	return s;
}

static void msm_destroy(void *data)
{
	struct msm *s = data;
	obs_enter_graphics();
	free_gpu(s);
	gs_effect_destroy(s->effect);
	obs_leave_graphics();
	bfree(s->trace);
	pthread_mutex_destroy(&s->status_mutex);
	dstr_free(&s->status);
	bfree(s);
}

static void msm_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_DELAY, 3);
	obs_data_set_default_int(settings, S_QUALITY, SC_GPU_MEDIUM);
	obs_data_set_default_int(settings, S_FLOW_W, 1280);
	obs_data_set_default_int(settings, S_GHOST, 3);
}

static bool msm_trace_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct msm *s = data;
	s->trace_request = true;
	set_status(s, "Diagnostic trace requested: recording starts with the next frame (60 s).");
	return false;
}

static bool msm_recalib_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct msm *s = data;
	s->recalib_request = true;
	set_status(s, "Recalibration requested: pan the camera for a few seconds.");
	return false;
}

static obs_properties_t *msm_properties(void *data)
{
	struct msm *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_int_slider(props, S_DELAY, obs_module_text("MotionSmoother.Delay"), 3,
					  SCT_MAX_LOOKAHEAD + 1, 1);
	obs_property_set_long_description(p, obs_module_text("MotionSmoother.Delay.Tooltip"));

	p = obs_properties_add_list(props, S_QUALITY, obs_module_text("MotionSmoother.Quality"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Quality.Fast"), SC_GPU_FAST);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Quality.Medium"), SC_GPU_MEDIUM);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Quality.Slow"), SC_GPU_SLOW);

	p = obs_properties_add_list(props, S_GHOST, obs_module_text("MotionSmoother.Ghost"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Ghost.Smooth"), 0);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Ghost.Balanced"), 1);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Ghost.Sharp"), 2);
	obs_property_list_add_int(p, obs_module_text("MotionSmoother.Ghost.FillOnly"), 3);
	obs_property_set_long_description(p, obs_module_text("MotionSmoother.Ghost.Tooltip"));

	p = obs_properties_add_list(props, S_FLOW_W, obs_module_text("MotionSmoother.FlowWidth"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "640", 640);
	obs_property_list_add_int(p, "960", 960);
	obs_property_list_add_int(p, "1280", 1280);

	obs_properties_add_button(props, S_TRACE, obs_module_text("MotionSmoother.RecordTrace"), msm_trace_clicked);
	p = obs_properties_add_button(props, S_RECALIB, obs_module_text("MotionSmoother.Recalibrate"),
				      msm_recalib_clicked);
	obs_property_set_long_description(p, obs_module_text("MotionSmoother.Recalibrate.Tooltip"));

	if (s) {
		pthread_mutex_lock(&s->status_mutex);
		p = obs_properties_add_text(props, S_STATUS, s->status.array, OBS_TEXT_INFO);
		pthread_mutex_unlock(&s->status_mutex);
		obs_property_text_set_info_word_wrap(p, true);
	}
	return props;
}

struct obs_source_info motion_smoother_filter = {
	.id = "capture_motion_smoother_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = msm_name,
	.create = msm_create,
	.destroy = msm_destroy,
	.update = msm_update,
	.get_defaults = msm_defaults,
	.get_properties = msm_properties,
	.video_tick = msm_tick,
	.video_render = msm_render,
	.video_get_color_space = msm_color_space,
};

#endif /* _WIN32 || SC_TEST_HARNESS */
