#include "flametext-glow.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <plugin-support.h>

/* Halo radius for a given intensity; grows with the strength so stronger
 * glows also reach further, like the per-effect bloom sliders. */
static float glow_radius(float intensity)
{
	float r = 6.0f + 9.0f * intensity;
	return r > 34.0f ? 34.0f : r;
}

gs_effect_t *fx_glow_load(void)
{
	char *path = obs_module_file("effects/glow.effect");
	gs_effect_t *e = NULL;
	if (path) {
		e = gs_effect_create_from_file(path, NULL);
		if (!e)
			obs_log(LOG_ERROR, "failed to load glow.effect (%s)",
				path);
	}
	bfree(path);
	return e;
}

static void set_params(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
		       uint32_t ch, const float rgba[4], float radius,
		       float intensity)
{
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, tex);
	if ((p = gs_effect_get_param_by_name(e, "canvas"))) {
		struct vec2 c;
		vec2_set(&c, (float)cw, (float)ch);
		gs_effect_set_vec2(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "glow_color"))) {
		struct vec4 c;
		vec4_set(&c, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "radius")))
		gs_effect_set_float(p, radius);
	if ((p = gs_effect_get_param_by_name(e, "intensity")))
		gs_effect_set_float(p, intensity);
}

void fx_glow_render_full(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			 uint32_t ch, const float rgba[4], float intensity)
{
	if (!e || !tex || rgba[3] <= 0.0f || intensity <= 0.0f)
		return;
	set_params(e, tex, cw, ch, rgba, glow_radius(intensity), intensity);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_blend_state_pop();
}

void fx_glow_render_sub(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			uint32_t ch, const float rgba[4], float intensity,
			uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
	if (!e || !tex || rgba[3] <= 0.0f || intensity <= 0.0f)
		return;
	float radius = glow_radius(intensity);
	set_params(e, tex, cw, ch, rgba, radius, intensity);

	/* Quad grown by the halo radius (plus a margin for the sampling disc)
	 * so the haze is not clipped at the glyph rect. Local coords start at
	 * the glyph's top-left, matching fx_textfill_render_sub. */
	float m = radius + 2.0f;
	float lx0 = -m, ly0 = -m;
	float lx1 = (float)cx + m, ly1 = (float)cy + m;
	float u0 = ((float)x - m) / (float)cw;
	float v0 = ((float)y - m) / (float)ch;
	float u1 = ((float)x + (float)cx + m) / (float)cw;
	float v1 = ((float)y + (float)cy + m) / (float)ch;

	float px[4] = {lx0, lx1, lx1, lx0};
	float py[4] = {ly0, ly0, ly1, ly1};
	float ux[4] = {u0, u1, u1, u0};
	float uy[4] = {v0, v0, v1, v1};
	int idx[6] = {0, 1, 2, 0, 2, 3};

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw")) {
		gs_render_start(true);
		for (int t = 0; t < 6; ++t) {
			int k = idx[t];
			gs_texcoord(ux[k], uy[k], 0);
			gs_vertex2f(px[k], py[k]);
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}
