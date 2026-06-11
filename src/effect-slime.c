#include "effect-slime.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SLM_FONT 0xFF7FE67Fu /* slime green #7FE67F */

struct slime_state {
	gs_effect_t *effect;

	uint32_t font_color;
	float    rate;   /* impulses per second (0 stops them) */
	float    soft;   /* 0 stiff .. 1 the whole body wobbles */
	float    sway;   /* horizontal wobble amount 0..1       */
	float    bounce; /* vertical wobble amount 0..1         */

	/* Two damped springs kicked by random impulses: q = vertical squash,
	 * u = horizontal sway, both dimensionless around 0. */
	float q, qv;
	float u, uv;
	float impulse_timer;
	uint32_t rng;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static float slime_frand(struct slime_state *s)
{
	uint32_t x = s->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s->rng = x;
	return (float)(x & 0xFFFFFF) / 16777216.0f;
}

static void *slime_create(void)
{
	struct slime_state *s = bzalloc(sizeof(struct slime_state));
	s->rng = 0x9E3779B9u;
	s->impulse_timer = 0.4f;
	return s;
}

static void slime_destroy(void *data)
{
	struct slime_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void slime_load_graphics(void *data)
{
	struct slime_state *s = data;
	char *path = obs_module_file("effects/slime.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load slime.effect (%s)",
				path);
	}
	bfree(path);
}

static void slime_update(void *data, obs_data_t *settings)
{
	struct slime_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "slm_color");
	s->rate = (float)obs_data_get_double(settings, "slm_rate");
	s->soft = (float)obs_data_get_double(settings, "slm_soft");
	s->sway = (float)obs_data_get_double(settings, "slm_sway");
	s->bounce = (float)obs_data_get_double(settings, "slm_bounce");
}

static void slime_reset(void *data)
{
	struct slime_state *s = data;
	s->q = s->qv = 0.0f;
	s->u = s->uv = 0.0f;
	s->impulse_timer = 0.4f;
}

static void slime_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	UNUSED_PARAMETER(ctx);
	struct slime_state *s = data;
	if (dt > 0.05f)
		dt = 0.05f; /* keep the integrator stable on hitches */

	/* Random impulses: a kick upward into the squash spring (the body
	 * compresses then rebounds) plus a sideways nudge. */
	if (s->rate > 0.01f) {
		s->impulse_timer -= dt;
		if (s->impulse_timer <= 0.0f) {
			s->qv += -(0.8f + 0.6f * slime_frand(s)) * 9.0f;
			s->uv += (slime_frand(s) * 2.0f - 1.0f) * 10.0f;
			s->impulse_timer =
				(0.5f + slime_frand(s)) / s->rate;
		}
	}

	/* Softer slime = a slower, less damped (jigglier) spring. */
	float omega = 11.0f - 6.5f * s->soft;
	float zeta = 0.30f - 0.20f * s->soft;
	float k = omega * omega;
	float c = 2.0f * zeta * omega;

	s->qv += (-k * s->q - c * s->qv) * dt;
	s->q += s->qv * dt;
	s->uv += (-k * s->u - c * s->uv) * dt;
	s->u += s->uv * dt;
}

static void set_float(gs_effect_t *e, const char *name, float f)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, f);
}

static void slime_render(void *data, const struct fx_render_ctx *ctx)
{
	struct slime_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;
	gs_effect_t *e = s->effect;

	float hgt = mask->text_bottom - mask->text_top;
	if (hgt < 1.0f)
		hgt = 1.0f;

	float q = s->q;
	if (q < -1.6f)
		q = -1.6f;
	if (q > 1.6f)
		q = 1.6f;
	float u = s->u;
	if (u < -1.6f)
		u = -1.6f;
	if (u > 1.6f)
		u = 1.6f;

	float rgba[4];
	unpack_color(s->font_color, rgba);

	gs_eparam_t *pi = gs_effect_get_param_by_name(e, "image");
	if (pi)
		gs_effect_set_texture(pi, mask->tex);
	gs_eparam_t *pc = gs_effect_get_param_by_name(e, "canvas");
	if (pc) {
		struct vec2 v;
		vec2_set(&v, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(pc, &v);
	}
	gs_eparam_t *pf = gs_effect_get_param_by_name(e, "font_color");
	if (pf) {
		struct vec4 v;
		vec4_set(&v, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(pf, &v);
	}
	set_float(e, "top", mask->text_top);
	set_float(e, "bottom", mask->text_bottom);
	set_float(e, "sway", s->sway * hgt * 0.10f * u);
	set_float(e, "squash", s->bounce * 0.22f * q);
	set_float(e, "softness", s->soft);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void slime_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "slm_color",
		obs_module_text("SlmColor"));
	obs_properties_add_float_slider(p, "slm_rate",
		obs_module_text("SlmRate"), 0.0, 5.0, 0.05);
	obs_properties_add_float_slider(p, "slm_soft",
		obs_module_text("SlmSoft"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "slm_sway",
		obs_module_text("SlmSway"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "slm_bounce",
		obs_module_text("SlmBounce"), 0.0, 1.0, 0.01);
}

static void slime_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "slm_color", DEFAULT_SLM_FONT);
	obs_data_set_default_double(settings, "slm_rate", 1.0);
	obs_data_set_default_double(settings, "slm_soft", 0.5);
	obs_data_set_default_double(settings, "slm_sway", 0.5);
	obs_data_set_default_double(settings, "slm_bounce", 0.5);
}

const struct text_effect fx_slime = {
	.id             = "slime",
	.name_key       = "EffectSlime",
	.create         = slime_create,
	.destroy        = slime_destroy,
	.load_graphics  = slime_load_graphics,
	.update         = slime_update,
	.tick           = slime_tick,
	.render         = slime_render,
	.reset          = slime_reset,
	.get_properties = slime_properties,
	.get_defaults   = slime_defaults,
};
