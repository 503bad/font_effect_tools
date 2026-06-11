#include "effect-wave.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_WAV_FONT 0xFFFFFFFFu      /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */

struct wave_state {
	gs_effect_t *effect;

	uint32_t font_color;
	float    freq;     /* ripples per text height        */
	float    wrandom;  /* irregularity 0..1              */
	float    strength; /* displacement amount 0..1       */
	float    speed;
	float    glow_amt;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *wave_create(void)
{
	return bzalloc(sizeof(struct wave_state));
}

static void wave_destroy(void *data)
{
	struct wave_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void wave_load_graphics(void *data)
{
	struct wave_state *s = data;
	char *path = obs_module_file("effects/wave.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load wave.effect (%s)",
				path);
	}
	bfree(path);
}

static void wave_update(void *data, obs_data_t *settings)
{
	struct wave_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "wav_font");
	s->freq = (float)obs_data_get_double(settings, "wav_freq");
	s->wrandom = (float)obs_data_get_double(settings, "wav_random");
	s->strength = (float)obs_data_get_double(settings, "wav_strength");
	s->speed = (float)obs_data_get_double(settings, "wav_speed");
	s->glow_amt = (float)obs_data_get_double(settings, "wav_glow");
	s->outline_on = obs_data_get_bool(settings, "wav_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "wav_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "wav_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "wav_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "wav_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "wav_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "wav_shadow_y");
}

static void set_vec2(gs_effect_t *e, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

static void set_vec4c(gs_effect_t *e, const char *name, const float rgba[4])
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec4 v;
		vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &v);
	}
}

static void set_float(gs_effect_t *e, const char *name, float f)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, f);
}

static void wave_render(void *data, const struct fx_render_ctx *ctx)
{
	struct wave_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	gs_effect_t *e = s->effect;

	float hgt = mask->text_bottom - mask->text_top;
	if (hgt < 1.0f)
		hgt = 1.0f;

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);

	gs_eparam_t *pi = gs_effect_get_param_by_name(e, "image");
	if (pi)
		gs_effect_set_texture(pi, mask->tex);
	set_vec2(e, "canvas", (float)ctx->width, (float)ctx->height);
	set_vec4c(e, "font_color", rgba);
	set_vec2(e, "center",
		 (mask->text_left + mask->text_right) * 0.5f,
		 (mask->text_top + mask->text_bottom) * 0.5f);
	set_float(e, "hpx", hgt);
	set_float(e, "time", fmodf(ctx->time, 1000.0f));
	set_float(e, "freq", s->freq);
	set_float(e, "strength", s->strength);
	set_float(e, "speed", s->speed);
	set_float(e, "wrandom", s->wrandom);
	set_float(e, "outline_on", s->outline_on ? 1.0f : 0.0f);
	set_float(e, "outline_width", s->outline_width);
	set_vec4c(e, "outline_color", orgba);
	set_float(e, "glow", s->glow_amt);
	set_float(e, "shadow_on",
		  (s->shadow_on && shrgba[3] > 0.0f) ? 1.0f : 0.0f);
	set_vec4c(e, "shadow_color", shrgba);
	set_vec2(e, "shadow_off", (float)s->shadow_x, (float)s->shadow_y);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void wave_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "wav_font",
		obs_module_text("WavFontColor"));
	obs_properties_add_float_slider(p, "wav_freq",
		obs_module_text("WavFreq"), 1.0, 20.0, 0.1);
	obs_properties_add_float_slider(p, "wav_random",
		obs_module_text("WavRandom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "wav_strength",
		obs_module_text("WavStrength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "wav_speed",
		obs_module_text("WavSpeed"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "wav_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "wav_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "wav_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "wav_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "wav_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "wav_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "wav_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "wav_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
}

static void wave_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "wav_font", DEFAULT_WAV_FONT);
	obs_data_set_default_double(settings, "wav_freq", 6.0);
	obs_data_set_default_double(settings, "wav_random", 0.3);
	obs_data_set_default_double(settings, "wav_strength", 0.5);
	obs_data_set_default_double(settings, "wav_speed", 1.0);
	obs_data_set_default_double(settings, "wav_glow", 0.0);
	obs_data_set_default_bool(settings, "wav_outline", false);
	obs_data_set_default_double(settings, "wav_outline_width", 4.0);
	obs_data_set_default_int(settings, "wav_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "wav_shadow", false);
	obs_data_set_default_int(settings, "wav_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "wav_shadow_x", 4);
	obs_data_set_default_int(settings, "wav_shadow_y", 4);
}

const struct text_effect fx_wave = {
	.id             = "wave",
	.name_key       = "EffectWave",
	.create         = wave_create,
	.destroy        = wave_destroy,
	.load_graphics  = wave_load_graphics,
	.update         = wave_update,
	.render         = wave_render,
	.get_properties = wave_properties,
	.get_defaults   = wave_defaults,
};
