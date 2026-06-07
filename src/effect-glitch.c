#include "effect-glitch.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_GLITCH_COLOR 0xFFFFFFFFu /* white #FFFFFF in OBS 0xAABBGGRR */

/* Per-instance state for the glitch effect. */
struct glitch_state {
	gs_effect_t *effect;

	uint32_t color;    /* OBS 0xAABBGGRR text tint     */
	float    rate;     /* bursts per second (0 = off)  */
	float    strength; /* glitch magnitude, 0..1       */
	float    glow;     /* emissive body boost, 0..1    */
	float    bloom;    /* soft halo spread, 0..1       */
	float    vhs;      /* analog smear/wobble, 0..1    */
};

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

/* Cheap hash of one variable, for per-slot burst randomization. */
static float hash11(float x)
{
	float s = sinf(x) * 43758.5453f;
	return s - floorf(s);
}

/* Burst envelope. Time is sliced into 1/rate-long slots; within each slot a
 * short burst may fire, with irregular start/length/strength chosen by hashing
 * the slot index. The text therefore sits mostly still and snaps into a glitch
 * every so often (≈`rate` times per second). Returns the activity in 0..1 and
 * writes the current burst's seed (used by the shader to vary the look). */
static float glitch_activity(float t, float rate, float *seed_out)
{
	*seed_out = 0.0f;
	if (rate <= 0.0f)
		return 0.0f;

	float slot = t * rate;
	float idx = floorf(slot);
	float frac = slot - idx; /* 0..1 within this slot */
	*seed_out = fmodf(idx, 1000.0f);

	/* Some slots stay calm so the glitches feel irregular. */
	if (hash11(idx * 2.3f + 7.7f) < 0.25f)
		return 0.0f;

	float start = hash11(idx * 1.1f + 0.13f) * 0.65f;
	float dur = 0.08f + hash11(idx * 1.7f + 3.10f) * 0.20f; /* 8%..28% */
	float peak = hash11(idx * 3.1f + 5.20f);

	float x = (frac - start) / dur;
	if (x < 0.0f || x > 1.0f)
		return 0.0f;

	float env = sinf(x * 3.14159265f); /* smooth 0 -> 1 -> 0 */
	/* A touch of flicker within the burst. */
	env *= 0.85f + 0.15f * hash11(floorf(frac * 37.0f) + idx);
	return env * (0.55f + 0.45f * peak);
}

static void *glitch_create(void)
{
	return bzalloc(sizeof(struct glitch_state));
}

static void glitch_destroy(void *data)
{
	struct glitch_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void glitch_load_graphics(void *data)
{
	struct glitch_state *s = data;
	char *path = obs_module_file("effects/glitch.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load glitch.effect (%s)",
				path);
	}
	bfree(path);
}

static void glitch_update(void *data, obs_data_t *settings)
{
	struct glitch_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "glitch_color");
	s->rate = (float)obs_data_get_double(settings, "glitch_rate");
	s->strength = (float)obs_data_get_double(settings, "glitch_strength");
	s->glow = (float)obs_data_get_double(settings, "glitch_glow");
	s->bloom = (float)obs_data_get_double(settings, "glitch_bloom");
	s->vhs = (float)obs_data_get_double(settings, "glitch_vhs");
}

static void glitch_render(void *data, const struct fx_render_ctx *ctx)
{
	struct glitch_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "glitch_color");
	gs_eparam_t *p_intensity = gs_effect_get_param_by_name(e, "intensity");
	gs_eparam_t *p_strength = gs_effect_get_param_by_name(e, "strength");
	gs_eparam_t *p_seed = gs_effect_get_param_by_name(e, "seed");
	gs_eparam_t *p_time = gs_effect_get_param_by_name(e, "time");
	gs_eparam_t *p_glow = gs_effect_get_param_by_name(e, "glow");
	gs_eparam_t *p_bloom = gs_effect_get_param_by_name(e, "bloom");
	gs_eparam_t *p_vhs = gs_effect_get_param_by_name(e, "vhs");

	float seed = 0.0f;
	float intensity = glitch_activity(ctx->time, s->rate, &seed);

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_canvas) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p_canvas, &c);
	}
	if (p_color) {
		float rgba[4];
		unpack_color(s->color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p_color, &col);
	}
	if (p_intensity)
		gs_effect_set_float(p_intensity, intensity);
	if (p_strength)
		gs_effect_set_float(p_strength, s->strength);
	if (p_seed)
		gs_effect_set_float(p_seed, seed);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(ctx->time, 1000.0f));
	if (p_glow)
		gs_effect_set_float(p_glow, s->glow);
	if (p_bloom)
		gs_effect_set_float(p_bloom, s->bloom);
	if (p_vhs)
		gs_effect_set_float(p_vhs, s->vhs);

	/* Premultiplied output: glow/bloom add light over the background. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void glitch_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "glitch_color",
		obs_module_text("GlitchColor"));
	obs_properties_add_float_slider(p, "glitch_rate",
		obs_module_text("GlitchRate"), 0.0, 10.0, 0.1);
	obs_properties_add_float_slider(p, "glitch_strength",
		obs_module_text("GlitchStrength"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "glitch_glow",
		obs_module_text("GlitchGlow"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "glitch_bloom",
		obs_module_text("GlitchBloom"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "glitch_vhs",
		obs_module_text("GlitchVhs"), 0.0, 1.0, 0.05);
}

static void glitch_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "glitch_color", DEFAULT_GLITCH_COLOR);
	obs_data_set_default_double(settings, "glitch_rate", 2.0);
	obs_data_set_default_double(settings, "glitch_strength", 0.5);
	obs_data_set_default_double(settings, "glitch_glow", 0.3);
	obs_data_set_default_double(settings, "glitch_bloom", 0.3);
	obs_data_set_default_double(settings, "glitch_vhs", 0.25);
}

const struct text_effect fx_glitch = {
	.id             = "glitch",
	.name_key       = "EffectGlitch",
	.create         = glitch_create,
	.destroy        = glitch_destroy,
	.load_graphics  = glitch_load_graphics,
	.update         = glitch_update,
	.render         = glitch_render,
	.get_properties = glitch_properties,
	.get_defaults   = glitch_defaults,
};
