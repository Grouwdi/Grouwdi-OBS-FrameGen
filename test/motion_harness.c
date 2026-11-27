/*
 * Runs the real motion-smoother-filter.c against a simulated capture source.
 * OBS graphics calls and the NVIDIA GPU layer (sc-gpu) are replaced with mocks
 * that model a camera panning over a scene, so the filter's orchestration
 * (slots, calibration, decisions, what reaches the shader) can be checked.
 */
#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc-flowmath.h"
#include "sc-gpu.h"
#include "sc-timing.h"

/* ================= simulated world ================= */
#define CX 1280
#define CY 720
static double g_world_pos; /* true camera position of the frame on the "card" now */
static int g_world_content; /* id of that frame (same id = identical image) */
static int g_nvidia_sign = 1; /* which flow convention the mock driver uses */
#define MAXT 20000
#define MAXT_DECL MAXT
static bool g_parallax; /* top half of the picture moves 3x faster (depth) */
static int g_cut_content = -1; /* content id where a scene cut happens */
static int g_cross_cut; /* interpolations that blended across the cut */
static bool g_talking; /* still picture, only a small patch changes per frame (hands, mouth) */
static int g_shown_content[MAXT_DECL];
static uint64_t g_now_ns;

/* ================= mock textures ================= */
struct gs_texture {
	int content;
	double pos;
	bool is_flow;
	float flow_uv_x; /* center vector, after upload */
	int uploads;
};
struct gs_texture_render {
	struct gs_texture tex;
};
struct gs_effect {
	int dummy;
};
struct gs_effect_param {
	const char *name;
};
struct obs_source {
	int dummy;
};

static struct obs_source g_target, g_filter;
static struct gs_effect g_effect, g_default_effect;
static struct gs_texture_render *g_cur_tr; /* texrender being rendered into */
static int g_skips, g_submits, g_out_draws;

/* what the output render bound */
static struct gs_texture *g_b_image, *g_b_image0, *g_b_image1, *g_b_flow01;
static float g_b_alpha;
static const char *g_tech;
static bool g_in_out_render;

/* recorded output: position shown per tick */
static double g_shown[MAXT];
static double g_truth_at[MAXT]; /* input position seen by OBS at each tick */
static int g_nshown;
static int g_interp_draws, g_interp_bad_flow;

/* ================= OBS / graphics stubs ================= */
const char *PLUGIN_NAME = "Grouwdi-OBS-FrameGen";
const char *PLUGIN_VERSION = "test";
static char g_last_log[2048];
static char g_all_log[1 << 16]; /* every log line since the last reset */
void obs_log(int level, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vsnprintf(g_last_log, sizeof(g_last_log), fmt, a);
	va_end(a);
	size_t used = strlen(g_all_log);
	if (used + strlen(g_last_log) + 2 < sizeof(g_all_log)) {
		strcat(g_all_log, g_last_log);
		strcat(g_all_log, "\n");
	}
	(void)level;
}
void blogva(int l, const char *f, va_list a) { (void)l; (void)f; (void)a; }
void blog(int l, const char *f, ...) { (void)l; (void)f; }
uint64_t os_gettime_ns(void) { return g_now_ns; }
const char *obs_module_text(const char *k) { return k; }
obs_module_t *obs_current_module(void) { return NULL; }
char *obs_find_module_file(obs_module_t *m, const char *f) { (void)m; return bstrdup(f); }
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
gs_effect_t *gs_effect_create_from_file(const char *f, char **e) { (void)f; (void)e; return &g_effect; }
void gs_effect_destroy(gs_effect_t *e) { (void)e; }
obs_source_t *obs_filter_get_target(const obs_source_t *f) { (void)f; return &g_target; }
obs_source_t *obs_filter_get_parent(const obs_source_t *f) { (void)f; return &g_target; }
uint32_t obs_source_get_base_width(obs_source_t *s) { (void)s; return CX; }
uint32_t obs_source_get_base_height(obs_source_t *s) { (void)s; return CY; }
const char *obs_source_get_name(const obs_source_t *s) { (void)s; return "Capture Card"; }
uint32_t obs_source_get_output_flags(const obs_source_t *s) { (void)s; return OBS_SOURCE_ASYNC_VIDEO; }
enum gs_color_space obs_source_get_color_space(obs_source_t *s, size_t n, const enum gs_color_space *p)
{ (void)s; (void)n; (void)p; return GS_CS_SRGB; }
void obs_source_skip_video_filter(obs_source_t *f) { (void)f; g_skips++; }
void obs_source_default_render(obs_source_t *s) { (void)s; }
void obs_source_video_render(obs_source_t *s)
{
	(void)s;
	if (g_cur_tr) {
		g_cur_tr->tex.content = g_world_content;
		g_cur_tr->tex.pos = g_world_pos;
	}
}
int gs_get_device_type(void) { return GS_DEVICE_DIRECT3D_11; }
void *gs_get_device_obj(void) { return (void *)0x1234; }
gs_texrender_t *gs_texrender_create(enum gs_color_format f, enum gs_zstencil_format z)
{ (void)f; (void)z; return calloc(1, sizeof(struct gs_texture_render)); }
void gs_texrender_destroy(gs_texrender_t *t) { free(t); }
void gs_texrender_reset(gs_texrender_t *t) { (void)t; }
bool gs_texrender_begin_with_color_space(gs_texrender_t *t, uint32_t cx, uint32_t cy, enum gs_color_space sp)
{ (void)cx; (void)cy; (void)sp; g_cur_tr = t; return true; }
void gs_texrender_end(gs_texrender_t *t) { (void)t; g_cur_tr = NULL; }
gs_texture_t *gs_texrender_get_texture(const gs_texrender_t *t) { return t ? (gs_texture_t *)&t->tex : NULL; }
gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format f, uint32_t l, const uint8_t **d,
				uint32_t fl)
{
	(void)w; (void)h; (void)l; (void)d; (void)fl;
	struct gs_texture *t = calloc(1, sizeof(*t));
	t->is_flow = f == GS_RG32F;
	return t;
}
void gs_texture_destroy(gs_texture_t *t) { free(t); }
void *gs_texture_get_obj(gs_texture_t *t) { return t; }
static uint32_t g_ow, g_oh;
void gs_texture_set_image(gs_texture_t *t, const uint8_t *data, uint32_t linesize, bool flip)
{
	(void)flip;
	const float *f = (const float *)data;
	size_t c = (size_t)(g_oh / 2) * (linesize / sizeof(float)) + (g_ow / 2) * 2;
	t->flow_uv_x = f[c];
	t->uploads++;
}
void gs_blend_state_push(void) {}
void gs_blend_state_pop(void) {}
void gs_blend_function(enum gs_blend_type a, enum gs_blend_type b) { (void)a; (void)b; }
void gs_clear(uint32_t c, const struct vec4 *col, float d, uint8_t s) { (void)c; (void)col; (void)d; (void)s; }
void gs_ortho(float l, float r, float t, float b, float n, float f) { (void)l; (void)r; (void)t; (void)b; (void)n; (void)f; }
gs_texture_t *gs_get_render_target(void) { return NULL; }
gs_zstencil_t *gs_get_zstencil_target(void) { return NULL; }
static struct gs_texture *g_rt; /* plain render target (gray, thumbnail, comparison downscales) */
void gs_set_render_target(gs_texture_t *t, gs_zstencil_t *z) { (void)z; g_rt = t; }
void gs_viewport_push(void) {}
void gs_viewport_pop(void) {}
void gs_projection_push(void) {}
void gs_projection_pop(void) {}
void gs_matrix_push(void) {}
void gs_matrix_pop(void) {}
void gs_matrix_identity(void) {}
void gs_set_viewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static bool g_srgb;
bool gs_framebuffer_srgb_enabled(void) { return g_srgb; }
void gs_enable_framebuffer_srgb(bool e) { g_srgb = e; }
void gs_flush(void) {}
enum gs_color_space gs_get_color_space(void) { return GS_CS_SRGB; }
float obs_get_video_sdr_white_level(void) { return 300.0f; }
gs_effect_t *obs_get_base_effect(enum obs_base_effect e) { (void)e; return &g_default_effect; }
static struct gs_effect_param P_image = {"image"}, P_image0 = {"image0"}, P_image1 = {"image1"},
			      P_flow01 = {"flow01"}, P_flow10 = {"flow10"}, P_alpha = {"alpha"}, P_other = {"x"};
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n)
{
	(void)e;
	if (!strcmp(n, "image"))
		return &P_image;
	if (!strcmp(n, "image0"))
		return &P_image0;
	if (!strcmp(n, "image1"))
		return &P_image1;
	if (!strcmp(n, "flow01"))
		return &P_flow01;
	if (!strcmp(n, "flow10"))
		return &P_flow10;
	if (!strcmp(n, "alpha"))
		return &P_alpha;
	return &P_other;
}
static void bind(gs_eparam_t *p, gs_texture_t *t)
{
	if (p == &P_image)
		g_b_image = t;
	else if (p == &P_image0)
		g_b_image0 = t;
	else if (p == &P_image1)
		g_b_image1 = t;
	else if (p == &P_flow01)
		g_b_flow01 = t;
}
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *t) { bind(p, t); }
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t) { bind(p, t); }
void gs_effect_set_float(gs_eparam_t *p, float v)
{
	if (p == &P_alpha)
		g_b_alpha = v;
}
void gs_effect_set_vec2(gs_eparam_t *p, const struct vec2 *v) { (void)p; (void)v; }
static int g_loop_state;
bool gs_effect_loop(gs_effect_t *e, const char *tech)
{
	(void)e;
	g_tech = tech;
	g_loop_state = !g_loop_state;
	return g_loop_state;
}
static struct gs_texrender *g_out_tr_seen;
void gs_draw_sprite(gs_texture_t *tex, uint32_t flip, uint32_t w, uint32_t h)
{
	(void)flip; (void)w; (void)h;
	if (!g_cur_tr) {
		/* comparison downscale: a plain copy into a render target */
		if (g_rt && !strcmp(g_tech, "Draw") && g_b_image) {
			g_rt->pos = g_b_image->pos;
			g_rt->content = g_b_image->content;
		}
		return; /* final draw to screen, or the gray/thumbnail passes */
	}
	/* drawing into a texrender that is not a history slot = the output render */
	if (!strcmp(g_tech, "Interp")) {
		double a = g_b_alpha;
		g_cur_tr->tex.pos = (1 - a) * g_b_image0->pos + a * g_b_image1->pos;
		g_cur_tr->tex.content = a < 0.5 ? g_b_image0->content : g_b_image1->content;
		g_interp_draws++;
		if (g_cut_content >= 0 && g_b_image0->content < g_cut_content && g_b_image1->content >= g_cut_content)
			g_cross_cut++;
		/* flow bound must be the upload for the pair image0 -> image1:
		   uv_x = f01 = prev - cur (scene moves), in flow px / flow_w */
		double expect = (g_b_image0->pos - g_b_image1->pos) / 1280.0; /* center row = slow layer */
		if (!g_b_flow01 || g_b_flow01->uploads == 0 || fabs(g_b_flow01->flow_uv_x - expect) > 1e-4)
			g_interp_bad_flow++;
	} else if (!strcmp(g_tech, "Draw")) {
		g_cur_tr->tex.pos = g_b_image->pos;
		g_cur_tr->tex.content = g_b_image->content;
	}
	(void)tex;
}
/* properties / settings */
static bool g_cal_exists;
static long long g_cal_sign, g_cal_pending;
static int g_cal_saves;
static long long g_set_delay = 4;
static long long g_set_ghost = 1;
long long obs_data_get_int(obs_data_t *d, const char *n)
{
	(void)d;
	if (!strcmp(n, "ghost_vs_smooth"))
		return g_set_ghost;
	if (!strcmp(n, "delay_frames"))
		return g_set_delay;
	if (!strcmp(n, "flow_width"))
		return 1280;
	if (!strcmp(n, "flow_sign"))
		return g_cal_sign;
	return SC_GPU_MEDIUM;
}
void obs_data_set_default_int(obs_data_t *d, const char *n, long long v) { (void)d; (void)n; (void)v; }
obs_properties_t *obs_properties_create(void) { return NULL; }
obs_property_t *obs_properties_add_int_slider(obs_properties_t *p, const char *a, const char *b, int c, int d, int e)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; return NULL; }
void obs_property_set_long_description(obs_property_t *p, const char *s) { (void)p; (void)s; }
obs_property_t *obs_properties_add_list(obs_properties_t *p, const char *a, const char *b, enum obs_combo_type t,
					enum obs_combo_format f)
{ (void)p; (void)a; (void)b; (void)t; (void)f; return NULL; }
size_t obs_property_list_add_int(obs_property_t *p, const char *n, long long v) { (void)p; (void)n; (void)v; return 0; }
obs_property_t *obs_properties_add_text(obs_properties_t *p, const char *a, const char *b, enum obs_text_type t)
{ (void)p; (void)a; (void)b; (void)t; return NULL; }
void obs_property_text_set_info_word_wrap(obs_property_t *p, bool w) { (void)p; (void)w; }
static obs_property_clicked_t g_button_cb, g_recalib_cb;
obs_property_t *obs_properties_add_button(obs_properties_t *p, const char *n, const char *t, obs_property_clicked_t cb)
{
	(void)p; (void)t;
	if (!strcmp(n, "recalibrate"))
		g_recalib_cb = cb;
	else
		g_button_cb = cb;
	return NULL;
}
/* saved calibration file (calibration.json) */
static struct { int dummy; } g_cal_data;
obs_data_t *obs_data_create(void) { return (obs_data_t *)&g_cal_data; }
obs_data_t *obs_data_create_from_json_file(const char *p) { (void)p; return g_cal_exists ? (obs_data_t *)&g_cal_data : NULL; }
void obs_data_release(obs_data_t *d) { (void)d; }
void obs_data_set_int(obs_data_t *d, const char *n, long long v) { (void)d; if (!strcmp(n, "flow_sign")) g_cal_pending = v; }
bool obs_data_save_json_safe(obs_data_t *d, const char *f, const char *t, const char *b)
{
	(void)d; (void)f; (void)t; (void)b;
	g_cal_sign = g_cal_pending;
	g_cal_exists = true;
	g_cal_saves++;
	return true;
}
char *obs_module_get_config_path(obs_module_t *m, const char *f) { (void)m; return bstrdup(f); }
void obs_source_update_properties(obs_source_t *s) { (void)s; }
int os_mkdirs(const char *p) { (void)p; return 0; }
static char *g_written;
static size_t g_written_len;
bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path; (void)marker;
	bfree(g_written);
	g_written = bmemdup(str, len + 1);
	g_written[len] = 0;
	g_written_len = len;
	return true;
}

/* ================= mock NVIDIA GPU layer ================= */
#define FW 1280
#define FH 720
#define OW ((FW + 3) / 4)
#define OH ((FH + 3) / 4)
struct sc_gpu {
	int cur, ref;
	double pos[2]; /* position of the gray frame in each input slot */
	int content[2];
	bool sub_flow, sub_gray, submitted;
	int sub_cur, sub_ref;
	uint8_t thumb[SC_THUMB_W * SC_THUMB_H];
	uint8_t fwd[OW * 4 * OH], bwd[OW * 4 * OH];
	uint8_t gray[2][FW * FH];
};
static uint8_t g_tex_row[8192 * 2];
static void fill_raw(uint8_t *raw, double px)
{
	for (int i = 0; i < OW * OH; i++) {
		int row = i / OW;
		double v = (g_parallax && row < OH / 2) ? px * 3.0 : px;
		int16_t vx = (int16_t)lround(v * 32.0), vy = 0;
		memcpy(raw + i * 4, &vx, 2);
		memcpy(raw + i * 4 + 2, &vy, 2);
	}
}
static void fill_noise(uint8_t *raw)
{
	for (int i = 0; i < OW * OH; i++) {
		int16_t vx = (int16_t)((rand() % 200 - 100) * 32), vy = (int16_t)((rand() % 100 - 50) * 32);
		memcpy(raw + i * 4, &vx, 2);
		memcpy(raw + i * 4 + 2, &vy, 2);
	}
}
static void fill_gray(uint8_t *g, double pos, int content)
{
	for (int y = 0; y < FH; y++) {
		/* same depth layout as fill_raw: top half moves 3x */
		double p = (g_parallax && y < FH / 2) ? pos * 3.0 : pos;
		int off = (int)(lround(p) % 8192);
		for (int x = 0; x < FW; x++)
			g[y * FW + x] = g_tex_row[(x + off + y * 7) % 8192];
	}
	if (g_talking) { /* a 24x24 patch (0.06% of the picture) changes every frame */
		for (int y = 340; y < 364; y++)
			for (int x = 620; x < 644; x++)
				g[y * FW + x] = (uint8_t)(content * 53 + x + y);
	}
}
static int g_gpu_fail; /* 0 ok, else the enum sc_gpu_fail value to simulate */
struct sc_gpu *sc_gpu_create(void *d, void *gt, void *tt, uint32_t fw, uint32_t fh, enum sc_gpu_quality q, char *e,
			     size_t el, enum sc_gpu_fail *why)
{
	(void)d; (void)gt; (void)tt; (void)fw; (void)fh; (void)q;
	if (g_gpu_fail) {
		*why = (enum sc_gpu_fail)g_gpu_fail;
		snprintf(e, el, "%s", g_gpu_fail == SC_GPU_FAIL_NOT_NVIDIA ? "AMD Radeon RX 7900 XTX"
									    : "Optical flow session create failed: status 2");
		return NULL;
	}
	*why = SC_GPU_FAIL_OTHER;
	struct sc_gpu *g = calloc(1, sizeof(*g));
	g->ref = -1;
	g_ow = OW;
	g_oh = OH;
	return g;
}
void sc_gpu_destroy(struct sc_gpu *g) { free(g); }
bool sc_gpu_submit(struct sc_gpu *g, bool want_gray)
{
	g_submits++;
	g->pos[g->cur] = g_world_pos;
	g->content[g->cur] = g_world_content;
	memset(g->thumb, (uint8_t)(g_world_content * 37 + 11), sizeof(g->thumb));
	g->sub_flow = g->ref >= 0;
	g->sub_gray = want_gray && g->sub_flow;
	g->sub_cur = g->cur;
	g->sub_ref = g->ref;
	fill_gray(g->gray[1], g_world_pos, g_world_content); /* always: repeat detection */
	if (g->sub_flow) {
		double pc = g->pos[g->cur], pr = g->pos[g->ref];
		/* convention H+: in(p) = ref(p + E). fwd: in=prev ref=cur -> E = prev - cur */
		bool across_cut = g_cut_content >= 0 && g->content[g->ref] < g_cut_content &&
				  g->content[g->cur] >= g_cut_content;
		if (across_cut) { /* flow is meaningless across a scene cut */
			fill_noise(g->fwd);
			fill_noise(g->bwd);
		} else {
			fill_raw(g->fwd, g_nvidia_sign * (pr - pc));
			fill_raw(g->bwd, g_nvidia_sign * (pc - pr));
		}
		if (g->sub_gray)
			fill_gray(g->gray[0], pr, g->content[g->ref]);
	}
	g->submitted = true;
	return true;
}
bool sc_gpu_collect(struct sc_gpu *g, struct sc_gpu_result *r)
{
	memset(r, 0, sizeof(*r));
	if (!g->submitted)
		return false;
	g->submitted = false;
	r->thumb = g->thumb;
	r->has_flow = g->sub_flow;
	r->fwd = g->fwd;
	r->bwd = g->bwd;
	r->flow_pitch = OW * 4;
	r->ow = OW;
	r->oh = OH;
	r->has_gray = g->sub_gray;
	r->gray_prev = g->gray[0];
	r->gray_cur = g->gray[1];
	return true;
}
void sc_gpu_commit(struct sc_gpu *g, bool was_new)
{
	if (was_new) {
		g->ref = g->sub_cur;
		g->cur = 1 - g->sub_cur;
	} else {
		g->cur = g->sub_cur;
	}
}
void sc_gpu_drop_reference(struct sc_gpu *g)
{
	g->ref = -1;
	g->cur = 0;
}
const char *sc_gpu_error(const struct sc_gpu *g) { (void)g; return "mock"; }

extern struct obs_source_info motion_smoother_filter;

/* ================= scenarios ================= */
static int fails;
#define CHECK(c, ...)                                     \
	do {                                              \
		printf((c) ? "  ok:   " : "  FAIL: "); \
		printf(__VA_ARGS__);                      \
		printf("\n");                             \
		if (!(c))                                 \
			fails++;                          \
	} while (0)

struct scenario {
	const char *name;
	double game_fps, card_fps;
	int seconds;
	int views; /* video_render calls per tick (preview + program + vertical canvas) */
	int hide_from, hide_to; /* ticks during which the source is not rendered */
	double speed; /* px per second */
};

static double irregularity(const double *p, int n, double *worst)
{
	/* step deviation from local average speed, like the Python reference */
	double sum = 0, mx = 0, mean_speed = 0;
	int cnt = 0;
	for (int i = 40; i + 1 < n; i++)
		mean_speed += fabs(p[i + 1] - p[i]);
	mean_speed /= (n - 41);
	for (int i = 44; i + 5 < n; i++) {
		double local = 0;
		for (int j = -4; j <= 4; j++)
			local += p[i + j + 1] - p[i + j];
		local /= 9;
		double dev = fabs((p[i + 1] - p[i]) - local);
		sum += dev;
		if (dev > mx)
			mx = dev;
		cnt++;
	}
	*worst = mx / mean_speed;
	return sum / cnt / mean_speed;
}

static void run(const struct scenario *sc, double *in_worst, double *out_worst)
{
	g_cal_exists = false; /* each scenario simulates a fresh PC (its own flow convention) */
	g_cal_sign = 0;
	g_skips = g_submits = g_interp_draws = g_interp_bad_flow = g_nshown = g_cross_cut = 0;
	g_now_ns = 1000000000ULL;
	void *f = motion_smoother_filter.create(NULL, &g_filter);
	int ticks = sc->seconds * 60;
	int last_content = -1;
	for (int t = 0; t < ticks; t++) {
		g_now_ns += 16666667ULL;
		/* what the card shows at this OBS tick */
		double t_tick = t / 60.0;
		double t_card = floor(t_tick * sc->card_fps + 1e-9) / sc->card_fps;
		double t_game = floor(t_card * sc->game_fps + 1e-9) / sc->game_fps;
		int content = (int)floor(t_card * sc->game_fps + 1e-9);
		g_world_pos = t_game * sc->speed;
		g_world_content = content;
		(void)last_content;

		motion_smoother_filter.video_tick(f, 1.0f / 60.0f);
		if (t >= sc->hide_from && t < sc->hide_to)
			continue;
		for (int v = 0; v < sc->views; v++)
			motion_smoother_filter.video_render(f, NULL);

		struct msm_peek {
			int dummy;
		};
		/* read the output texrender's content: it is the one rendered last tick (draw_out binds it) */
		if (g_b_image && g_nshown < MAXT) {
			g_shown[g_nshown] = g_b_image->pos;
			g_shown_content[g_nshown] = g_b_image->content;
			g_truth_at[g_nshown] = g_world_pos;
			g_nshown++;
		}
	}
	motion_smoother_filter.destroy(f);

	int vis_ticks = ticks - (sc->hide_to - sc->hide_from);
	double iw, ow;
	/* input reference: positions OBS received, shifted by the filter delay */
	irregularity(g_truth_at, g_nshown, &iw);
	irregularity(g_shown, g_nshown, &ow);
	*in_worst = iw;
	*out_worst = ow;
	printf("[%s] submits %d (visible ticks %d), interpolated draws %d, worst step error in %.1f%% -> out %.1f%%\n",
	       sc->name, g_submits, vis_ticks, g_interp_draws, iw * 100, ow * 100);
	CHECK(g_submits == vis_ticks, "GPU work queued exactly once per visible tick (%d)", g_submits);
	CHECK(g_interp_bad_flow == 0, "every interpolation used the flow uploaded for its own frame pair (%d wrong)",
	      g_interp_bad_flow);
}

int main(void)
{
	uint32_t r = 99;
	for (int i = 0; i < 8192 * 2; i++) {
		r = r * 1103515245u + 12345u;
		g_tex_row[i] = (uint8_t)(r >> 24);
	}
	for (int pass = 0; pass < 2; pass++)
		for (int i = 1; i < 8192 * 2 - 1; i++)
			g_tex_row[i] = (uint8_t)((g_tex_row[i - 1] + 2 * g_tex_row[i] + g_tex_row[i + 1]) / 4);

	double iw, ow;
	struct scenario s119 = {"game 119 -> 60, 3 views/tick", 119, 60, 30, 3, -1, -1, 600};
	run(&s119, &iw, &ow);
	CHECK(ow < iw * 0.25, "hitch reduced by at least 4x (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	struct scenario s59 = {"game 59 -> 60 (repeat each second)", 59, 60, 30, 1, -1, -1, 600};
	run(&s59, &iw, &ow);
	CHECK(ow < iw * 0.25, "hitch reduced by at least 4x (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	struct scenario sc59 = {"card 59 -> OBS 60 (OBS re-shows)", 60, 59, 30, 1, -1, -1, 600};
	run(&sc59, &iw, &ow);
	CHECK(ow < iw * 0.25, "hitch reduced by at least 4x (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	struct scenario s60 = {"clean 60 -> 60", 60, 60, 20, 1, -1, -1, 600};
	run(&s60, &iw, &ow);
	CHECK(ow <= iw + 0.005, "clean footage stays clean (%.2f%% -> %.2f%%)", iw * 100, ow * 100);

	g_nvidia_sign = -1;
	struct scenario sneg = {"game 119 -> 60, opposite driver flow sign", 119, 60, 30, 1, -1, -1, 600};
	run(&sneg, &iw, &ow);
	CHECK(ow < iw * 0.25, "calibration handled the other sign (%.1f%% -> %.1f%%)", iw * 100, ow * 100);
	g_nvidia_sign = 1;

	struct scenario shide = {"source hidden for 2 s mid-run", 119, 60, 30, 1, 600, 720, 600};
	run(&shide, &iw, &ow);
	{
		/* judge only the part after the source is visible again (+1 s to settle) */
		int from = 600 + 60;
		double wi, wo;
		irregularity(g_truth_at + from, g_nshown - from, &wi);
		irregularity(g_shown + from, g_nshown - from, &wo);
		CHECK(wo < wi * 0.25, "recovers after being hidden (%.1f%% -> %.1f%%)", wi * 100, wo * 100);
	}

	struct scenario slong = {"10 minutes continuous (slot reuse)", 144, 60, 600, 1, -1, -1, 600};
	run(&slong, &iw, &ow);
	CHECK(ow < iw * 0.25, "still smoothing after 10 min (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	{
		/* talking head: camera still, only small hand/mouth motion, 30 fps video */
		g_talking = true;
		struct scenario st = {"talking head: still picture, small motion, 30 fps video", 30, 60, 20, 1, -1, -1, 0};
		run(&st, &iw, &ow);
		int distinct = 0, longest = 0, run_len = 0;
		for (int i = 60; i < g_nshown; i++) {
			if (g_shown_content[i] != g_shown_content[i - 1]) {
				distinct++;
				run_len = 0;
			} else if (++run_len > longest) {
				longest = run_len;
			}
		}
		int expected = (g_nshown - 60) / 2; /* 30 fps content */
		CHECK(distinct >= expected * 9 / 10, "small motion is not frozen: %d of ~%d frames shown", distinct,
		      expected);
		CHECK(longest <= 2, "no frozen stretch longer than one repeat (longest %d extra ticks)", longest);
		g_talking = false;
	}

	g_parallax = true;
	struct scenario s43 = {"game 43 fps -> 60, depth motion (timing mode)", 43, 60, 30, 1, -1, -1, 600};
	run(&s43, &iw, &ow);
	CHECK(ow < iw * 0.25, "irregular cadence smoothed (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	struct scenario s50 = {"game 50 fps -> 60, depth motion", 50, 60, 30, 1, -1, -1, 600};
	run(&s50, &iw, &ow);
	CHECK(ow < iw * 0.25, "irregular cadence smoothed (%.1f%% -> %.1f%%)", iw * 100, ow * 100);

	{
		/* "Fill missing frames only": real frames untouched, only repeats generated, no freezes */
		g_set_ghost = 3;
		g_set_delay = 3;
		struct scenario sf = {"game 50 fps -> 60, fill missing frames only, delay 3", 50, 60, 30, 1, -1, -1, 600};
		run(&sf, &iw, &ow);
		int repeats = 0, frozen = 0;
		for (int i = 1; i < g_nshown; i++) {
			if (g_truth_at[i] == g_truth_at[i - 1])
				repeats++;
			if (g_shown[i] == g_shown[i - 1])
				frozen++;
		}
		CHECK(g_interp_draws <= repeats + 2, "only repeated frames are generated (%d generated, %d repeats)",
		      g_interp_draws, repeats);
		CHECK(g_interp_draws >= repeats * 9 / 10, "every repeat is filled (%d generated, %d repeats)",
		      g_interp_draws, repeats);
		CHECK(frozen <= 2, "no frozen ticks (%d)", frozen);
		g_set_ghost = 1;
		g_set_delay = 4;
	}

	g_cut_content = 43 * 15; /* scene cut 15 s in */
	struct scenario scut = {"game 43 fps -> 60, depth motion, scene cut", 43, 60, 30, 1, -1, -1, 600};
	run(&scut, &iw, &ow);
	CHECK(g_cross_cut == 0, "no interpolation across the scene cut (%d)", g_cross_cut);
	g_cut_content = -1;
	g_parallax = false;

	{
		/* diagnostic trace: press the button, run past 60 s, check the file and replay it */
		g_parallax = true;
		g_now_ns = 1000000000ULL;
		void *f = motion_smoother_filter.create(NULL, &g_filter);
		motion_smoother_filter.get_properties(f);
		for (int t = 0; t < 3900; t++) {
			g_now_ns += 16666667ULL;
			double t_card = t / 60.0, t_game = floor(t_card * 43 + 1e-9) / 43;
			g_world_pos = t_game * 600;
			g_world_content = (int)floor(t_card * 43 + 1e-9);
			if (t == 120 && g_button_cb)
				g_button_cb(NULL, NULL, f);
			motion_smoother_filter.video_tick(f, 1.0f / 60.0f);
			motion_smoother_filter.video_render(f, NULL);
		}
		motion_smoother_filter.destroy(f);
		g_parallax = false;
		int rows = 0, mism = 0, decided = 0, warmup_mism = 0;
		if (g_written) {
			struct sct_engine e;
			sct_init(&e, 3);
			char *line = strtok(g_written, "\n");
			while (line) {
				if (line[0] >= '0' && line[0] <= '9') {
					long long tick, a, b;
					int nw, conf, cut, kind, mode;
					double gx, gy, al;
					unsigned changed;
					if (sscanf(line, "%lld,%d,%lf,%lf,%d,%d,%d,%lld,%lld,%lf,%d,%u", &tick, &nw, &gx, &gy,
						   &conf, &cut, &kind, &a, &b, &al, &mode, &changed) == 12) {
						rows++;
						sct_push(&e, tick, nw, gx, gy, conf, cut);
						if (kind) {
							struct sct_decision d = sct_decide(&e, tick - 3, tick);
							decided++;
							if ((int)d.kind != kind || d.a != a ||
							    (kind == 2 && (d.b != b || fabs(d.alpha - al) > 1e-6))) {
								if (rows > 30)
									mism++; /* replay starts with no history */
								else
									warmup_mism++;
							}
						}
					}
				}
				line = strtok(NULL, "\n");
			}
		}
		printf("  (replay warm-up differences in the first 30 rows: %d)\n", warmup_mism);
		CHECK(g_written != NULL, "trace file written after 60 s");
		CHECK(rows == 3600, "trace has 3600 rows (%d)", rows);
		CHECK(decided > 3000 && mism == 0,
		      "replaying the trace reproduces every recorded decision (%d decisions, %d mismatched)", decided, mism);
	}

	{
		/* saved calibration: calibrate once, reuse in a new filter, recalibrate on request */
		printf("\n[saved calibration]\n");
		g_cal_exists = false;
		g_cal_sign = 0;
		g_cal_saves = 0;
		int interp_first_s[2] = {0, 0};
		for (int pass = 0; pass < 2; pass++) {
			g_all_log[0] = 0;
			g_now_ns = 1000000000ULL;
			void *f = motion_smoother_filter.create(NULL, &g_filter);
			motion_smoother_filter.get_properties(f);
			for (int t = 0; t < 600; t++) {
				g_now_ns += 16666667ULL;
				double tg = floor(t / 60.0 * 50 + 1e-9) / 50;
				g_world_pos = tg * 600;
				g_world_content = 400000 + pass * 100000 + (int)floor(t / 60.0 * 50 + 1e-9);
				int before = g_interp_draws;
				motion_smoother_filter.video_tick(f, 1.0f / 60.0f);
				motion_smoother_filter.video_render(f, NULL);
				if (t < 20)
					interp_first_s[pass] += g_interp_draws > before;
			}
			motion_smoother_filter.destroy(f);
			if (pass == 0) {
				CHECK(g_cal_exists && g_cal_sign == 1 && g_cal_saves == 1,
				      "first filter calibrates and saves the flow direction once (sign %lld, saves %d)",
				      g_cal_sign, g_cal_saves);
			} else {
				CHECK(strstr(g_all_log, "loaded from saved calibration") != NULL && g_cal_saves == 1,
				      "a new filter loads the saved direction instead of calibrating again (saves %d)",
				      g_cal_saves);
			}
		}
		CHECK(interp_first_s[1] > interp_first_s[0],
		      "with the saved direction, smoothing starts sooner (generated frames in the first 20 ticks: %d calibrating, %d saved)",
		      interp_first_s[0], interp_first_s[1]);

		g_all_log[0] = 0;
		g_now_ns = 1000000000ULL;
		void *f = motion_smoother_filter.create(NULL, &g_filter);
		motion_smoother_filter.get_properties(f);
		for (int t = 0; t < 600; t++) {
			g_now_ns += 16666667ULL;
			double tg = floor(t / 60.0 * 50 + 1e-9) / 50;
			g_world_pos = tg * 600;
			g_world_content = 700000 + (int)floor(t / 60.0 * 50 + 1e-9);
			if (t == 120 && g_recalib_cb)
				g_recalib_cb(NULL, NULL, f);
			motion_smoother_filter.video_tick(f, 1.0f / 60.0f);
			motion_smoother_filter.video_render(f, NULL);
		}
		motion_smoother_filter.destroy(f);
		CHECK(g_recalib_cb != NULL && strstr(g_all_log, "recalibration requested") &&
			      strstr(g_all_log, "flow direction calibrated") && g_cal_saves == 3 && g_cal_sign == 1,
		      "Recalibrate clears the saved direction, calibrates again and saves it (saves %d, sign %lld)",
		      g_cal_saves, g_cal_sign);
	}

	for (int kind = SC_GPU_FAIL_NOT_NVIDIA; kind <= SC_GPU_FAIL_NO_OF; kind++) {
		printf("\n[%s]\n", kind == SC_GPU_FAIL_NOT_NVIDIA ? "non-NVIDIA GPU" : "NVIDIA GPU without optical flow");
		g_gpu_fail = kind;
		g_all_log[0] = 0;
		g_skips = 0;
		g_now_ns = 1000000000ULL;
		void *f = motion_smoother_filter.create(NULL, &g_filter);
		for (int t = 0; t < 120; t++) {
			g_now_ns += 16666667ULL;
			g_world_pos = t * 10.0;
			g_world_content = 900000 + t;
			motion_smoother_filter.video_tick(f, 1.0f / 60.0f);
			motion_smoother_filter.video_render(f, NULL);
		}
		motion_smoother_filter.destroy(f);
		g_gpu_fail = 0;
		const char *want = kind == SC_GPU_FAIL_NOT_NVIDIA ? "needs an NVIDIA GPU; OBS renders on AMD Radeon RX 7900 XTX"
								    : "NVIDIA optical flow unavailable";
		CHECK(strstr(g_all_log, want) != NULL, "plain message: \"%s\"", want);
		CHECK(g_skips == 120, "video passes through unchanged on every frame (%d of 120)", g_skips);
	}
	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
