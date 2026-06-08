#include "effect-scanline.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SCAN_FONT 0xFF66FF66u /* phosphor green-ish #66FF66 */

/* Per-instance state for the CRT scanline effect. */
struct scanline_state {
	gs_effect_t *effect;

	uint32_t font_color; /* text fill (OBS 0xAABBGGRR) */
	float    intensity;  /* scanline darkness          */
	float    density;    /* lines across the text      */
	float    flicker;    /* brightness flicker amount  */
	float    wobble;     /* horizontal bleed / jitter  */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *scanline_create(void)
{
	return bzalloc(sizeof(struct scanline_state));
}

static void scanline_destroy(void *data)
{
	struct scanline_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void scanline_load_graphics(void *data)
{
	struct scanline_state *s = data;
	char *path = obs_module_file("effects/scanline.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR,
				"failed to load scanline.effect (%s)", path);
	}
	bfree(path);
}

static void scanline_update(void *data, obs_data_t *settings)
{
	struct scanline_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "scan_color");
	s->intensity = (float)obs_data_get_double(settings, "scan_intensity");
	s->density = (float)obs_data_get_double(settings, "scan_density");
	s->flicker = (float)obs_data_get_double(settings, "scan_flicker");
	s->wobble = (float)obs_data_get_double(settings, "scan_wobble");
}

static void scanline_render(void *data, const struct fx_render_ctx *ctx)
{
	struct scanline_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
	gs_eparam_t *p_canvas = gs_effect_get_param_by_name(e, "canvas");
	gs_eparam_t *p_color = gs_effect_get_param_by_name(e, "font_color");

	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_canvas) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p_canvas, &c);
	}
	if (p_color) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p_color, &col);
	}

	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "time")))
		gs_effect_set_float(p, fmodf(ctx->time, 1000.0f));
	if ((p = gs_effect_get_param_by_name(e, "intensity")))
		gs_effect_set_float(p, s->intensity);
	if ((p = gs_effect_get_param_by_name(e, "density")))
		gs_effect_set_float(p, s->density);
	if ((p = gs_effect_get_param_by_name(e, "flicker")))
		gs_effect_set_float(p, s->flicker);
	if ((p = gs_effect_get_param_by_name(e, "wobble")))
		gs_effect_set_float(p, s->wobble);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void scanline_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "scan_color",
		obs_module_text("ScanColor"));
	obs_properties_add_float_slider(p, "scan_intensity",
		obs_module_text("ScanIntensity"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "scan_density",
		obs_module_text("ScanDensity"), 0.1, 1.0, 0.01);
	obs_properties_add_float_slider(p, "scan_flicker",
		obs_module_text("ScanFlicker"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "scan_wobble",
		obs_module_text("ScanWobble"), 0.0, 1.0, 0.01);
}

static void scanline_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "scan_color", DEFAULT_SCAN_FONT);
	obs_data_set_default_double(settings, "scan_intensity", 0.6);
	obs_data_set_default_double(settings, "scan_density", 0.5);
	obs_data_set_default_double(settings, "scan_flicker", 0.3);
	obs_data_set_default_double(settings, "scan_wobble", 0.3);
}

const struct text_effect fx_scanline = {
	.id             = "scanline",
	.name_key       = "EffectScanline",
	.create         = scanline_create,
	.destroy        = scanline_destroy,
	.load_graphics  = scanline_load_graphics,
	.update         = scanline_update,
	.render         = scanline_render,
	.get_properties = scanline_properties,
	.get_defaults   = scanline_defaults,
};
