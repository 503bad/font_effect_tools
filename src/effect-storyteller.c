#include "effect-storyteller.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_ST_COLOR 0xFF6BD8FFu /* pale gold #FFD86B */
#define ST_HORIZON 0.12f

struct storyteller_state {
	gs_effect_t *effect;

	uint32_t color;
	float    speed;
	bool     fade;
	bool     loop;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *storyteller_create(void)
{
	return bzalloc(sizeof(struct storyteller_state));
}

static void storyteller_destroy(void *data)
{
	struct storyteller_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void storyteller_load_graphics(void *data)
{
	struct storyteller_state *s = data;
	char *path = obs_module_file("effects/storyteller.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR,
				"failed to load storyteller.effect (%s)", path);
	}
	bfree(path);
}

static void storyteller_update(void *data, obs_data_t *settings)
{
	struct storyteller_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "st_color");
	s->speed = (float)obs_data_get_double(settings, "st_speed");
	s->fade = obs_data_get_bool(settings, "st_fade");
	s->loop = obs_data_get_bool(settings, "st_loop");
}

static void storyteller_render(void *data, const struct fx_render_ctx *ctx)
{
	struct storyteller_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	if (mask->width == 0 || mask->height == 0)
		return;

	float w = (float)mask->width, h = (float)mask->height;
	float left_n = mask->text_left / w;
	float right_n = mask->text_right / w;
	float top_n = mask->text_top / h;
	float bottom_n = mask->text_bottom / h;
	float bandH = bottom_n - top_n;
	if (bandH < 0.001f)
		return;

	float density = bandH * 0.45f;
	float period = bandH * 1.8f;
	float vel = s->speed * bandH * 0.6f;
	float scroll = ctx->time * vel;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, mask->tex);
	if ((p = gs_effect_get_param_by_name(e, "band"))) {
		struct vec4 b;
		vec4_set(&b, left_n, right_n, top_n, bottom_n);
		gs_effect_set_vec4(p, &b);
	}
	if ((p = gs_effect_get_param_by_name(e, "color"))) {
		float c[4];
		unpack_color(s->color, c);
		struct vec4 cc;
		vec4_set(&cc, c[0], c[1], c[2], c[3]);
		gs_effect_set_vec4(p, &cc);
	}
	if ((p = gs_effect_get_param_by_name(e, "horizon")))
		gs_effect_set_float(p, ST_HORIZON);
	if ((p = gs_effect_get_param_by_name(e, "scroll")))
		gs_effect_set_float(p, scroll);
	if ((p = gs_effect_get_param_by_name(e, "density")))
		gs_effect_set_float(p, density);
	if ((p = gs_effect_get_param_by_name(e, "period")))
		gs_effect_set_float(p, period);
	if ((p = gs_effect_get_param_by_name(e, "loopf")))
		gs_effect_set_float(p, s->loop ? 1.0f : 0.0f);
	if ((p = gs_effect_get_param_by_name(e, "fadef")))
		gs_effect_set_float(p, s->fade ? 1.0f : 0.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void storyteller_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "st_color",
		obs_module_text("StColor"));
	obs_properties_add_float_slider(p, "st_speed",
		obs_module_text("StSpeed"), 0.05, 1.5, 0.01);
	obs_properties_add_bool(p, "st_fade", obs_module_text("StFade"));
	obs_properties_add_bool(p, "st_loop", obs_module_text("LoopOn"));
}

static void storyteller_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "st_color", DEFAULT_ST_COLOR);
	obs_data_set_default_double(settings, "st_speed", 0.3);
	obs_data_set_default_bool(settings, "st_fade", true);
	obs_data_set_default_bool(settings, "st_loop", true);
}

const struct text_effect fx_storyteller = {
	.id             = "storyteller",
	.name_key       = "EffectStoryteller",
	.create         = storyteller_create,
	.destroy        = storyteller_destroy,
	.load_graphics  = storyteller_load_graphics,
	.update         = storyteller_update,
	.render         = storyteller_render,
	.get_properties = storyteller_properties,
	.get_defaults   = storyteller_defaults,
};
