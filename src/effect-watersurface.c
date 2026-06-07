#include "effect-watersurface.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_WSURF_COLOR 0xFFFFFFFFu /* white */

/* Wave shapes (matches the dropdown order / the shader's `wave_mode`). */
#define WSURF_WAVE_HORIZONTAL 0 /* whole reflection swaying side to side */
#define WSURF_WAVE_CONCENTRIC 1 /* rings spreading from the centre        */

/* Per-instance state for the water-surface (reflection) effect. */
struct watersurface_state {
	gs_effect_t *effect;

	uint32_t color;        /* OBS 0xAABBGGRR font/reflection tint    */
	int      wave_mode;    /* WSURF_WAVE_*                           */
	float    wave_strength;/* ripple amplitude, 0..1                 */
	float    wave_freq;    /* ripple cycles across the reflection    */
	float    wave_speed;   /* ripple animation speed                 */
	float    wave_random;  /* irregularity of the ripples, 0..1      */
	float    reflect;      /* reflection opacity at the waterline    */
	float    fade;         /* how strongly it fades with depth, 0..1 */
	float    glow;         /* body emission multiplier               */
	float    bloom;        /* outward halo multiplier                */
	float    bloom_radius; /* halo spread, pixels                    */
};

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *watersurface_create(void)
{
	return bzalloc(sizeof(struct watersurface_state));
}

static void watersurface_destroy(void *data)
{
	struct watersurface_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void watersurface_load_graphics(void *data)
{
	struct watersurface_state *s = data;
	char *path = obs_module_file("effects/watersurface.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR,
				"failed to load watersurface.effect (%s)", path);
	}
	bfree(path);
}

static void watersurface_update(void *data, obs_data_t *settings)
{
	struct watersurface_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "wsurf_color");
	s->wave_mode = (int)obs_data_get_int(settings, "wsurf_wave_mode");
	s->wave_strength =
		(float)obs_data_get_double(settings, "wsurf_wave_strength");
	s->wave_freq = (float)obs_data_get_double(settings, "wsurf_wave_freq");
	s->wave_speed = (float)obs_data_get_double(settings, "wsurf_wave_speed");
	s->wave_random =
		(float)obs_data_get_double(settings, "wsurf_wave_random");
	s->reflect = (float)obs_data_get_double(settings, "wsurf_reflect");
	s->fade = (float)obs_data_get_double(settings, "wsurf_fade");
	s->glow = (float)obs_data_get_double(settings, "wsurf_glow");
	s->bloom = (float)obs_data_get_double(settings, "wsurf_bloom");
	s->bloom_radius =
		(float)obs_data_get_double(settings, "wsurf_bloom_radius");
}

/* Reserve room below the text for the flipped reflection plus the bloom halo
 * and the ripple's vertical swing. The reflection mirrors the text downward;
 * a single line of text spans roughly the font height, so ~1.9x comfortably
 * covers ascenders and descenders even for descender-heavy faces (so the far
 * end is not clipped when the depth fade is turned down). Far parts of taller
 * multi-line text fade out before reaching here. */
static uint32_t watersurface_wanted_bottom_pad(void *data, uint32_t font_size)
{
	struct watersurface_state *s = data;
	float reflection = font_size * 1.9f;
	float wave_margin = font_size * 0.3f * s->wave_strength;
	return (uint32_t)(reflection + s->bloom_radius + wave_margin + 4.0f);
}

static void watersurface_render(void *data, const struct fx_render_ctx *ctx)
{
	struct watersurface_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	if (ctx->height == 0)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "font_color");
	gs_eparam_t *p_waterline = gs_effect_get_param_by_name(e, "waterline");
	gs_eparam_t *p_band = gs_effect_get_param_by_name(e, "band");
	gs_eparam_t *p_wave_mode = gs_effect_get_param_by_name(e, "wave_mode");
	gs_eparam_t *p_wave_strength =
		gs_effect_get_param_by_name(e, "wave_strength");
	gs_eparam_t *p_wave_freq = gs_effect_get_param_by_name(e, "wave_freq");
	gs_eparam_t *p_wave_speed = gs_effect_get_param_by_name(e, "wave_speed");
	gs_eparam_t *p_wave_random =
		gs_effect_get_param_by_name(e, "wave_random");
	gs_eparam_t *p_reflect = gs_effect_get_param_by_name(e, "reflect");
	gs_eparam_t *p_fade = gs_effect_get_param_by_name(e, "fade");
	gs_eparam_t *p_glow = gs_effect_get_param_by_name(e, "glow");
	gs_eparam_t *p_bloom = gs_effect_get_param_by_name(e, "bloom");
	gs_eparam_t *p_radius = gs_effect_get_param_by_name(e, "radius");
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
	/* Waterline and reflection band, in uv (0..1), from the text geometry. */
	if (p_waterline)
		gs_effect_set_float(p_waterline,
				    mask->text_bottom / (float)ctx->height);
	if (p_band) {
		float band = (mask->text_bottom - mask->text_top) /
			     (float)ctx->height;
		gs_effect_set_float(p_band, band > 1e-4f ? band : 1e-4f);
	}
	if (p_wave_mode)
		gs_effect_set_float(p_wave_mode, (float)s->wave_mode);
	if (p_wave_strength)
		gs_effect_set_float(p_wave_strength, s->wave_strength);
	if (p_wave_freq)
		gs_effect_set_float(p_wave_freq, s->wave_freq);
	if (p_wave_speed)
		gs_effect_set_float(p_wave_speed, s->wave_speed);
	if (p_wave_random)
		gs_effect_set_float(p_wave_random, s->wave_random);
	if (p_reflect)
		gs_effect_set_float(p_reflect, s->reflect);
	if (p_fade)
		gs_effect_set_float(p_fade, s->fade);
	if (p_glow)
		gs_effect_set_float(p_glow, s->glow);
	if (p_bloom)
		gs_effect_set_float(p_bloom, s->bloom);
	if (p_radius)
		gs_effect_set_float(p_radius, s->bloom_radius);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(ctx->time, 1000.0f));

	/* Premultiplied output: bright cores/halo add light over the background. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void watersurface_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "wsurf_color",
		obs_module_text("WaterSurfaceColor"));

	obs_property_t *mode = obs_properties_add_list(p, "wsurf_wave_mode",
		obs_module_text("WaterSurfaceWaveMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode,
		obs_module_text("WaterSurfaceWaveHorizontal"),
		WSURF_WAVE_HORIZONTAL);
	obs_property_list_add_int(mode,
		obs_module_text("WaterSurfaceWaveConcentric"),
		WSURF_WAVE_CONCENTRIC);

	obs_properties_add_float_slider(p, "wsurf_wave_strength",
		obs_module_text("WaterSurfaceWaveStrength"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_wave_freq",
		obs_module_text("WaterSurfaceWaveFreq"), 1.0, 20.0, 0.5);
	obs_properties_add_float_slider(p, "wsurf_wave_speed",
		obs_module_text("WaterSurfaceWaveSpeed"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_wave_random",
		obs_module_text("WaterSurfaceWaveRandom"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_reflect",
		obs_module_text("WaterSurfaceReflect"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_fade",
		obs_module_text("WaterSurfaceFade"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_glow",
		obs_module_text("WaterSurfaceGlow"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_bloom",
		obs_module_text("WaterSurfaceBloom"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "wsurf_bloom_radius",
		obs_module_text("WaterSurfaceBloomRadius"), 2.0, 60.0, 0.5);
}

static void watersurface_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "wsurf_color", DEFAULT_WSURF_COLOR);
	obs_data_set_default_int(settings, "wsurf_wave_mode",
				 WSURF_WAVE_HORIZONTAL);
	obs_data_set_default_double(settings, "wsurf_wave_strength", 0.5);
	obs_data_set_default_double(settings, "wsurf_wave_freq", 6.0);
	obs_data_set_default_double(settings, "wsurf_wave_speed", 1.0);
	obs_data_set_default_double(settings, "wsurf_wave_random", 0.3);
	obs_data_set_default_double(settings, "wsurf_reflect", 0.7);
	obs_data_set_default_double(settings, "wsurf_fade", 0.6);
	obs_data_set_default_double(settings, "wsurf_glow", 0.8);
	obs_data_set_default_double(settings, "wsurf_bloom", 1.0);
	obs_data_set_default_double(settings, "wsurf_bloom_radius", 16.0);
}

const struct text_effect fx_watersurface = {
	.id                = "watersurface",
	.name_key          = "EffectWaterSurface",
	.create            = watersurface_create,
	.destroy           = watersurface_destroy,
	.load_graphics     = watersurface_load_graphics,
	.update            = watersurface_update,
	.wanted_bottom_pad = watersurface_wanted_bottom_pad,
	.render            = watersurface_render,
	.get_properties    = watersurface_properties,
	.get_defaults      = watersurface_defaults,
};
