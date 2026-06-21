#include "effect-cartoon.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_CTN_COLOR 0xFFFFFFFFu /* white #FFFFFF */

struct cartoon_state {
	gs_effect_t *effect;

	uint32_t color;
	float    period;   /* seconds between boil "frames" */
	float    strength; /* 0..1 distortion amount        */
	float    bleed;    /* 0..1 ink bleed                */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static float cartoon_amp_px(const struct cartoon_state *s)
{
	return s->strength * 24.0f;
}

static void *cartoon_create(void)
{
	return bzalloc(sizeof(struct cartoon_state));
}

static void cartoon_destroy(void *data)
{
	struct cartoon_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void cartoon_load_graphics(void *data)
{
	struct cartoon_state *s = data;
	char *path = obs_module_file("effects/cartoon.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load cartoon.effect (%s)",
				path);
	}
	bfree(path);
}

static void cartoon_update(void *data, obs_data_t *settings)
{
	struct cartoon_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "ctn_color");
	s->period = (float)obs_data_get_double(settings, "ctn_period");
	s->strength = (float)obs_data_get_double(settings, "ctn_strength");
	s->bleed = (float)obs_data_get_double(settings, "ctn_bleed");
}

/* Reserve room so the warp + ink bleed are not clipped by the canvas edge. */
static void cartoon_margins(void *data, uint32_t font_size,
			    struct fx_margins *out)
{
	struct cartoon_state *s = data;
	UNUSED_PARAMETER(font_size);
	uint32_t m = (uint32_t)(cartoon_amp_px(s) + s->bleed * 14.0f + 8.0f);
	out->left = out->right = out->top = out->bottom = m;
}

static void cartoon_render(void *data, const struct fx_render_ctx *ctx)
{
	struct cartoon_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	if (mask->width == 0 || mask->height == 0)
		return;

	float period = s->period < 0.01f ? 0.01f : s->period;
	float step_seed = fmodf(floorf(ctx->time / period), 1024.0f);
	float freq = (float)mask->width / 28.0f;
	if (freq < 1.0f)
		freq = 1.0f;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, mask->tex);
	if ((p = gs_effect_get_param_by_name(e, "color"))) {
		float c[4];
		unpack_color(s->color, c);
		struct vec4 cc;
		vec4_set(&cc, c[0], c[1], c[2], c[3]);
		gs_effect_set_vec4(p, &cc);
	}
	if ((p = gs_effect_get_param_by_name(e, "texel"))) {
		struct vec2 t;
		vec2_set(&t, 1.0f / (float)mask->width,
			 1.0f / (float)mask->height);
		gs_effect_set_vec2(p, &t);
	}
	if ((p = gs_effect_get_param_by_name(e, "step_seed")))
		gs_effect_set_float(p, step_seed);
	if ((p = gs_effect_get_param_by_name(e, "freq")))
		gs_effect_set_float(p, freq);
	if ((p = gs_effect_get_param_by_name(e, "amp")))
		gs_effect_set_float(p, cartoon_amp_px(s));
	if ((p = gs_effect_get_param_by_name(e, "bleed")))
		gs_effect_set_float(p, s->bleed);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void cartoon_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "ctn_color",
		obs_module_text("CtnColor"));
	obs_properties_add_float_slider(p, "ctn_period",
		obs_module_text("CtnPeriod"), 0.03, 0.5, 0.01);
	obs_properties_add_float_slider(p, "ctn_strength",
		obs_module_text("CtnStrength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "ctn_bleed",
		obs_module_text("CtnBleed"), 0.0, 1.0, 0.01);
}

static void cartoon_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "ctn_color", DEFAULT_CTN_COLOR);
	obs_data_set_default_double(settings, "ctn_period", 0.12);
	obs_data_set_default_double(settings, "ctn_strength", 0.35);
	obs_data_set_default_double(settings, "ctn_bleed", 0.3);
}

const struct text_effect fx_cartoon = {
	.id             = "cartoon",
	.name_key       = "EffectCartoon",
	.create         = cartoon_create,
	.destroy        = cartoon_destroy,
	.load_graphics  = cartoon_load_graphics,
	.update         = cartoon_update,
	.wanted_margins = cartoon_margins,
	.render         = cartoon_render,
	.get_properties = cartoon_properties,
	.get_defaults   = cartoon_defaults,
};
