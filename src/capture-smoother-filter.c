/*
 * Grouwdi-OBS-FrameGen - "Capture Smoother": OBS async video filter for
 * capture-card sources.
 *
 *   - Measures the real frame rate the capture card delivers vs. OBS's FPS.
 *   - Counts render ticks where OBS had no new frame and re-showed the old one.
 *   - Counts gaps in the card's own frame timestamps.
 *   - Counts frames whose picture repeats the previous frame, with the same exact
 *     rule as Motion Smoother. Runs longer than SC_MAX_REPEAT_RUN frames are still
 *     pictures (menus, pauses) and are reported separately.
 *   - Optionally repairs single repeats by blending the frames on either side
 *     (adds 2 frames of video delay while enabled; off by default because Motion
 *     Smoother fills repeats with real motion instead of a 50/50 blend).
 * Results go to the OBS log and to the filter's properties window.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <obs-module.h>
#include <util/deque.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "plugin-support.h"
#include "sc-core.h"

#define S_REPAIR "repair_repeats"
#define S_LOG_SECONDS "log_seconds"
#define S_STATUS "status"

#define TS_JUMP_NS 1000000000ULL /* > 1 s between frames = treat as a reset */
#define TS_HIST 15

struct held {
	struct obs_source_frame *frame;
};

struct smoother {
	obs_source_t *context;

	/* settings (written on the UI thread, read on the video thread) */
	volatile bool repair;
	volatile bool reset_pending;
	int log_seconds;

	/* frame queue (repair mode only), contains struct held */
	struct deque queue;

	/* repeat detection: previous frame's half-size luma */
	uint8_t *luma_prev, *luma_cur;
	size_t luma_size;
	bool have_prev;
	bool same_a;   /* frame A (oldest of the three) repeated its predecessor */
	bool same_ab;  /* frame B repeats A */
	uint32_t run;  /* identical frames in a row so far */
	uint32_t run_reported; /* of those, already counted as still picture in an earlier report */
	uint64_t run_start_tick;

	enum video_format last_format;
	uint32_t last_w, last_h;
	bool warned_format;

	/* timing */
	uint64_t last_ts;        /* card frame timestamp */
	uint64_t last_tick_time; /* OBS video time at last call */
	uint64_t ts_delta_hist[TS_HIST];
	int ts_hist_count, ts_hist_pos;

	/* stats window */
	uint64_t win_start_time;
	uint64_t win_first_ts, win_last_ts;
	uint32_t win_frames;
	uint32_t win_obs_reshows;
	uint32_t win_card_gaps;
	uint32_t win_repeats;
	uint32_t win_still;
	uint32_t win_repaired;
	struct sc_events ev_reshow, ev_repeat;

	pthread_mutex_t status_mutex;
	struct dstr status;
};

/* ------------------------------------------------------------------ */

static enum sc_layout layout_for(enum video_format f)
{
	switch (f) {
	case VIDEO_FORMAT_I420:
		return SC_PLANAR_420_3;
	case VIDEO_FORMAT_NV12:
		return SC_PLANAR_420_2;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I444:
		return SC_PLANAR_FULL_3;
	case VIDEO_FORMAT_Y800:
		return SC_PLANAR_1;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
		return SC_PACKED_Y0;
	case VIDEO_FORMAT_UYVY:
		return SC_PACKED_Y1;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		return SC_PACKED_RGB4;
	case VIDEO_FORMAT_BGR3:
		return SC_PACKED_RGB3;
	default:
		return SC_UNSUPPORTED;
	}
}

static const char *format_name(enum video_format f)
{
	const char *n = get_video_format_name(f);
	return n ? n : "unknown";
}

static void release_queue(struct smoother *s, obs_source_t *parent)
{
	while (s->queue.size) {
		struct held h;
		deque_pop_front(&s->queue, &h, sizeof(h));
		obs_source_release_frame(parent, h.frame);
	}
}

static void reset_detection(struct smoother *s)
{
	s->have_prev = false;
	s->same_a = s->same_ab = false;
	s->run = 0;
	s->run_reported = 0;
	s->ts_hist_count = s->ts_hist_pos = 0;
	s->last_ts = 0;
	s->last_tick_time = 0;
}

static void reset_window(struct smoother *s, uint64_t now)
{
	s->win_start_time = now;
	s->win_first_ts = s->win_last_ts = 0;
	s->win_frames = 0;
	s->win_obs_reshows = 0;
	s->win_card_gaps = 0;
	s->win_repeats = 0;
	s->win_still = 0;
	s->win_repaired = 0;
	sc_events_clear(&s->ev_reshow);
	sc_events_clear(&s->ev_repeat);
}

static uint64_t ts_normal_delta(struct smoother *s)
{
	uint64_t tmp[TS_HIST];
	int n = s->ts_hist_count;
	if (!n)
		return 0;
	memcpy(tmp, s->ts_delta_hist, sizeof(uint64_t) * (size_t)n);
	/* insertion sort, n <= 15 */
	for (int i = 1; i < n; i++) {
		uint64_t v = tmp[i];
		int j = i - 1;
		while (j >= 0 && tmp[j] > v) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = v;
	}
	return tmp[n / 2];
}

static void push_ts_delta(struct smoother *s, uint64_t d)
{
	s->ts_delta_hist[s->ts_hist_pos] = d;
	s->ts_hist_pos = (s->ts_hist_pos + 1) % TS_HIST;
	if (s->ts_hist_count < TS_HIST)
		s->ts_hist_count++;
}

static void describe_rhythm(struct dstr *out, const struct sc_events *e, double tick_s)
{
	uint64_t p = sc_events_period(e);
	if (p)
		dstr_catf(out, " (steady, every %.2f s)", (double)p * tick_s);
	else if (e->count >= 2)
		dstr_cat(out, " (irregular)");
}

static void finish_window(struct smoother *s, uint64_t now)
{
	struct obs_video_info ovi;
	double obs_fps = 0.0, card_fps = 0.0, secs, tick_s;
	struct dstr msg = {0};

	if (!obs_get_video_info(&ovi) || !ovi.fps_den)
		return;
	/* a still picture that is still on screen: count what is known so far */
	if (s->run > SC_MAX_REPEAT_RUN) {
		s->win_still += s->run - s->run_reported;
		s->run_reported = s->run;
	}
	obs_fps = (double)ovi.fps_num / (double)ovi.fps_den;
	tick_s = 1.0 / obs_fps;
	secs = (double)(now - s->win_start_time) / 1e9;

	if (s->win_frames > 1 && s->win_last_ts > s->win_first_ts)
		card_fps = (double)(s->win_frames - 1) * 1e9 / (double)(s->win_last_ts - s->win_first_ts);

	dstr_catf(&msg, "Last %.0f s: card delivered %.3f fps, OBS runs at %.3f fps.\n", secs, card_fps, obs_fps);
	dstr_catf(&msg, "OBS re-showed an old frame: %u", s->win_obs_reshows);
	describe_rhythm(&msg, &s->ev_reshow, tick_s);
	dstr_catf(&msg, "\nRepeated content from card: %u", s->win_repeats);
	describe_rhythm(&msg, &s->ev_repeat, tick_s);
	if (s->win_still)
		dstr_catf(&msg, "\nStill picture (menu, pause, loading): %u frames, not counted as repeats",
			  s->win_still);
	dstr_catf(&msg, "\nGaps in card timestamps: %u", s->win_card_gaps);
	if (s->repair)
		dstr_catf(&msg, "\nRepaired: %u", s->win_repaired);

	/* plain-language hint */
	if (s->win_frames < 10) {
		dstr_cat(&msg, "\nHint: few frames arrived. Is the source showing video?");
	} else if (s->win_repeats >= 3 && s->win_repeats >= s->win_obs_reshows) {
		dstr_cat(&msg, "\nHint: the repeats are already in the signal from the card. "
			       "The cause is upstream (game frame rate vs. the capture output on the gaming PC).");
	} else if (s->win_obs_reshows >= 3) {
		if (card_fps > 0.0 && card_fps < obs_fps - 0.005)
			dstr_catf(&msg,
				  "\nHint: the card is slower than OBS (%.3f vs %.3f), so OBS re-shows frames. "
				  "Match OBS FPS to the card's rate.",
				  card_fps, obs_fps);
		else
			dstr_cat(&msg, "\nHint: OBS is re-showing frames even though the card rate matches. "
				       "Check the source's Buffering setting and OBS render lag.");
	} else if (s->win_still * 2 > s->win_frames) {
		dstr_cat(&msg, "\nHint: the picture was mostly still (menu, pause or loading screen), so there was "
			       "little motion to measure.");
	} else if (s->win_card_gaps >= 3) {
		dstr_cat(&msg, "\nHint: the card itself is skipping frames. Check its input signal and drivers.");
	} else {
		dstr_cat(&msg, "\nHint: no repeats or re-shows in this window. If you still see a hitch, it is "
			       "likely uneven motion steps (game fps vs. capture rate) or the game's own frame "
			       "time spikes, which are not repeats.");
	}

	obs_log(LOG_INFO, "[%s] %s", obs_source_get_name(s->context), msg.array);

	pthread_mutex_lock(&s->status_mutex);
	dstr_copy_dstr(&s->status, &msg);
	pthread_mutex_unlock(&s->status_mutex);

	dstr_free(&msg);
	reset_window(s, now);
}

/* A run of identical frames just ended (a new picture arrived): repeats or a still picture. */
static void end_run(struct smoother *s, uint64_t interval)
{
	if (!s->run)
		return;
	if (s->run <= SC_MAX_REPEAT_RUN) {
		s->win_repeats += s->run;
		for (uint32_t i = 0; i < s->run; i++)
			sc_events_add(&s->ev_repeat, s->run_start_tick + i);
	} else {
		s->win_still += s->run - s->run_reported;
	}
	s->run = 0;
	s->run_reported = 0;
	(void)interval;
}

/* Compare this frame (C) with the previous one (B). Returns true if B is a single repeat
 * that can be repaired: B repeats A, A did not repeat its own predecessor, and C is new. */
static bool judge(struct smoother *s, const struct obs_source_frame *frame, enum sc_layout layout, uint64_t now,
		  uint64_t interval)
{
	size_t need = (size_t)(frame->width / 2) * (frame->height / 2);
	if (need != s->luma_size) {
		bfree(s->luma_prev);
		bfree(s->luma_cur);
		s->luma_prev = bmalloc(need ? need : 1);
		s->luma_cur = bmalloc(need ? need : 1);
		s->luma_size = need;
		s->have_prev = false;
	}
	if (!need || !sc_half_luma(s->luma_cur, layout, frame->data[0], frame->linesize[0], frame->width,
				   frame->height)) {
		s->have_prev = false;
		return false;
	}
	bool same_bc = s->have_prev && sc_same_picture(s->luma_prev, s->luma_cur, need);
	if (same_bc) {
		if (!s->run)
			s->run_start_tick = interval ? now / interval : 0;
		s->run++;
	} else {
		end_run(s, interval);
	}
	bool b_single_repeat = s->same_ab && !s->same_a && !same_bc;

	s->same_a = s->same_ab;
	s->same_ab = same_bc;
	uint8_t *t = s->luma_prev;
	s->luma_prev = s->luma_cur;
	s->luma_cur = t;
	s->have_prev = true;
	return b_single_repeat;
}

static bool same_shape(const struct obs_source_frame *x, const struct obs_source_frame *y)
{
	if (x->format != y->format || x->width != y->width || x->height != y->height)
		return false;
	for (int p = 0; p < 3; p++)
		if (x->linesize[p] != y->linesize[p])
			return false;
	return true;
}

static void blend_into(struct obs_source_frame *dst, const struct obs_source_frame *a, const struct obs_source_frame *c,
		       enum sc_layout layout)
{
	uint8_t *d[3] = {dst->data[0], dst->data[1], dst->data[2]};
	const uint8_t *pa[3] = {a->data[0], a->data[1], a->data[2]};
	const uint8_t *pc[3] = {c->data[0], c->data[1], c->data[2]};
	sc_blend_planes(d, dst->linesize, pa, a->linesize, pc, c->linesize, layout, dst->height);
}

static struct obs_source_frame *smoother_video(void *data, struct obs_source_frame *frame)
{
	struct smoother *s = data;
	obs_source_t *parent = obs_filter_get_parent(s->context);
	uint64_t now = obs_get_video_frame_time();
	uint64_t interval = obs_get_frame_interval_ns();
	enum sc_layout layout = layout_for(frame->format);
	bool repair = s->repair;
	struct held h = {.frame = frame};
	bool b_repeats = false;

	/* resets: settings change, format/size change, timestamp jump */
	bool shape_changed = frame->format != s->last_format || frame->width != s->last_w || frame->height != s->last_h;
	bool ts_jump = s->last_ts && (frame->timestamp < s->last_ts || frame->timestamp - s->last_ts > TS_JUMP_NS);
	if (s->reset_pending || shape_changed || ts_jump) {
		release_queue(s, parent);
		reset_detection(s);
		s->reset_pending = false;
		s->last_format = frame->format;
		s->last_w = frame->width;
		s->last_h = frame->height;
		if (layout == SC_UNSUPPORTED && !s->warned_format) {
			obs_log(LOG_WARNING,
				"[%s] video format %s is not supported for content checks or repair; "
				"timing measurements still work. Set the capture source's Video Format to NV12 or YUY2.",
				obs_source_get_name(s->context), format_name(frame->format));
			s->warned_format = true;
		}
	}
	if (!s->win_start_time)
		reset_window(s, now);

	/* --- OBS-side re-shows: render ticks that passed without a new frame --- */
	if (s->last_tick_time && interval && now > s->last_tick_time) {
		uint64_t ticks = (now - s->last_tick_time + interval / 2) / interval;
		if (ticks > 1) {
			s->win_obs_reshows += (uint32_t)(ticks - 1);
			sc_events_add(&s->ev_reshow, now / interval - 1);
		}
	}
	s->last_tick_time = now;

	/* --- card-side timestamp gaps and delivered rate --- */
	if (s->last_ts && frame->timestamp > s->last_ts) {
		uint64_t d = frame->timestamp - s->last_ts;
		uint64_t normal = ts_normal_delta(s);
		if (s->ts_hist_count >= 5 && normal && d > normal + normal / 2)
			s->win_card_gaps++;
		else
			push_ts_delta(s, d);
	}
	s->last_ts = frame->timestamp;
	if (!s->win_first_ts)
		s->win_first_ts = frame->timestamp;
	s->win_last_ts = frame->timestamp;
	s->win_frames++;

	/* --- content repeats (counted in end_run, when the run of identical frames ends) --- */
	if (layout != SC_UNSUPPORTED)
		b_repeats = judge(s, frame, layout, now, interval);

	struct obs_source_frame *out = frame;

	if (repair && layout != SC_UNSUPPORTED) {
		/* queue holds [A, B]; C (this frame) is pushed, B is judged, A goes out */
		deque_push_back(&s->queue, &h, sizeof(h));
		if (s->queue.size < 3 * sizeof(struct held)) {
			out = NULL; /* filling the 2-frame delay */
		} else {
			struct held a, b;
			/* pop/peek copy safely even when an element wraps the ring buffer */
			deque_pop_front(&s->queue, &a, sizeof(a));
			deque_peek_front(&s->queue, &b, sizeof(b));
			if (b_repeats && same_shape(a.frame, b.frame) && same_shape(b.frame, frame)) {
				/* A is still ours until we return it, so reading it here is safe */
				blend_into(b.frame, a.frame, frame, layout);
				s->win_repaired++;
			}
			out = a.frame;
		}
	} else if (s->queue.size) {
		/* repair was switched off: drain anything still held */
		release_queue(s, parent);
	}

	if (s->log_seconds > 0 && now - s->win_start_time >= (uint64_t)s->log_seconds * 1000000000ULL)
		finish_window(s, now);

	return out;
}

/* ------------------------------------------------------------------ */

static const char *smoother_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CaptureSmoother");
}

static void smoother_update(void *data, obs_data_t *settings)
{
	struct smoother *s = data;
	s->log_seconds = (int)obs_data_get_int(settings, S_LOG_SECONDS);
	s->repair = obs_data_get_bool(settings, S_REPAIR);
	s->reset_pending = true;
}

static void *smoother_create(obs_data_t *settings, obs_source_t *context)
{
	struct smoother *s = bzalloc(sizeof(*s));
	s->context = context;
	s->last_format = VIDEO_FORMAT_NONE;
	pthread_mutex_init(&s->status_mutex, NULL);
	dstr_copy(&s->status, obs_module_text("StatusWaiting"));
	smoother_update(s, settings);
	return s;
}

static void smoother_remove(void *data, obs_source_t *parent)
{
	struct smoother *s = data;
	release_queue(s, parent);
}

static void smoother_destroy(void *data)
{
	struct smoother *s = data;
	/* frames are released in filter_remove, which libobs calls first */
	deque_free(&s->queue);
	bfree(s->luma_prev);
	bfree(s->luma_cur);
	pthread_mutex_destroy(&s->status_mutex);
	dstr_free(&s->status);
	bfree(s);
}

static void smoother_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_REPAIR, false);
	obs_data_set_default_int(settings, S_LOG_SECONDS, 10);
}

static obs_properties_t *smoother_properties(void *data)
{
	struct smoother *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_int_slider(props, S_LOG_SECONDS, obs_module_text("LogSeconds"), 5, 60, 5);
	obs_property_int_set_suffix(p, " s");

	p = obs_properties_add_bool(props, S_REPAIR, obs_module_text("RepairRepeats"));
	obs_property_set_long_description(p, obs_module_text("RepairRepeats.Tooltip"));

	if (s) {
		pthread_mutex_lock(&s->status_mutex);
		p = obs_properties_add_text(props, S_STATUS, s->status.array, OBS_TEXT_INFO);
		pthread_mutex_unlock(&s->status_mutex);
		obs_property_text_set_info_word_wrap(p, true);
	}
	return props;
}

struct obs_source_info capture_smoother_filter = {
	.id = "capture_smoother_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = smoother_name,
	.create = smoother_create,
	.destroy = smoother_destroy,
	.update = smoother_update,
	.get_defaults = smoother_defaults,
	.get_properties = smoother_properties,
	.filter_video = smoother_video,
	.filter_remove = smoother_remove,
};
