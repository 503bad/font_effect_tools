#include "effect-chroma.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_CHROMA_FONT 0xFFFFFFFFu /* white #FFFFFF */

/* Per-instance state for the chromatic glow effect. */
struct chroma_state {
	gs_effect_t *effect;

	uint32_t font_color; /* base text fill (OBS 0xAABBGGRR) */
	float    intensity;  /* hue tint / glow strength        */
	float    speed;      /* hue rotation speed              */
	float    split;      /* RGB split distance (px)         */
	float    glow;       /* halo strength                   */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *chroma_create(void)
{
	return bzalloc(sizeof(struct chroma_state));
}

static void chroma_destroy(void *data)
{
	struct chroma_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void chroma_load_graphics(void *data)
{
	struct chroma_state *s = data;
	char *path = obs_module_file("effects/chroma.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load chroma.effect (%s)",
				path);
	}
	bfree(path);
}

static void chroma_update(void *data, obs_data_t *settings)
{
	struct chroma_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "chroma_color");
	s->intensity = (float)obs_data_get_double(settings, "chroma_intensity");
	s->speed = (float)obs_data_get_double(settings, "chroma_speed");
	s->split = (float)obs_data_get_double(settings, "chroma_split");
	s->glow = (float)obs_data_get_double(settings, "chroma_glow");
}

static void chroma_render(void *data, const struct fx_render_ctx *ctx)
{
	struct chroma_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "font_color");

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_canvas) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p_canvas, &c);
	}
	if (p_color) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p_color, &col);
	}

	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "time")))
		gs_effect_set_float(p, fmodf(ctx->time, 1000.0f));
	if ((p = gs_effect_get_param_by_name(e, "intensity")))
		gs_effect_set_float(p, s->intensity);
	if ((p = gs_effect_get_param_by_name(e, "speed")))
		gs_effect_set_float(p, s->speed);
	if ((p = gs_effect_get_param_by_name(e, "split")))
		gs_effect_set_float(p, s->split);
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, s->glow);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void chroma_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "chroma_color",
		obs_module_text("ChromaColor"));
	obs_properties_add_float_slider(p, "chroma_intensity",
		obs_module_text("ChromaIntensity"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "chroma_speed",
		obs_module_text("ChromaSpeed"), 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(p, "chroma_split",
		obs_module_text("ChromaSplit"), 0.0, 12.0, 0.1);
	obs_properties_add_float_slider(p, "chroma_glow",
		obs_module_text("ChromaGlow"), 0.0, 3.0, 0.05);
}

static void chroma_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "chroma_color", DEFAULT_CHROMA_FONT);
	obs_data_set_default_double(settings, "chroma_intensity", 0.7);
	obs_data_set_default_double(settings, "chroma_speed", 0.5);
	obs_data_set_default_double(settings, "chroma_split", 3.0);
	obs_data_set_default_double(settings, "chroma_glow", 1.0);
}

const struct text_effect fx_chroma = {
	.id             = "chroma",
	.name_key       = "EffectChroma",
	.create         = chroma_create,
	.destroy        = chroma_destroy,
	.load_graphics  = chroma_load_graphics,
	.update         = chroma_update,
	.render         = chroma_render,
	.get_properties = chroma_properties,
	.get_defaults   = chroma_defaults,
};
