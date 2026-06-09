#include "effect-spotlight.h"
#include "flametext-outline.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SPOT_FONT  0xFFFFFFFFu /* white #FFFFFF                  */
#define DEFAULT_SPOT_LIGHT 0xFFFFFFFFu /* white sweep                    */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black                */

/* Per-instance state for the spotlight sweep effect. */
struct spotlight_state {
	gs_effect_t *effect;
	gs_effect_t *outline;

	uint32_t font_color;  /* base text fill (OBS 0xAABBGGRR) */
	uint32_t light_color; /* sweeping highlight tint         */
	float    strength;    /* peak flare strength             */
	float    speed;       /* sweeps per second               */
	float    width;       /* band width (fraction)           */
	float    angle;       /* sweep direction in degrees       */

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;
};

/* Decompose an OBS 0xAABBGGRR color into normalized rgba. */
static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *spotlight_create(void)
{
	return bzalloc(sizeof(struct spotlight_state));
}

static void spotlight_destroy(void *data)
{
	struct spotlight_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	if (s->outline)
		gs_effect_destroy(s->outline);
	bfree(s);
}

static void spotlight_load_graphics(void *data)
{
	struct spotlight_state *s = data;
	char *path = obs_module_file("effects/spotlight.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR,
				"failed to load spotlight.effect (%s)", path);
	}
	bfree(path);
	s->outline = fx_outline_load();
}

static void spotlight_update(void *data, obs_data_t *settings)
{
	struct spotlight_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "spot_color");
	s->light_color = (uint32_t)obs_data_get_int(settings, "spot_light");
	s->strength = (float)obs_data_get_double(settings, "spot_strength");
	s->speed = (float)obs_data_get_double(settings, "spot_speed");
	s->width = (float)obs_data_get_double(settings, "spot_width");
	s->angle = (float)obs_data_get_double(settings, "spot_angle");
	s->outline_on = obs_data_get_bool(settings, "spot_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "spot_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "spot_outline_color");
}

static void set_color(gs_effect_t *e, const char *name, uint32_t c)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (!p)
		return;
	float rgba[4];
	unpack_color(c, rgba);
	struct vec4 v;
	vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
	gs_effect_set_vec4(p, &v);
}

static void set_float(gs_effect_t *e, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, v);
}

static void spotlight_render(void *data, const struct fx_render_ctx *ctx)
{
	struct spotlight_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);

	set_color(e, "font_color", s->font_color);
	set_color(e, "light_color", s->light_color);
	set_float(e, "time", fmodf(ctx->time, 1000.0f));
	set_float(e, "strength", s->strength);
	set_float(e, "speed", s->speed);
	set_float(e, "width", s->width < 0.01f ? 0.01f : s->width);

	gs_eparam_t *p_dir = gs_effect_get_param_by_name(e, "dir");
	if (p_dir) {
		float rad = s->angle * 3.14159265f / 180.0f;
		struct vec2 d;
		vec2_set(&d, cosf(rad), sinf(rad));
		gs_effect_set_vec2(p_dir, &d);
	}

	if (s->outline_on && s->outline) {
		float oc[4];
		unpack_color(s->outline_color, oc);
		fx_outline_render_full(s->outline, mask->tex, ctx->width,
				       ctx->height, oc, s->outline_width);
	}

	/* Premultiplied: the flare adds light over the base fill. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void spotlight_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "spot_color",
		obs_module_text("SpotColor"));
	obs_properties_add_color_alpha(p, "spot_light",
		obs_module_text("SpotLight"));
	obs_properties_add_float_slider(p, "spot_strength",
		obs_module_text("SpotStrength"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "spot_speed",
		obs_module_text("SpotSpeed"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "spot_width",
		obs_module_text("SpotWidth"), 0.02, 0.5, 0.01);
	obs_properties_add_float_slider(p, "spot_angle",
		obs_module_text("SpotAngle"), 0.0, 360.0, 1.0);
	obs_properties_add_bool(p, "spot_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "spot_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "spot_outline_color",
		obs_module_text("OutlineColor"));
}

static void spotlight_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "spot_color", DEFAULT_SPOT_FONT);
	obs_data_set_default_int(settings, "spot_light", DEFAULT_SPOT_LIGHT);
	obs_data_set_default_double(settings, "spot_strength", 1.4);
	obs_data_set_default_double(settings, "spot_speed", 0.6);
	obs_data_set_default_double(settings, "spot_width", 0.1);
	obs_data_set_default_double(settings, "spot_angle", 75.0);
	obs_data_set_default_bool(settings, "spot_outline", false);
	obs_data_set_default_double(settings, "spot_outline_width", 4.0);
	obs_data_set_default_int(settings, "spot_outline_color",
				 DEFAULT_OUTLINE_COLOR);
}

const struct text_effect fx_spotlight = {
	.id             = "spotlight",
	.name_key       = "EffectSpotlight",
	.create         = spotlight_create,
	.destroy        = spotlight_destroy,
	.load_graphics  = spotlight_load_graphics,
	.update         = spotlight_update,
	.render         = spotlight_render,
	.get_properties = spotlight_properties,
	.get_defaults   = spotlight_defaults,
};
