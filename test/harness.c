/*
 * Test harness: runs the real capture-smoother-filter.c against simulated
 * capture streams, with libobs calls stubbed out. Not part of the plugin.
 */
#include <obs-module.h>
#include <util/dstr.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc-core.h"

/* ---------------- stubs ---------------- */
const char *PLUGIN_NAME = "Grouwdi-OBS-FrameGen";
const char *PLUGIN_VERSION = "test";
static uint64_t g_now;
static uint64_t g_interval = 16666667; /* 60 fps */
static int g_released, g_logs;
static char g_last_log[4096];
static char g_logs_all[8][4096]; /* every report of the current run */

void obs_log(int level, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vsnprintf(g_last_log, sizeof(g_last_log), fmt, a);
	va_end(a);
	if (g_logs < 8)
		memcpy(g_logs_all[g_logs], g_last_log, sizeof(g_last_log));
	g_logs++;
	(void)level;
}
void blogva(int l, const char *f, va_list a) { (void)l; (void)f; (void)a; }
void blog(int l, const char *f, ...) { (void)l; (void)f; }
uint64_t obs_get_video_frame_time(void) { return g_now; }
uint64_t obs_get_frame_interval_ns(void) { return g_interval; }
obs_source_t *obs_filter_get_parent(const obs_source_t *f) { (void)f; return (obs_source_t *)0x1; }
const char *obs_source_get_name(const obs_source_t *s) { (void)s; return "Capture Card"; }
const char *obs_module_text(const char *k) { return k; }
bool obs_get_video_info(struct obs_video_info *o)
{
	memset(o, 0, sizeof(*o));
	o->fps_num = 60;
	o->fps_den = 1;
	return true;
}
void obs_source_release_frame(obs_source_t *s, struct obs_source_frame *f)
{
	(void)s;
	for (int i = 0; i < 2; i++)
		free(f->data[i]);
	free(f);
	g_released++;
}
/* properties are not exercised; settings are faked through obs_data stubs */
static bool g_set_repair;
static long long g_set_log = 10, g_set_sens = 5;
long long obs_data_get_int(obs_data_t *d, const char *n)
{
	(void)d;
	return strcmp(n, "log_seconds") == 0 ? g_set_log : g_set_sens;
}
bool obs_data_get_bool(obs_data_t *d, const char *n) { (void)d; (void)n; return g_set_repair; }
void obs_data_set_default_bool(obs_data_t *d, const char *n, bool v) { (void)d; (void)n; (void)v; }
void obs_data_set_default_int(obs_data_t *d, const char *n, long long v) { (void)d; (void)n; (void)v; }
obs_properties_t *obs_properties_create(void) { return NULL; }
obs_property_t *obs_properties_add_int_slider(obs_properties_t *p, const char *a, const char *b, int c, int d, int e)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; return NULL; }
void obs_property_int_set_suffix(obs_property_t *p, const char *s) { (void)p; (void)s; }
obs_property_t *obs_properties_add_bool(obs_properties_t *p, const char *a, const char *b)
{ (void)p; (void)a; (void)b; return NULL; }
void obs_property_set_long_description(obs_property_t *p, const char *s) { (void)p; (void)s; }
obs_property_t *obs_properties_add_text(obs_properties_t *p, const char *a, const char *b, enum obs_text_type t)
{ (void)p; (void)a; (void)b; (void)t; return NULL; }
void obs_property_text_set_info_word_wrap(obs_property_t *p, bool w) { (void)p; (void)w; }

extern struct obs_source_info capture_smoother_filter;

/* ---------------- synthetic frames ---------------- */
#define W 1280
#define H 720
static uint8_t g_tex[H][4096];

static void make_texture(void)
{
	uint32_t r = 12345;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < 4096; x++) {
			r = r * 1103515245u + 12345u;
			g_tex[y][x] = (uint8_t)(r >> 24);
		}
	/* light blur so motion is measurable but textured */
	for (int y = 0; y < H; y++)
		for (int x = 1; x < 4095; x++)
			g_tex[y][x] = (uint8_t)((g_tex[y][x - 1] + 2 * g_tex[y][x] + g_tex[y][x + 1]) / 4);
}

/* how the game content moves: position in pixels for content frame k */
static int g_pos_mode; /* 0: 10 px per frame, 1: steps alternate 10 px and 1 px, 2: still 3..8 s */
static int content_pos(int k, double t)
{
	if (g_pos_mode == 1)
		return (k / 2) * 11 + (k % 2) * 10;
	if (g_pos_mode == 2 && t >= 3.0 && t < 8.0)
		return 600; /* frozen picture */
	return k * 10;
}

/* NV12 frame showing the texture panned to position pos (pixels) */
static struct obs_source_frame *make_frame(int pos, uint64_t ts, enum video_format fmt)
{
	struct obs_source_frame *f = calloc(1, sizeof(*f));
	f->format = fmt;
	f->width = W;
	f->height = H;
	f->timestamp = ts;
	f->linesize[0] = W;
	f->linesize[1] = W;
	f->data[0] = malloc((size_t)W * H);
	f->data[1] = malloc((size_t)W * H / 2);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			f->data[0][y * W + x] = g_tex[y][(x + pos) % 4096];
	memset(f->data[1], 128, (size_t)W * H / 2);
	return f;
}

static int g_fail;
#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			printf("  FAIL: " __VA_ARGS__);   \
			printf("\n");                     \
			g_fail++;                         \
		} else {                                  \
			printf("  ok:   " __VA_ARGS__);   \
			printf("\n");                     \
		}                                         \
	} while (0)

static unsigned count_in(const char *log, const char *key)
{
	const char *p = strstr(log, key);
	return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 9999;
}

/*
 * Run one scenario.
 *  content_rate: rate of the game content (pan steps of 10 px per content frame)
 *  card_rate:    frames per second the card delivers
 *  ticks at 60/s; the filter is called on a tick only when a new card frame is due
 */
static int g_out_repeats; /* outputs that look identical to the previous output */
static void run(const char *name, double content_rate, double card_rate, int seconds, bool repair,
		enum video_format fmt, int *outputs, int *inputs)
{
	static uint8_t prev_l[(W / 2) * (H / 2)], cur_l[(W / 2) * (H / 2)];
	bool have_prev_l = false;
	g_out_repeats = 0;
	void *s;
	g_set_repair = repair;
	g_released = 0;
	g_logs = 0;
	g_last_log[0] = 0;
	s = capture_smoother_filter.create(NULL, NULL);

	int in = 0, out = 0;
	int ticks = seconds * 60;
	int next_card = 0;
	for (int t = 0; t < ticks; t++) {
		g_now = 1000000000ULL + (uint64_t)t * g_interval;
		/* card frames due at or before this tick */
		double card_time = next_card / card_rate;
		double tick_time = t / 60.0;
		if (card_time > tick_time + 1e-9)
			continue; /* OBS has no new frame: re-shows the old one, filter not called */
		uint64_t ts = 5000000000ULL + (uint64_t)(card_time * 1e9);
		int content = (int)floor(card_time * content_rate + 1e-9);
		struct obs_source_frame *f = make_frame(content_pos(content, card_time), ts, fmt);
		next_card++;
		in++;
		struct obs_source_frame *o = capture_smoother_filter.filter_video(s, f);
		if (o) {
			out++;
			if (o->format == VIDEO_FORMAT_NV12) {
				sc_half_luma(cur_l, SC_PLANAR_420_2, o->data[0], o->linesize[0], o->width, o->height);
				if (have_prev_l && t > 120 && sc_same_picture(prev_l, cur_l, sizeof(cur_l)))
					g_out_repeats++;
				memcpy(prev_l, cur_l, sizeof(cur_l));
				have_prev_l = true;
			}
			obs_source_release_frame(NULL, o); /* OBS would render then release */
		}
	}
	printf("[%s]\n%s\n", name, g_last_log);
	capture_smoother_filter.filter_remove(s, NULL);
	capture_smoother_filter.destroy(s);
	*outputs = out;
	*inputs = in;
}

int main(void)
{
	int out, in;
	make_texture();

	printf("== 1. clean: game 60, card 60, OBS 60\n");
	run("clean", 60, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	CHECK(count_in(g_last_log, "re-showed an old frame: ") == 0, "no OBS re-shows");
	CHECK(count_in(g_last_log, "Repeated content from card: ") == 0, "no content repeats");
	CHECK(strstr(g_last_log, "card delivered 60.000 fps") != NULL, "card rate measured as 60.000");
	CHECK(g_released == in, "every frame released exactly once (%d of %d)", g_released, in);

	printf("== 2. upstream repeat: game 59, card 60\n");
	run("upstream", 59, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	unsigned rep = count_in(g_last_log, "Repeated content from card: ");
	CHECK(rep >= 9 && rep <= 11, "about 10 repeats per 10 s (got %u)", rep);
	CHECK(strstr(g_last_log, "Repeated content from card") && strstr(g_last_log, "every 1.00 s"),
	      "repeat rhythm reported as every 1.00 s");
	CHECK(strstr(g_last_log, "upstream") != NULL, "hint blames upstream");
	CHECK(count_in(g_last_log, "re-showed an old frame: ") == 0, "no OBS re-shows");

	printf("== 3. OBS-side: game 59, card 59, OBS 60\n");
	run("obs-side", 59, 59, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	unsigned rs = count_in(g_last_log, "re-showed an old frame: ");
	CHECK(rs >= 9 && rs <= 11, "about 10 OBS re-shows per 10 s (got %u)", rs);
	CHECK(strstr(g_last_log, "card delivered 59.000 fps") != NULL, "card rate measured as 59.000");
	CHECK(strstr(g_last_log, "Match OBS FPS") != NULL, "hint says match OBS FPS");
	CHECK(count_in(g_last_log, "Repeated content from card: ") == 0, "no content repeats");

	printf("== 4. repair on: game 59, card 60\n");
	run("repair", 59, 60, 21, true, VIDEO_FORMAT_NV12, &out, &in);
	unsigned fixed = count_in(g_last_log, "Repaired: ");
	CHECK(fixed >= 9 && fixed <= 11, "about 10 repaired per 10 s (got %u)", fixed);
	CHECK(out == in - 2, "output = input - 2 (2-frame delay): %d vs %d", out, in);
	CHECK(g_released == in, "every frame released exactly once incl. held (%d of %d)", g_released, in);

	CHECK(g_out_repeats == 0, "no repeated frames left in the output (%d)", g_out_repeats);

	printf("== 4b. same stream, repair off, for comparison\n");
	run("no-repair", 59, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	CHECK(g_out_repeats >= 18, "output without repair still has its repeats (%d)", g_out_repeats);

	printf("== 5. unsupported format (P010) passes through\n");
	run("p010", 59, 60, 11, true, VIDEO_FORMAT_P010, &out, &in);
	CHECK(out == in, "all frames passed through (%d of %d)", out, in);
	CHECK(g_released == in, "every frame released exactly once (%d of %d)", g_released, in);

	printf("== 6. game 119 into card 60 (short steps, not repeats)\n");
	run("119", 119, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	CHECK(count_in(g_last_log, "Repeated content from card: ") == 0, "short steps are not mis-flagged as repeats");
	CHECK(strstr(g_last_log, "upstream") == NULL, "hint does not blame upstream");

	printf("== 7. uneven motion: steps alternate 10 px and 1 px (the old rule over-counted these)\n");
	g_pos_mode = 1;
	run("uneven", 60, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	CHECK(count_in(g_last_log, "Repeated content from card: ") == 0, "small real steps are not repeats (got %u)",
	      count_in(g_last_log, "Repeated content from card: "));
	g_pos_mode = 0;

	printf("== 8. still picture 3..8 s (menu/pause), moving otherwise\n");
	g_pos_mode = 2;
	run("still", 60, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	CHECK(g_logs >= 2, "two reports (%d)", g_logs);
	CHECK(count_in(g_logs_all[0], "Repeated content from card: ") == 0, "still picture is not counted as repeats (got %u)",
	      count_in(g_logs_all[0], "Repeated content from card: "));
	{
		unsigned st = count_in(g_logs_all[0], "Still picture (menu, pause, loading): ");
		CHECK(st >= 295 && st <= 300, "still frames reported separately (%u, expected about 299)", st);
	}
	CHECK(strstr(g_logs_all[0], "upstream") == NULL, "hint does not blame upstream for a menu");
	g_pos_mode = 0;

	printf("== 9. game 10 fps into card 60 (each picture held 6 frames = hitch-like runs)\n");
	run("10fps", 10, 60, 21, false, VIDEO_FORMAT_NV12, &out, &in);
	rep = count_in(g_last_log, "Repeated content from card: ");
	CHECK(rep >= 495 && rep <= 505, "runs of 5 identical frames count as repeats, not still (got %u, expected 500)", rep);

	printf("== 10. game 30 into card 60 with repair: every other frame repaired\n");
	run("30-repair", 30, 60, 21, true, VIDEO_FORMAT_NV12, &out, &in);
	fixed = count_in(g_last_log, "Repaired: ");
	CHECK(fixed >= 295 && fixed <= 305, "about 300 repaired per 10 s (got %u)", fixed);
	CHECK(g_released == in, "every frame released exactly once (%d of %d)", g_released, in);

	printf("== 11. game 20 into card 60 with repair: runs of 2 are counted but not blended\n");
	run("20-repair", 20, 60, 21, true, VIDEO_FORMAT_NV12, &out, &in);
	rep = count_in(g_last_log, "Repeated content from card: ");
	fixed = count_in(g_last_log, "Repaired: ");
	CHECK(rep >= 395 && rep <= 405, "about 400 repeats per 10 s (got %u)", rep);
	CHECK(fixed == 0, "double repeats are not blended (a 50/50 mix would be wrong there) (got %u)", fixed);

	/* direct check of the blend math */
	{
		uint8_t a0[4] = {0, 10, 255, 100}, c0[4] = {255, 20, 255, 101}, d0[4];
		uint8_t *d[3] = {d0, NULL, NULL};
		const uint8_t *pa[3] = {a0, NULL, NULL}, *pc[3] = {c0, NULL, NULL};
		uint32_t ls[3] = {4, 0, 0};
		sc_blend_planes(d, ls, pa, ls, pc, ls, SC_PLANAR_1, 1);
		CHECK(d0[0] == 128 && d0[1] == 15 && d0[2] == 255 && d0[3] == 101,
		      "blend = rounded average (%u %u %u %u)", d0[0], d0[1], d0[2], d0[3]);
	}

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
