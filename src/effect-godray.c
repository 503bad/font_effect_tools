#include "effect-godray.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_RAY_FONT  0xFFFFFFFFu /* white #FFFFFF        */
#define DEFAULT_RAY_LIGHT 0xFFB9F0FFu /* warm light #FFF0B9   */

/* Per-instance state for the god-ray effect. */
struct godray_state {
	gs_effect_t *effect;

	uint32_t font_color;  /* base text fill (OBS 0xAABBGGRR) */
	uint32_t light_color; /* shaft tint                      */
	float    strength;    /* shaft brightness / density      */
	float    length;      /* shaft length multiplier         */
	float    light;       /* light intensity                 */
	float    random;      /* flutter randomness              */
	float    angle;       /* shaft direction in degrees      */
	float    font_px;     /* cached font size (for reach)    */
};

/* Shaft reach, in font-size units per length unit. */
#define RAY_REACH 2.4f

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *godray_create(void)
{
	struct godray_state *s = bzalloc(sizeof(struct godray_state));
	s->font_px = 64.0f;
	return s;
}

static void godray_destroy(void *data)
{
	struct godray_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void godray_load_graphics(void *data)
{
	struct godray_state *s = data;
	char *path = obs_module_file("effects/godray.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load godray.effect (%s)",
				path);
	}
	bfree(path);
}

static void godray_update(void *data, obs_data_t *settings)
{
	struct godray_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "ray_color");
	s->light_color = (uint32_t)obs_data_get_int(settings, "ray_light");
	s->strength = (float)obs_data_get_double(settings, "ray_strength");
	s->length = (float)obs_data_get_double(settings, "ray_length");
	s->light = (float)obs_data_get_double(settings, "ray_light_intensity");
	s->random = (float)obs_data_get_double(settings, "ray_random");
	s->angle = (float)obs_data_get_double(settings, "ray_angle");
}

/* Reserve room on all sides so the shafts fade out before the canvas edge
 * (the shaft direction is configurable, so pad every side). */
static void godray_wanted_margins(void *data, uint32_t font_size,
				  struct fx_margins *out)
{
	struct godray_state *s = data;
	s->font_px = (float)font_size;
	uint32_t reach = (uint32_t)((float)font_size * s->length * RAY_REACH *
				    1.15f) + 8u;
	out->left = reach;
	out->right = reach;
	out->top = reach;
	out->bottom = reach;
}

static void godray_render(void *data, const struct fx_render_ctx *ctx)
{
	struct godray_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_canvas) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p_canvas, &c);
	}

	gs_eparam_t *pf, *pl;
	pf = gs_effect_get_param_by_name(e, "font_color");
	if (pf) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(pf, &col);
	}
	pl = gs_effect_get_param_by_name(e, "light_color");
	if (pl) {
		float rgba[4];
		unpack_color(s->light_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(pl, &col);
	}

	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "time")))
		gs_effect_set_float(p, fmodf(ctx->time, 1000.0f));
	if ((p = gs_effect_get_param_by_name(e, "strength")))
		gs_effect_set_float(p, s->strength);
	if ((p = gs_effect_get_param_by_name(e, "length")))
		gs_effect_set_float(p, s->font_px * s->length * RAY_REACH);
	if ((p = gs_effect_get_param_by_name(e, "light")))
		gs_effect_set_float(p, s->light);
	if ((p = gs_effect_get_param_by_name(e, "random")))
		gs_effect_set_float(p, s->random);

	gs_eparam_t *p_dir = gs_effect_get_param_by_name(e, "dir");
	if (p_dir) {
		float rad = s->angle * 3.14159265f / 180.0f;
		struct vec2 d;
		vec2_set(&d, cosf(rad), sinf(rad));
		gs_effect_set_vec2(p_dir, &d);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void godray_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "ray_color",
		obs_module_text("RayColor"));
	obs_properties_add_color_alpha(p, "ray_light",
		obs_module_text("RayLight"));
	obs_properties_add_float_slider(p, "ray_strength",
		obs_module_text("RayStrength"), 0.0, 2.0, 0.02);
	obs_properties_add_float_slider(p, "ray_length",
		obs_module_text("RayLength"), 0.2, 4.0, 0.05);
	obs_properties_add_float_slider(p, "ray_light_intensity",
		obs_module_text("RayLightIntensity"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "ray_random",
		obs_module_text("RayRandom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "ray_angle",
		obs_module_text("RayAngle"), 0.0, 360.0, 1.0);
}

static void godray_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "ray_color", DEFAULT_RAY_FONT);
	obs_data_set_default_int(settings, "ray_light", DEFAULT_RAY_LIGHT);
	obs_data_set_default_double(settings, "ray_strength", 1.0);
	obs_data_set_default_double(settings, "ray_length", 1.0);
	obs_data_set_default_double(settings, "ray_light_intensity", 1.2);
	obs_data_set_default_double(settings, "ray_random", 0.4);
	obs_data_set_default_double(settings, "ray_angle", 110.0);
}

const struct text_effect fx_godray = {
	.id             = "godray",
	.name_key       = "EffectGodray",
	.create         = godray_create,
	.destroy        = godray_destroy,
	.load_graphics  = godray_load_graphics,
	.update         = godray_update,
	.wanted_margins = godray_wanted_margins,
	.render         = godray_render,
	.get_properties = godray_properties,
	.get_defaults   = godray_defaults,
};
