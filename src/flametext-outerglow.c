#include "flametext-outerglow.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define BLUR_TAPS 12 /* taps per side; must match the shader loop */

/* Decompose an OBS 0xAABBGGRR color into normalized rgb (alpha ignored;
 * the glow opacity is a separate slider). */
static void unpack_color(uint32_t c, float rgb[3])
{
	rgb[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgb[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgb[2] = (float)((c >> 16) & 0xFF) / 255.0f;
}

void fx_oglow_load(struct fx_outerglow *g)
{
	char *path = obs_module_file("effects/outerglow.effect");
	/* Graphics lock held by the host. */
	if (path) {
		g->effect = gs_effect_create_from_file(path, NULL);
		if (!g->effect)
			obs_log(LOG_ERROR,
				"failed to load outerglow.effect (%s)", path);
	}
	bfree(path);
}

void fx_oglow_free(struct fx_outerglow *g)
{
	/* Graphics lock held by the host. */
	if (g->effect)
		gs_effect_destroy(g->effect);
	if (g->capture)
		gs_texrender_destroy(g->capture);
	if (g->blur_a)
		gs_texrender_destroy(g->blur_a);
	if (g->blur_b)
		gs_texrender_destroy(g->blur_b);
	g->effect = NULL;
	g->capture = g->blur_a = g->blur_b = NULL;
}

void fx_oglow_update(struct fx_outerglow *g, obs_data_t *settings)
{
	g->enabled = obs_data_get_bool(settings, "oglow_on");
	unpack_color((uint32_t)obs_data_get_int(settings, "oglow_color"),
		     g->rgba);
	g->rgba[3] = (float)obs_data_get_int(settings, "oglow_opacity") / 100.0f;
	g->blend = (int)obs_data_get_int(settings, "oglow_blend");
	g->size = (float)obs_data_get_int(settings, "oglow_size");
	g->spread = (float)obs_data_get_int(settings, "oglow_spread") / 100.0f;
	g->blur = (float)obs_data_get_int(settings, "oglow_blur") / 100.0f;
}

uint32_t fx_oglow_margin(const struct fx_outerglow *g)
{
	if (!g->enabled || g->rgba[3] <= 0.0f || g->size <= 0.5f)
		return 0u;
	return (uint32_t)ceilf(g->size) + 2u;
}

void fx_oglow_get_properties(obs_properties_t *props)
{
	obs_properties_t *grp = obs_properties_create();

	obs_properties_add_color(grp, "oglow_color",
				 obs_module_text("OgColor"));
	obs_properties_add_int_slider(grp, "oglow_opacity",
				      obs_module_text("OgOpacity"), 0, 100, 1);

	obs_property_t *bl = obs_properties_add_list(grp, "oglow_blend",
		obs_module_text("OgBlend"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bl, obs_module_text("OgBlendNormal"),
				  OGLOW_BLEND_NORMAL);
	obs_property_list_add_int(bl, obs_module_text("OgBlendScreen"),
				  OGLOW_BLEND_SCREEN);
	obs_property_list_add_int(bl, obs_module_text("OgBlendMultiply"),
				  OGLOW_BLEND_MULTIPLY);
	obs_property_list_add_int(bl, obs_module_text("OgBlendHardLight"),
				  OGLOW_BLEND_HARDLIGHT);
	obs_property_list_add_int(bl, obs_module_text("OgBlendColorBurn"),
				  OGLOW_BLEND_COLORBURN);

	obs_properties_add_int_slider(grp, "oglow_size",
				      obs_module_text("OgSize"), 1, 250, 1);
	obs_properties_add_int_slider(grp, "oglow_spread",
				      obs_module_text("OgSpread"), 0, 100, 1);
	obs_properties_add_int_slider(grp, "oglow_blur",
				      obs_module_text("OgBlur"), 0, 100, 1);

	obs_properties_add_group(props, "oglow_on",
				 obs_module_text("OuterGlow"),
				 OBS_GROUP_CHECKABLE, grp);
}

void fx_oglow_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "oglow_on", false);
	/* Photoshop's pale-yellow default #FFFFBE, as OBS 0xAABBGGRR. */
	obs_data_set_default_int(settings, "oglow_color", 0xFFBEFFFF);
	obs_data_set_default_int(settings, "oglow_opacity", 75);
	obs_data_set_default_int(settings, "oglow_blend", OGLOW_BLEND_SCREEN);
	obs_data_set_default_int(settings, "oglow_size", 30);
	obs_data_set_default_int(settings, "oglow_spread", 0);
	obs_data_set_default_int(settings, "oglow_blur", 100);
}

bool fx_oglow_begin(struct fx_outerglow *g, uint32_t w, uint32_t h)
{
	if (!g->enabled || !g->effect || g->rgba[3] <= 0.0f || g->size <= 0.5f)
		return false;

	if (!g->capture)
		g->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	/* 16f field textures keep the wide, slow gradients band-free. */
	if (!g->blur_a)
		g->blur_a = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	if (!g->blur_b)
		g->blur_b = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	if (!g->capture || !g->blur_a || !g->blur_b)
		return false;

	gs_texrender_reset(g->capture);
	if (!gs_texrender_begin(g->capture, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

/* One separable gaussian pass of `src`'s alpha along `dir`. Returns the
 * resulting field texture, or NULL when the target could not be set up. */
static gs_texture_t *blur_pass(struct fx_outerglow *g, gs_texrender_t *dst,
			       gs_texture_t *src, uint32_t w, uint32_t h,
			       bool horizontal)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return NULL;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	float step = g->size / (float)BLUR_TAPS;
	float sigma = g->size * (g->blur < 0.05f ? 0.05f : g->blur) / 2.5f;
	if (sigma < 0.4f)
		sigma = 0.4f;

	gs_effect_t *e = g->effect;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, src);
	if ((p = gs_effect_get_param_by_name(e, "dir"))) {
		struct vec2 d;
		if (horizontal)
			vec2_set(&d, step / (float)w, 0.0f);
		else
			vec2_set(&d, 0.0f, step / (float)h);
		gs_effect_set_vec2(p, &d);
	}
	if ((p = gs_effect_get_param_by_name(e, "step_px")))
		gs_effect_set_float(p, step);
	if ((p = gs_effect_get_param_by_name(e, "sigma")))
		gs_effect_set_float(p, sigma);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO); /* overwrite */
	while (gs_effect_loop(e, "Blur"))
		gs_draw_sprite(src, 0, w, h);
	gs_blend_state_pop();

	gs_texrender_end(dst);
	return gs_texrender_get_texture(dst);
}

/* Lay the halo down on the scene with the selected blend mode. The shader
 * passes only emit the source term; the actual arithmetic against what is
 * already behind the source lives in the GPU blend state.
 *
 * Hard light splits per channel (the branch depends only on the constant glow
 * colour): channels <= 0.5 darken as multiply-by-2c, channels > 0.5 lighten
 * as screen-with-(2c-1); colour write masks route each pass to its channels,
 * which makes the mode exact. Color burn's 1-(1-d)/c is not expressible with
 * blend factors (slope 1/c > 1), so it is approximated by squaring the
 * multiply term — same endpoints, steeper darkening in between. */
static void draw_glow(struct fx_outerglow *g, gs_texture_t *field, uint32_t w,
		      uint32_t h)
{
	gs_effect_t *e = g->effect;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, field);
	if ((p = gs_effect_get_param_by_name(e, "glow_color"))) {
		struct vec4 c;
		vec4_set(&c, g->rgba[0], g->rgba[1], g->rgba[2], g->rgba[3]);
		gs_effect_set_vec4(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "spread")))
		gs_effect_set_float(p, g->spread);

	gs_blend_state_push();
	switch (g->blend) {
	case OGLOW_BLEND_SCREEN:
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR,
					   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "GlowPre"))
			gs_draw_sprite(field, 0, w, h);
		break;
	case OGLOW_BLEND_MULTIPLY:
		gs_blend_function_separate(GS_BLEND_ZERO, GS_BLEND_SRCCOLOR,
					   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "GlowMultiply"))
			gs_draw_sprite(field, 0, w, h);
		break;
	case OGLOW_BLEND_HARDLIGHT: {
		bool dark[3] = {g->rgba[0] <= 0.5f, g->rgba[1] <= 0.5f,
				g->rgba[2] <= 0.5f};
		/* Dark channels (multiply by 2c); the alpha channel rides
		 * along here with a plain src-over. */
		gs_enable_color(dark[0], dark[1], dark[2], true);
		gs_blend_function_separate(GS_BLEND_ZERO, GS_BLEND_SRCCOLOR,
					   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "GlowHardDark"))
			gs_draw_sprite(field, 0, w, h);
		/* Light channels (screen with 2c-1). */
		gs_enable_color(!dark[0], !dark[1], !dark[2], false);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR);
		while (gs_effect_loop(e, "GlowHardLight"))
			gs_draw_sprite(field, 0, w, h);
		gs_enable_color(true, true, true, true);
		break;
	}
	case OGLOW_BLEND_COLORBURN:
		gs_blend_function_separate(GS_BLEND_ZERO, GS_BLEND_SRCCOLOR,
					   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "GlowBurn"))
			gs_draw_sprite(field, 0, w, h);
		break;
	case OGLOW_BLEND_NORMAL:
	default:
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA,
					   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(e, "GlowPre"))
			gs_draw_sprite(field, 0, w, h);
		break;
	}
	gs_blend_state_pop();
}

void fx_oglow_end(struct fx_outerglow *g, uint32_t w, uint32_t h)
{
	gs_texrender_end(g->capture);

	gs_texture_t *body = gs_texrender_get_texture(g->capture);
	if (!body)
		return;

	/* Glow first so it sits behind everything the effect drew. Run it
	 * best-effort: if a blur target fails, the body must still appear. */
	gs_texture_t *field = blur_pass(g, g->blur_a, body, w, h, true);
	if (field)
		field = blur_pass(g, g->blur_b, field, w, h, false);
	if (field)
		draw_glow(g, field, w, h);

	/* The capture holds premultiplied colour: composite it src-over. */
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *im = gs_effect_get_param_by_name(def, "image");
	if (im)
		gs_effect_set_texture(im, body);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(body, 0, w, h);
	gs_blend_state_pop();
}
