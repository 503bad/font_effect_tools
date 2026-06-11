#include "effect-slice.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SLC_FONT 0xFFFFFFFFu      /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */
#define SLC_PI 3.14159265f

struct slice_state {
	gs_effect_t *effect;

	uint32_t font_color;
	float    angle; /* cut angle in degrees (0 = horizontal) */
	int      bands; /* number of slices                      */
	float    speed;
	float    dist;  /* slide-in travel distance in px        */
	float    glow_amt;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;

	bool  loop;
	float wait;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *slice_create(void)
{
	return bzalloc(sizeof(struct slice_state));
}

static void slice_destroy(void *data)
{
	struct slice_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void slice_load_graphics(void *data)
{
	struct slice_state *s = data;
	char *path = obs_module_file("effects/slice.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load slice.effect (%s)",
				path);
	}
	bfree(path);
}

static void slice_update(void *data, obs_data_t *settings)
{
	struct slice_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "slc_font");
	s->angle = (float)obs_data_get_double(settings, "slc_angle");
	s->bands = (int)obs_data_get_int(settings, "slc_count");
	s->speed = (float)obs_data_get_double(settings, "slc_speed");
	s->dist = (float)obs_data_get_double(settings, "slc_dist");
	s->glow_amt = (float)obs_data_get_double(settings, "slc_glow");
	s->outline_on = obs_data_get_bool(settings, "slc_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "slc_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "slc_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "slc_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "slc_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "slc_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "slc_shadow_y");
	s->loop = obs_data_get_bool(settings, "slc_loop");
	s->wait = (float)obs_data_get_double(settings, "slc_wait");
}

static void set_vec2(gs_effect_t *e, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

static void set_vec4c(gs_effect_t *e, const char *name, const float rgba[4])
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec4 v;
		vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &v);
	}
}

static void set_float(gs_effect_t *e, const char *name, float f)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, f);
}

static void slice_render(void *data, const struct fx_render_ctx *ctx)
{
	struct slice_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	gs_effect_t *e = s->effect;

	/* Timeline: slide in -> hold `wait` -> slide back out -> repeat.
	 * Loop off enters once and stays assembled. */
	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.9f / sp;
	float hold_end = enter_dur + s->wait;
	float cycle = hold_end + enter_dur + 0.3f;
	float lt = s->loop ? fmodf(ctx->time, cycle)
			   : fminf(ctx->time, enter_dur);

	float shift;
	if (lt < enter_dur) {
		float p = lt / enter_dur;
		float ease = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
		shift = s->dist * (1.0f - ease);
	} else if (lt < hold_end) {
		shift = 0.0f;
	} else {
		float p = (lt - hold_end) / enter_dur;
		if (p > 1.0f)
			p = 1.0f;
		shift = s->dist * p * p * p;
	}

	/* Cut geometry: a line at `angle` through the text centre. Bands are
	 * measured along the normal across the text bounding box. */
	float arad = s->angle * SLC_PI / 180.0f;
	float dx = cosf(arad), dy = -sinf(arad); /* canvas y grows down */
	float nx = sinf(arad), ny = cosf(arad);
	float cx = (mask->text_left + mask->text_right) * 0.5f;
	float cy = (mask->text_top + mask->text_bottom) * 0.5f;
	float hw = (mask->text_right - mask->text_left) * 0.5f;
	float hh = (mask->text_bottom - mask->text_top) * 0.5f;
	float extent = fabsf(hw * nx) + fabsf(hh * ny);
	if (extent < 1.0f)
		extent = 1.0f;

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);

	gs_eparam_t *pi = gs_effect_get_param_by_name(e, "image");
	if (pi)
		gs_effect_set_texture(pi, mask->tex);
	set_vec2(e, "canvas", (float)ctx->width, (float)ctx->height);
	set_vec4c(e, "font_color", rgba);
	set_vec2(e, "center", cx, cy);
	set_vec2(e, "dirv", dx, dy);
	set_vec2(e, "nrm", nx, ny);
	set_float(e, "extent", extent);
	set_float(e, "bands", (float)(s->bands < 2 ? 2 : s->bands));
	set_float(e, "shift", shift);
	set_float(e, "outline_on", s->outline_on ? 1.0f : 0.0f);
	set_float(e, "outline_width", s->outline_width);
	set_vec4c(e, "outline_color", orgba);
	set_float(e, "glow", s->glow_amt);
	set_float(e, "shadow_on",
		  (s->shadow_on && shrgba[3] > 0.0f) ? 1.0f : 0.0f);
	set_vec4c(e, "shadow_color", shrgba);
	set_vec2(e, "shadow_off", (float)s->shadow_x, (float)s->shadow_y);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void slice_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "slc_font",
		obs_module_text("SlcFontColor"));
	obs_properties_add_float_slider(p, "slc_angle",
		obs_module_text("SlcAngle"), 0.0, 360.0, 1.0);
	obs_properties_add_int_slider(p, "slc_count",
		obs_module_text("SlcCount"), 2, 12, 1);
	obs_properties_add_float_slider(p, "slc_speed",
		obs_module_text("SlcSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "slc_dist",
		obs_module_text("SlcDist"), 50.0, 2000.0, 1.0);
	obs_properties_add_float_slider(p, "slc_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "slc_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "slc_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "slc_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "slc_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "slc_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "slc_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "slc_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
	obs_properties_add_bool(p, "slc_loop", obs_module_text("LoopOn"));
	obs_properties_add_float_slider(p, "slc_wait",
		obs_module_text("LoopWait"), 0.0, 10.0, 0.1);
}

static void slice_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "slc_font", DEFAULT_SLC_FONT);
	obs_data_set_default_double(settings, "slc_angle", 25.0);
	obs_data_set_default_int(settings, "slc_count", 2);
	obs_data_set_default_double(settings, "slc_speed", 1.0);
	obs_data_set_default_double(settings, "slc_dist", 400.0);
	obs_data_set_default_double(settings, "slc_glow", 0.0);
	obs_data_set_default_bool(settings, "slc_outline", false);
	obs_data_set_default_double(settings, "slc_outline_width", 4.0);
	obs_data_set_default_int(settings, "slc_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "slc_shadow", false);
	obs_data_set_default_int(settings, "slc_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "slc_shadow_x", 4);
	obs_data_set_default_int(settings, "slc_shadow_y", 4);
	obs_data_set_default_bool(settings, "slc_loop", true);
	obs_data_set_default_double(settings, "slc_wait", 1.5);
}

const struct text_effect fx_slice = {
	.id             = "slice",
	.name_key       = "EffectSlice",
	.create         = slice_create,
	.destroy        = slice_destroy,
	.load_graphics  = slice_load_graphics,
	.update         = slice_update,
	.render         = slice_render,
	.get_properties = slice_properties,
	.get_defaults   = slice_defaults,
};
