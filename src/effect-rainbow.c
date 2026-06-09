#include "effect-rainbow.h"
#include "flametext-outline.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black */

/* Per-instance state for the rainbow fill effect. */
struct rainbow_state {
	gs_effect_t *effect;
	gs_effect_t *outline;

	float speed;      /* hue scroll speed                      */
	float scale;      /* number of color bands across the text */
	float saturation; /* 0..1                                  */
	float brightness; /* 0..1                                  */
	float angle;      /* gradient direction in degrees         */

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *rainbow_create(void)
{
	return bzalloc(sizeof(struct rainbow_state));
}

static void rainbow_destroy(void *data)
{
	struct rainbow_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	if (s->outline)
		gs_effect_destroy(s->outline);
	bfree(s);
}

static void rainbow_load_graphics(void *data)
{
	struct rainbow_state *s = data;
	char *path = obs_module_file("effects/rainbow.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load rainbow.effect (%s)",
				path);
	}
	bfree(path);
	s->outline = fx_outline_load();
}

static void rainbow_update(void *data, obs_data_t *settings)
{
	struct rainbow_state *s = data;
	s->speed = (float)obs_data_get_double(settings, "rb_speed");
	s->scale = (float)obs_data_get_double(settings, "rb_scale");
	s->saturation = (float)obs_data_get_double(settings, "rb_saturation");
	s->brightness = (float)obs_data_get_double(settings, "rb_brightness");
	s->angle = (float)obs_data_get_double(settings, "rb_angle");
	s->outline_on = obs_data_get_bool(settings, "rb_outline");
	s->outline_width = (float)obs_data_get_double(settings, "rb_outline_width");
	s->outline_color = (uint32_t)obs_data_get_int(settings, "rb_outline_color");
}

static void rainbow_render(void *data, const struct fx_render_ctx *ctx)
{
	struct rainbow_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_time = gs_effect_get_param_by_name(e, "time");
	gs_eparam_t *p_speed = gs_effect_get_param_by_name(e, "speed");
	gs_eparam_t *p_scale = gs_effect_get_param_by_name(e, "scale");
	gs_eparam_t *p_sat = gs_effect_get_param_by_name(e, "saturation");
	gs_eparam_t *p_bri = gs_effect_get_param_by_name(e, "brightness");
	gs_eparam_t *p_dir = gs_effect_get_param_by_name(e, "dir");

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_time)
		gs_effect_set_float(p_time, fmodf(ctx->time, 1000.0f));
	if (p_speed)
		gs_effect_set_float(p_speed, s->speed);
	if (p_scale)
		gs_effect_set_float(p_scale, s->scale);
	if (p_sat)
		gs_effect_set_float(p_sat, s->saturation);
	if (p_bri)
		gs_effect_set_float(p_bri, s->brightness);
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

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void rainbow_properties(obs_properties_t *p)
{
	obs_properties_add_float_slider(p, "rb_speed",
		obs_module_text("RbSpeed"), 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(p, "rb_scale",
		obs_module_text("RbScale"), 0.25, 8.0, 0.05);
	obs_properties_add_float_slider(p, "rb_angle",
		obs_module_text("RbAngle"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(p, "rb_saturation",
		obs_module_text("RbSaturation"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "rb_brightness",
		obs_module_text("RbBrightness"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(p, "rb_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "rb_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "rb_outline_color",
		obs_module_text("OutlineColor"));
}

static void rainbow_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "rb_speed", 0.4);
	obs_data_set_default_double(settings, "rb_scale", 1.5);
	obs_data_set_default_double(settings, "rb_angle", 90.0);
	obs_data_set_default_double(settings, "rb_saturation", 0.95);
	obs_data_set_default_double(settings, "rb_brightness", 1.0);
	obs_data_set_default_bool(settings, "rb_outline", false);
	obs_data_set_default_double(settings, "rb_outline_width", 4.0);
	obs_data_set_default_int(settings, "rb_outline_color",
				 DEFAULT_OUTLINE_COLOR);
}

const struct text_effect fx_rainbow = {
	.id             = "rainbow",
	.name_key       = "EffectRainbow",
	.create         = rainbow_create,
	.destroy        = rainbow_destroy,
	.load_graphics  = rainbow_load_graphics,
	.update         = rainbow_update,
	.render         = rainbow_render,
	.get_properties = rainbow_properties,
	.get_defaults   = rainbow_defaults,
};
