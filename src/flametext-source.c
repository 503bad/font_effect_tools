#include "flametext-source.h"
#include "flametext-text.h"
#include "flametext-particles.h"
#include "flametext-font-resolve.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_FONT_SIZE     120
#define SPARK_CAPACITY        4096
#define DEFAULT_SPARK_COLOR   0xFFDCFFFFu /* pale warm yellow (#FFFFDC) in ABGR */

static const char *flametext_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FlameTextSource");
}

static void replace_string(char **dst, const char *src)
{
	if (*dst)
		bfree(*dst);
	*dst = src ? bstrdup(src) : NULL;
}

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void apply_spark_color(struct flametext_source *s)
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

static void rebuild_mask(struct flametext_source *s)
{
	obs_enter_graphics();
	if (s->mask) {
		flametext_mask_free(s->mask);
		s->mask = NULL;
	}
	if (s->text && s->text[0] && s->font_path[0]) {
		s->mask = flametext_mask_build(s->text, s->font_path,
					       s->font_size, s->bold, s->italic);
	}
	obs_leave_graphics();

	if (s->mask && s->sparks) {
		/* Origin 1 emits below the text (off the bottom edge, dir +1);
		 * origin 0 emits above the text (off the top edge, dir -1). */
		float edge_y = (s->spark_origin == 1) ? s->mask->text_bottom
						      : s->mask->text_top;
		float dir = (s->spark_origin == 1) ? 1.0f : -1.0f;
		flametext_particles_set_emitter(s->sparks, s->mask->text_left,
						s->mask->text_right, edge_y, dir);
	}
	if (s->text && s->text[0] && s->font_path[0] && !s->mask)
		obs_log(LOG_WARNING, "flame mask build failed (text='%s' font='%s')",
			s->text, s->font_path);
}

static void flametext_update(void *data, obs_data_t *settings)
{
	struct flametext_source *s = data;

	replace_string(&s->text, obs_data_get_string(settings, "text"));

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	const char *face = font_obj ? obs_data_get_string(font_obj, "face") : "";
	uint32_t size = font_obj ? (uint32_t)obs_data_get_int(font_obj, "size")
				 : DEFAULT_FONT_SIZE;
	long long flags = font_obj ? obs_data_get_int(font_obj, "flags") : 0;
	bool bold = (flags & OBS_FONT_BOLD) != 0;
	bool italic = (flags & OBS_FONT_ITALIC) != 0;

	bool face_changed = (s->font_face == NULL && face[0]) ||
			    (s->font_face && strcmp(s->font_face, face) != 0);
	bool style_changed = (s->bold != bold) || (s->italic != italic);

	replace_string(&s->font_face, face);
	s->font_size = size ? size : DEFAULT_FONT_SIZE;
	s->bold = bold;
	s->italic = italic;

	if (face_changed || style_changed) {
		s->font_path[0] = 0;
		if (s->font_face && s->font_face[0])
			flametext_resolve_font(s->font_face, s->bold, s->italic,
					       s->font_path, sizeof(s->font_path));
	}

	/* Flame parameters */
	s->flame_height = (float)obs_data_get_double(settings, "flame_height");
	s->sway_speed = (float)obs_data_get_double(settings, "sway_speed");
	s->color_temp = (float)obs_data_get_double(settings, "color_temp");
	s->intensity = (float)obs_data_get_double(settings, "intensity");

	/* Spark parameters */
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

	rebuild_mask(s);

	if (font_obj)
		obs_data_release(font_obj);
}

static void load_effects(struct flametext_source *s)
{
	char *flame_path = obs_module_file("effects/flame.effect");
	char *spark_path = obs_module_file("effects/spark.effect");

	obs_enter_graphics();
	if (flame_path) {
		s->flame_effect = gs_effect_create_from_file(flame_path, NULL);
		if (!s->flame_effect)
			obs_log(LOG_ERROR, "failed to load flame.effect (%s)", flame_path);
	}
	if (spark_path) {
		s->spark_effect = gs_effect_create_from_file(spark_path, NULL);
		if (!s->spark_effect)
			obs_log(LOG_ERROR, "failed to load spark.effect (%s)", spark_path);
	}
	obs_leave_graphics();

	bfree(flame_path);
	bfree(spark_path);
}

static void *flametext_create(obs_data_t *settings, obs_source_t *source)
{
	struct flametext_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->clock = 0.0f;
	s->sparks = flametext_particles_create(SPARK_CAPACITY);
	load_effects(s);
	flametext_update(s, settings);
	return s;
}

static void flametext_destroy(void *data)
{
	struct flametext_source *s = data;
	obs_enter_graphics();
	if (s->mask)
		flametext_mask_free(s->mask);
	if (s->flame_effect)
		gs_effect_destroy(s->flame_effect);
	if (s->spark_effect)
		gs_effect_destroy(s->spark_effect);
	obs_leave_graphics();

	flametext_particles_destroy(s->sparks);
	bfree(s->text);
	bfree(s->font_face);
	bfree(s);
}

static void flametext_video_tick(void *data, float seconds)
{
	struct flametext_source *s = data;
	s->clock += seconds;
	if (s->sparks && s->mask)
		flametext_particles_tick(s->sparks, seconds);
}

static void flametext_show(void *data)
{
	struct flametext_source *s = data;
	s->clock = 0.0f;
	if (s->sparks)
		flametext_particles_reset(s->sparks);
}

static void flametext_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct flametext_source *s = data;
	if (!s->mask || !s->mask->tex || !s->flame_effect)
		return;

	const float w = (float)s->mask->width;
	const float h = (float)s->mask->height;

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
		gs_effect_set_texture(p_image, s->mask->tex);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(s->clock, 1000.0f));
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
		gs_draw_sprite(s->mask->tex, 0, 0, 0);
	gs_blend_state_pop();

	/* --- spark pass (additive) --- */
	if (s->spark_effect)
		flametext_particles_render(s->sparks, s->spark_effect);
}

static uint32_t flametext_get_width(void *data)
{
	const struct flametext_source *s = data;
	return (s->mask && s->mask->width) ? s->mask->width : 1u;
}

static uint32_t flametext_get_height(void *data)
{
	const struct flametext_source *s = data;
	return (s->mask && s->mask->height) ? s->mask->height : 1u;
}

static obs_properties_t *flametext_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_text(p, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);
	obs_properties_add_font(p, "font", obs_module_text("Font"));

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

	return p;
}

static void flametext_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", "test");

	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", "Impact");
	obs_data_set_default_int(font, "size", DEFAULT_FONT_SIZE);
	obs_data_set_default_int(font, "flags", 0);
	obs_data_set_default_string(font, "style", "Regular");
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);

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

static struct obs_source_info s_flametext_source_info = {
	.id             = "flame_text_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type      = OBS_ICON_TYPE_TEXT,
	.get_name       = flametext_get_name,
	.create         = flametext_create,
	.destroy        = flametext_destroy,
	.update         = flametext_update,
	.video_tick     = flametext_video_tick,
	.video_render   = flametext_video_render,
	.get_width      = flametext_get_width,
	.get_height     = flametext_get_height,
	.get_properties = flametext_properties,
	.get_defaults   = flametext_defaults,
	.show           = flametext_show,
};

void flametext_register_source(void)
{
	obs_register_source(&s_flametext_source_info);
}
