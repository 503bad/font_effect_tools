#include "effect-flame.h"
#include "flametext-particles.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define SPARK_CAPACITY      4096
#define DEFAULT_SPARK_COLOR 0xFFDCFFFFu /* pale warm yellow (#FFFFDC) in ABGR */

/* Per-instance state for the flame effect. */
struct flame_state {
	gs_effect_t         *flame_effect;
	gs_effect_t         *spark_effect;
	struct spark_system *sparks;

	/* Flame shader parameters */
	float flame_height; /* upward reach, fraction of canvas (0..1) */
	float sway_speed;   /* turbulence scroll speed                 */
	float color_temp;   /* heat multiplier                         */
	float intensity;    /* overall strength                        */

	/* Spark parameters */
	float    emit_rate;
	float    init_speed;
	float    lifetime;
	float    spark_size;
	float    spread;
	uint32_t spark_color; /* OBS 0xAABBGGRR */
	int      spark_origin; /* 0 = above the text, 1 = below the text */
	float    bloom;        /* additive glow strength for particles */
};

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void apply_spark_color(struct flame_state *s)
{
	if (!s->sparks)
		return;
	float rgba[4];
	unpack_color(s->spark_color, rgba);
	/* Hot core: the picked color, nudged toward white. */
	s->sparks->hot[0] = clampf(rgba[0] + 0.12f, 0.0f, 1.0f);
	s->sparks->hot[1] = clampf(rgba[1] + 0.08f, 0.0f, 1.0f);
	s->sparks->hot[2] = clampf(rgba[2] + 0.04f, 0.0f, 1.0f);
	s->sparks->hot[3] = 1.0f;
	/* Cool tail: shifted toward deep red. */
	s->sparks->cool[0] = clampf(rgba[0], 0.0f, 1.0f);
	s->sparks->cool[1] = clampf(rgba[1] * 0.35f, 0.0f, 1.0f);
	s->sparks->cool[2] = clampf(rgba[2] * 0.12f, 0.0f, 1.0f);
	s->sparks->cool[3] = 1.0f;
}

static void *flame_create(void)
{
	struct flame_state *s = bzalloc(sizeof(*s));
	s->sparks = flametext_particles_create(SPARK_CAPACITY);
	return s;
}

static void flame_destroy(void *data)
{
	struct flame_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->flame_effect)
		gs_effect_destroy(s->flame_effect);
	if (s->spark_effect)
		gs_effect_destroy(s->spark_effect);
	flametext_particles_destroy(s->sparks);
	bfree(s);
}

static void flame_load_graphics(void *data)
{
	struct flame_state *s = data;
	char *flame_path = obs_module_file("effects/flame.effect");
	char *spark_path = obs_module_file("effects/spark.effect");

	/* Graphics lock held by the host. */
	if (flame_path) {
		s->flame_effect = gs_effect_create_from_file(flame_path, NULL);
		if (!s->flame_effect)
			obs_log(LOG_ERROR, "failed to load flame.effect (%s)",
				flame_path);
	}
	if (spark_path) {
		s->spark_effect = gs_effect_create_from_file(spark_path, NULL);
		if (!s->spark_effect)
			obs_log(LOG_ERROR, "failed to load spark.effect (%s)",
				spark_path);
	}

	bfree(flame_path);
	bfree(spark_path);
}

static void flame_update(void *data, obs_data_t *settings)
{
	struct flame_state *s = data;

	s->flame_height = (float)obs_data_get_double(settings, "flame_height");
	s->sway_speed = (float)obs_data_get_double(settings, "sway_speed");
	s->color_temp = (float)obs_data_get_double(settings, "color_temp");
	s->intensity = (float)obs_data_get_double(settings, "intensity");

	s->emit_rate = (float)obs_data_get_double(settings, "emit_rate");
	s->init_speed = (float)obs_data_get_double(settings, "init_speed");
	s->lifetime = (float)obs_data_get_double(settings, "lifetime");
	s->spark_size = (float)obs_data_get_double(settings, "spark_size");
	s->spread = (float)obs_data_get_double(settings, "spread");
	s->spark_color = (uint32_t)obs_data_get_int(settings, "spark_color");
	s->spark_origin = (int)obs_data_get_int(settings, "spark_origin");
	s->bloom = (float)obs_data_get_double(settings, "bloom");

	if (s->sparks) {
		s->sparks->emit_rate = s->emit_rate;
		s->sparks->init_speed = s->init_speed;
		s->sparks->lifetime = s->lifetime;
		s->sparks->base_size = s->spark_size;
		s->sparks->spread = clampf(s->spread, 0.0f, 1.0f);
		s->sparks->bloom = s->bloom;
		apply_spark_color(s);
	}
}

static void flame_set_mask(void *data, const struct flametext_mask *mask)
{
	struct flame_state *s = data;
	if (!s->sparks || !mask)
		return;
	/* Origin 1 emits below the text (off the bottom edge, dir +1);
	 * origin 0 emits above the text (off the top edge, dir -1). */
	float edge_y = (s->spark_origin == 1) ? mask->text_bottom
					      : mask->text_top;
	float dir = (s->spark_origin == 1) ? 1.0f : -1.0f;
	flametext_particles_set_emitter(s->sparks, mask->text_left,
					mask->text_right, edge_y, dir);
}

static void flame_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	struct flame_state *s = data;
	if (s->sparks && ctx->mask)
		flametext_particles_tick(s->sparks, dt);
}

static void flame_reset(void *data)
{
	struct flame_state *s = data;
	if (s->sparks)
		flametext_particles_reset(s->sparks);
}

static void flame_render(void *data, const struct fx_render_ctx *ctx)
{
	struct flame_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->flame_effect)
		return;

	const float w = (float)ctx->width;
	const float h = (float)ctx->height;

	/* --- flame pass (alpha-blended) --- */
	gs_effect_t *fe = s->flame_effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(fe, "image");
	gs_eparam_t *p_time = gs_effect_get_param_by_name(fe, "time");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(fe, "canvas");
	gs_eparam_t *p_height = gs_effect_get_param_by_name(fe, "flame_height");
	gs_eparam_t *p_sway = gs_effect_get_param_by_name(fe, "sway_speed");
	gs_eparam_t *p_temp = gs_effect_get_param_by_name(fe, "color_temp");
	gs_eparam_t *p_int = gs_effect_get_param_by_name(fe, "intensity");

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(ctx->time, 1000.0f));
	if (p_canvas) {
		struct vec2 c;
		vec2_set(&c, w, h);
		gs_effect_set_vec2(p_canvas, &c);
	}
	if (p_height)
		gs_effect_set_float(p_height, s->flame_height);
	if (p_sway)
		gs_effect_set_float(p_sway, s->sway_speed);
	if (p_temp)
		gs_effect_set_float(p_temp, s->color_temp);
	if (p_int)
		gs_effect_set_float(p_int, s->intensity);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	/* Pass the mask for sizing (canvas dimensions); sampling uses the
	 * explicitly-bound `image` param above. */
	while (gs_effect_loop(fe, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();

	/* --- spark pass (additive) --- */
	if (s->spark_effect)
		flametext_particles_render(s->sparks, s->spark_effect);
}

static void flame_properties(obs_properties_t *p)
{
	obs_properties_add_float_slider(p, "flame_height",
		obs_module_text("FlameHeight"), 0.0, 0.2, 0.005);
	obs_properties_add_float_slider(p, "sway_speed",
		obs_module_text("SwaySpeed"), 0.1, 3.0, 0.01);
	obs_properties_add_float_slider(p, "color_temp",
		obs_module_text("ColorTemp"), 0.5, 2.0, 0.01);
	obs_properties_add_float_slider(p, "intensity",
		obs_module_text("Intensity"), 0.3, 2.5, 0.01);

	obs_properties_add_float_slider(p, "emit_rate",
		obs_module_text("EmitRate"), 0.0, 1500.0, 10.0);
	obs_properties_add_float_slider(p, "init_speed",
		obs_module_text("InitSpeed"), 40.0, 600.0, 5.0);
	obs_properties_add_float_slider(p, "lifetime",
		obs_module_text("Lifetime"), 0.3, 3.0, 0.05);
	obs_properties_add_float_slider(p, "spark_size",
		obs_module_text("SparkSize"), 1.0, 20.0, 0.5);
	obs_properties_add_float_slider(p, "spread",
		obs_module_text("Spread"), 0.0, 1.0, 0.01);
	obs_properties_add_color_alpha(p, "spark_color",
		obs_module_text("SparkColor"));

	obs_property_t *origin = obs_properties_add_list(p, "spark_origin",
		obs_module_text("SparkOrigin"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(origin, obs_module_text("OriginTop"), 0);
	obs_property_list_add_int(origin, obs_module_text("OriginBottom"), 1);

	obs_properties_add_float_slider(p, "bloom",
		obs_module_text("Bloom"), 0.0, 3.0, 0.05);
}

static void flame_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "flame_height", 0.10);
	obs_data_set_default_double(settings, "sway_speed", 1.10);
	obs_data_set_default_double(settings, "color_temp", 1.0);
	obs_data_set_default_double(settings, "intensity", 1.0);

	obs_data_set_default_double(settings, "emit_rate", 150.0);
	obs_data_set_default_double(settings, "init_speed", 140.0);
	obs_data_set_default_double(settings, "lifetime", 2.25);
	obs_data_set_default_double(settings, "spark_size", 2.0);
	obs_data_set_default_double(settings, "spread", 1.0);
	obs_data_set_default_int(settings, "spark_color", DEFAULT_SPARK_COLOR);
	obs_data_set_default_int(settings, "spark_origin", 0);
	obs_data_set_default_double(settings, "bloom", 1.0);
}

const struct text_effect fx_flame = {
	.id             = "flame",
	.name_key       = "EffectFlame",
	.create         = flame_create,
	.destroy        = flame_destroy,
	.load_graphics  = flame_load_graphics,
	.update         = flame_update,
	.set_mask       = flame_set_mask,
	.tick           = flame_tick,
	.render         = flame_render,
	.reset          = flame_reset,
	.get_properties = flame_properties,
	.get_defaults   = flame_defaults,
};
