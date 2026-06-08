#include "effect-circuit.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_CIRC_FONT  0xFF202020u /* dim base #202020      */
#define DEFAULT_CIRC_LIGHT 0xFF66FF66u /* bright green #66FF66  */

/* Per-instance state for the current-circuit effect. */
struct circuit_state {
	gs_effect_t *effect;

	uint32_t font_color;  /* base text fill (OBS 0xAABBGGRR) */
	uint32_t light_color; /* travelling pulse tint           */
	float    rate;        /* pulse travel speed              */
	float    random;      /* dash jitter randomness          */
	float    glow;        /* pulse brightness                */
	float    bloom;       /* halo strength                   */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *circuit_create(void)
{
	return bzalloc(sizeof(struct circuit_state));
}

static void circuit_destroy(void *data)
{
	struct circuit_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void circuit_load_graphics(void *data)
{
	struct circuit_state *s = data;
	char *path = obs_module_file("effects/circuit.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR,
				"failed to load circuit.effect (%s)", path);
	}
	bfree(path);
}

static void circuit_update(void *data, obs_data_t *settings)
{
	struct circuit_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "circ_color");
	s->light_color = (uint32_t)obs_data_get_int(settings, "circ_light");
	s->rate = (float)obs_data_get_double(settings, "circ_rate");
	s->random = (float)obs_data_get_double(settings, "circ_random");
	s->glow = (float)obs_data_get_double(settings, "circ_glow");
	s->bloom = (float)obs_data_get_double(settings, "circ_bloom");
}

static void circuit_render(void *data, const struct fx_render_ctx *ctx)
{
	struct circuit_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p;

	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, mask->tex);
	if ((p = gs_effect_get_param_by_name(e, "canvas"))) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "font_color"))) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &col);
	}
	if ((p = gs_effect_get_param_by_name(e, "light_color"))) {
		float rgba[4];
		unpack_color(s->light_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &col);
	}
	if ((p = gs_effect_get_param_by_name(e, "time")))
		gs_effect_set_float(p, fmodf(ctx->time, 1000.0f));
	if ((p = gs_effect_get_param_by_name(e, "rate")))
		gs_effect_set_float(p, s->rate);
	if ((p = gs_effect_get_param_by_name(e, "random")))
		gs_effect_set_float(p, s->random);
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, s->glow);
	if ((p = gs_effect_get_param_by_name(e, "bloom")))
		gs_effect_set_float(p, s->bloom);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void circuit_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "circ_color",
		obs_module_text("CircColor"));
	obs_properties_add_color_alpha(p, "circ_light",
		obs_module_text("CircLight"));
	obs_properties_add_float_slider(p, "circ_rate",
		obs_module_text("CircRate"), 0.0, 4.0, 0.02);
	obs_properties_add_float_slider(p, "circ_random",
		obs_module_text("CircRandom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "circ_glow",
		obs_module_text("CircGlow"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "circ_bloom",
		obs_module_text("CircBloom"), 0.0, 3.0, 0.05);
}

static void circuit_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "circ_color", DEFAULT_CIRC_FONT);
	obs_data_set_default_int(settings, "circ_light", DEFAULT_CIRC_LIGHT);
	obs_data_set_default_double(settings, "circ_rate", 1.2);
	obs_data_set_default_double(settings, "circ_random", 0.4);
	obs_data_set_default_double(settings, "circ_glow", 1.3);
	obs_data_set_default_double(settings, "circ_bloom", 1.0);
}

const struct text_effect fx_circuit = {
	.id             = "circuit",
	.name_key       = "EffectCircuit",
	.create         = circuit_create,
	.destroy        = circuit_destroy,
	.load_graphics  = circuit_load_graphics,
	.update         = circuit_update,
	.render         = circuit_render,
	.get_properties = circuit_properties,
	.get_defaults   = circuit_defaults,
};
