#include "effect-bloom.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_BLOOM_COLOR 0xFF7FD2FFu /* warm #FFD27F in OBS 0xAABBGGRR */

/* Per-instance state for the bloom (glowing solid text) effect. */
struct bloom_state {
	gs_effect_t *effect;

	uint32_t color;     /* OBS 0xAABBGGRR fill/glow tint    */
	float    radius;    /* bloom halo spread, pixels        */
	float    intensity; /* overall luminance multiplier     */
	float    pulse;     /* random strength variation, 0..1  */
	float    shimmer;   /* spatial shimmer amount, 0..1     */
};

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static inline float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* Cheap value noise of one variable, for the random strength variation. */
static float hash11(float x)
{
	float s = sinf(x) * 43758.5453f;
	return s - floorf(s);
}

static float vnoise1(float x)
{
	float i = floorf(x);
	float f = x - i;
	float u = f * f * (3.0f - 2.0f * f);
	return lerpf(hash11(i), hash11(i + 1.0f), u);
}

/* Random bloom strength: a multi-octave smooth-noise wander centered on 1.0.
 * `amount` scales how far the strength swings (0 = steady). */
static float bloom_pulse(float t, float amount)
{
	if (amount <= 0.0f)
		return 1.0f;

	/* Weighted octaves give an organic, irregular ebb and flow. */
	float n = 0.55f * vnoise1(t * 0.7f) +
		  0.30f * vnoise1(t * 1.9f + 5.0f) +
		  0.15f * vnoise1(t * 4.3f + 11.0f);

	/* n is ~0..1; map to a multiplier centered on 1.0, spanning 1 +/- amount. */
	return 1.0f + amount * (n - 0.5f) * 2.0f;
}

static void *bloom_create(void)
{
	return bzalloc(sizeof(struct bloom_state));
}

static void bloom_destroy(void *data)
{
	struct bloom_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void bloom_load_graphics(void *data)
{
	struct bloom_state *s = data;
	char *path = obs_module_file("effects/bloom.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load bloom.effect (%s)",
				path);
	}
	bfree(path);
}

static void bloom_update(void *data, obs_data_t *settings)
{
	struct bloom_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "bloom_color");
	s->radius = (float)obs_data_get_double(settings, "bloom_radius");
	s->intensity = (float)obs_data_get_double(settings, "bloom_intensity");
	s->pulse = (float)obs_data_get_double(settings, "bloom_pulse");
	s->shimmer = (float)obs_data_get_double(settings, "bloom_shimmer");
}

static void bloom_render(void *data, const struct fx_render_ctx *ctx)
{
	struct bloom_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "bloom_color");
	gs_eparam_t *p_radius = gs_effect_get_param_by_name(e, "radius");
	gs_eparam_t *p_intensity = gs_effect_get_param_by_name(e, "intensity");
	gs_eparam_t *p_pulse = gs_effect_get_param_by_name(e, "pulse");
	gs_eparam_t *p_shimmer = gs_effect_get_param_by_name(e, "shimmer");
	gs_eparam_t *p_time = gs_effect_get_param_by_name(e, "time");

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
	if (p_radius)
		gs_effect_set_float(p_radius, s->radius);
	if (p_intensity)
		gs_effect_set_float(p_intensity, s->intensity);
	if (p_pulse)
		gs_effect_set_float(p_pulse, bloom_pulse(ctx->time, s->pulse));
	if (p_shimmer)
		gs_effect_set_float(p_shimmer, s->shimmer);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(ctx->time, 1000.0f));

	/* Premultiplied output: bright cores/halo add light over the background. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void bloom_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "bloom_color",
		obs_module_text("BloomColor"));
	obs_properties_add_float_slider(p, "bloom_radius",
		obs_module_text("BloomRadius"), 2.0, 60.0, 0.5);
	obs_properties_add_float_slider(p, "bloom_intensity",
		obs_module_text("BloomIntensity"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "bloom_pulse",
		obs_module_text("BloomPulse"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "bloom_shimmer",
		obs_module_text("BloomShimmer"), 0.0, 1.0, 0.05);
}

static void bloom_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bloom_color", DEFAULT_BLOOM_COLOR);
	obs_data_set_default_double(settings, "bloom_radius", 20.0);
	obs_data_set_default_double(settings, "bloom_intensity", 1.0);
	obs_data_set_default_double(settings, "bloom_pulse", 0.5);
	obs_data_set_default_double(settings, "bloom_shimmer", 0.3);
}

const struct text_effect fx_bloom = {
	.id             = "bloom",
	.name_key       = "EffectBloom",
	.create         = bloom_create,
	.destroy        = bloom_destroy,
	.load_graphics  = bloom_load_graphics,
	.update         = bloom_update,
	.render         = bloom_render,
	.get_properties = bloom_properties,
	.get_defaults   = bloom_defaults,
};
