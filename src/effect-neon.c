#include "effect-neon.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_NEON_COLOR 0xFFFFE500u /* cyan #00E5FF in OBS 0xAABBGGRR */

/* Per-instance state for the neon-tube effect. */
struct neon_state {
	gs_effect_t *effect;

	uint32_t color;        /* OBS 0xAABBGGRR tube/glow tint */
	float    tube_width;   /* tube half-width, pixels       */
	float    glow_radius;  /* bloom spread, pixels          */
	float    bloom;        /* halo strength                 */
	float    brightness;   /* overall luminance multiplier  */
	float    flicker_rate; /* flicker frequency (0 = off)   */
	bool     outline;      /* draw the tube line (else glow only) */
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

static inline float smoothstep01(float e0, float e1, float x)
{
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

/* Cheap value noise of one variable for the irregular flicker component. */
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

/* Combined flicker: a regular gentle pulse (brief intensify, slow return)
 * layered with an irregular neon "buzz" plus rare dropouts. Returns a
 * multiplier centered around 1.0. */
static float neon_flicker(float t, float rate)
{
	if (rate <= 0.0f)
		return 1.0f;

	/* Regular pulse: spikes at each period start, decays smoothly. */
	float phase = t * rate;
	float fp = phase - floorf(phase);
	float pulse = expf(-fp * 5.0f);
	float regular = 1.0f + 0.5f * pulse;

	/* Irregular buzz: fast low-amplitude shimmer. */
	float buzz = 0.94f + 0.06f * vnoise1(t * 11.0f);

	/* Rare dropout: occasionally dim, like a failing tube. */
	float drop = smoothstep01(0.06f, 0.18f, vnoise1(t * 2.3f + 17.0f));
	buzz *= 0.6f + 0.4f * drop;

	return regular * buzz;
}

static void *neon_create(void)
{
	return bzalloc(sizeof(struct neon_state));
}

static void neon_destroy(void *data)
{
	struct neon_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void neon_load_graphics(void *data)
{
	struct neon_state *s = data;
	char *path = obs_module_file("effects/neon.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load neon.effect (%s)",
				path);
	}
	bfree(path);
}

static void neon_update(void *data, obs_data_t *settings)
{
	struct neon_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "neon_color");
	s->tube_width = (float)obs_data_get_double(settings, "neon_width");
	s->glow_radius = (float)obs_data_get_double(settings, "neon_glow");
	s->bloom = (float)obs_data_get_double(settings, "neon_bloom");
	s->brightness = (float)obs_data_get_double(settings, "neon_brightness");
	s->flicker_rate = (float)obs_data_get_double(settings, "neon_flicker_rate");
	s->outline = obs_data_get_bool(settings, "neon_outline");
}

static void neon_render(void *data, const struct fx_render_ctx *ctx)
{
	struct neon_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "neon_color");
	gs_eparam_t *p_width = gs_effect_get_param_by_name(e, "tube_width");
	gs_eparam_t *p_glow = gs_effect_get_param_by_name(e, "glow_radius");
	gs_eparam_t *p_bloom = gs_effect_get_param_by_name(e, "bloom");
	gs_eparam_t *p_bri = gs_effect_get_param_by_name(e, "brightness");
	gs_eparam_t *p_flick = gs_effect_get_param_by_name(e, "flicker");
	gs_eparam_t *p_outline = gs_effect_get_param_by_name(e, "outline");

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
	if (p_width)
		gs_effect_set_float(p_width, s->tube_width);
	if (p_glow)
		gs_effect_set_float(p_glow, s->glow_radius);
	if (p_bloom)
		gs_effect_set_float(p_bloom, s->bloom);
	if (p_bri)
		gs_effect_set_float(p_bri, s->brightness);
	if (p_flick)
		gs_effect_set_float(p_flick,
				    neon_flicker(ctx->time, s->flicker_rate));
	if (p_outline)
		gs_effect_set_float(p_outline, s->outline ? 1.0f : 0.0f);

	/* Premultiplied output: bright cores add light over the background. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void neon_properties(obs_properties_t *p)
{
	obs_properties_add_bool(p, "neon_outline",
		obs_module_text("NeonOutline"));
	obs_properties_add_color_alpha(p, "neon_color",
		obs_module_text("NeonColor"));
	obs_properties_add_float_slider(p, "neon_width",
		obs_module_text("NeonWidth"), 1.0, 12.0, 0.5);
	obs_properties_add_float_slider(p, "neon_glow",
		obs_module_text("NeonGlow"), 2.0, 40.0, 0.5);
	obs_properties_add_float_slider(p, "neon_bloom",
		obs_module_text("NeonBloom"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "neon_brightness",
		obs_module_text("NeonBrightness"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "neon_flicker_rate",
		obs_module_text("NeonFlickerRate"), 0.0, 5.0, 0.05);
}

static void neon_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "neon_outline", true);
	obs_data_set_default_int(settings, "neon_color", DEFAULT_NEON_COLOR);
	obs_data_set_default_double(settings, "neon_width", 3.0);
	obs_data_set_default_double(settings, "neon_glow", 14.0);
	obs_data_set_default_double(settings, "neon_bloom", 1.0);
	obs_data_set_default_double(settings, "neon_brightness", 1.0);
	obs_data_set_default_double(settings, "neon_flicker_rate", 1.0);
}

const struct text_effect fx_neon = {
	.id             = "neon",
	.name_key       = "EffectNeon",
	.create         = neon_create,
	.destroy        = neon_destroy,
	.load_graphics  = neon_load_graphics,
	.update         = neon_update,
	.render         = neon_render,
	.get_properties = neon_properties,
	.get_defaults   = neon_defaults,
};
