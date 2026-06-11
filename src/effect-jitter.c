#include "effect-jitter.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"
#include "flametext-glow.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_JIT_FONT 0xFFFFFFFFu      /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */

struct jitter_state {
	gs_effect_t *fill;
	gs_effect_t *outline;
	gs_effect_t *glow;

	uint32_t font_color;
	float    speed;  /* target hops per unit time            */
	int      mode;   /* 0 = linear glide, 1 = bouncy ease    */
	float    dist;   /* wander radius in px                  */
	float    active; /* shaking burst length when looping    */
	float    glow_amt;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;

	bool  loop;
	float wait;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *jitter_create(void)
{
	return bzalloc(sizeof(struct jitter_state));
}

static void jitter_destroy(void *data)
{
	struct jitter_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->outline)
		gs_effect_destroy(s->outline);
	if (s->glow)
		gs_effect_destroy(s->glow);
	bfree(s);
}

static void jitter_load_graphics(void *data)
{
	struct jitter_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "jitter: failed to load textfill.effect");
	s->outline = fx_outline_load();
	s->glow = fx_glow_load();
}

static void jitter_update(void *data, obs_data_t *settings)
{
	struct jitter_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "jit_font");
	s->speed = (float)obs_data_get_double(settings, "jit_speed");
	s->mode = (int)obs_data_get_int(settings, "jit_mode");
	s->dist = (float)obs_data_get_double(settings, "jit_dist");
	s->active = (float)obs_data_get_double(settings, "jit_active");
	s->glow_amt = (float)obs_data_get_double(settings, "jit_glow");
	s->outline_on = obs_data_get_bool(settings, "jit_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "jit_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "jit_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "jit_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "jit_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "jit_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "jit_shadow_y");
	s->loop = obs_data_get_bool(settings, "jit_loop");
	s->wait = (float)obs_data_get_double(settings, "jit_wait");
}

/* Stateless deterministic random in [-1, 1] from (glyph, step, axis), so a
 * glyph's wander path is reproducible every frame without per-glyph state. */
static float jitter_rand(int glyph, int step, int axis)
{
	uint32_t h = (uint32_t)glyph * 2654435761u +
		     (uint32_t)step * 2246822519u + (uint32_t)axis * 374761393u;
	h ^= h >> 13;
	h *= 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0xFFFFFF) / 8388607.5f - 1.0f;
}

/* Wander offset of glyph `i` at time `t`: the glyph glides between random
 * targets inside the wander disc, one hop per interval. Linear mode moves at
 * constant speed; bounce mode overshoots into each target and wobbles back
 * (ease-out-back). */
static void jitter_offset(const struct jitter_state *s, int i, float t,
			  float *dx, float *dy)
{
	float sp = s->speed < 0.05f ? 0.05f : s->speed;
	float interval = 0.35f / sp;
	float ft = t / interval;
	int step = (int)floorf(ft);
	float p = ft - (float)step;

	if (s->mode == 1) {
		const float c1 = 1.70158f;
		const float c3 = c1 + 1.0f;
		float q = p - 1.0f;
		p = 1.0f + c3 * q * q * q + c1 * q * q;
	}

	float x0 = jitter_rand(i, step, 0);
	float y0 = jitter_rand(i, step, 1);
	float x1 = jitter_rand(i, step + 1, 0);
	float y1 = jitter_rand(i, step + 1, 1);
	*dx = (x0 + (x1 - x0) * p) * s->dist;
	*dy = (y0 + (y1 - y0) * p) * s->dist;
}

static void jitter_render(void *data, const struct fx_render_ctx *ctx)
{
	struct jitter_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;

	/* Loop on: shake for `active` seconds, rest for `wait`, repeat. The
	 * envelope ramps over 0.25 s so the letters ease into and out of the
	 * rest pose instead of popping. Loop off shakes forever. */
	float env = 1.0f;
	if (s->loop) {
		float active = s->active < 0.1f ? 0.1f : s->active;
		float cycle = active + s->wait;
		float ct = fmodf(ctx->time, cycle);
		if (ct >= active) {
			env = 0.0f;
		} else {
			const float ramp = 0.25f;
			float in = ct / ramp;
			float out = (active - ct) / ramp;
			env = fminf(1.0f, fminf(in, out));
			if (env < 0.0f)
				env = 0.0f;
		}
	}

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);
	bool draw_outline = s->outline_on && s->outline;
	bool draw_glow = s->glow_amt > 0.001f && s->glow;
	bool draw_shadow = s->shadow_on && shrgba[3] > 0.0f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (draw_shadow) {
		for (int i = 0; i < n; ++i) {
			const struct flametext_glyph *g = &mask->glyphs[i];
			float dx, dy;
			jitter_offset(s, i, ctx->time, &dx, &dy);
			gs_matrix_push();
			gs_matrix_translate3f(
				g->x + dx * env + (float)s->shadow_x,
				g->y + dy * env + (float)s->shadow_y, 0.0f);
			fx_textfill_render_sub(s->fill, mask->tex, shrgba,
					       (uint32_t)g->x, (uint32_t)g->y,
					       (uint32_t)g->w, (uint32_t)g->h);
			gs_matrix_pop();
		}
	}

	for (int i = 0; i < n; ++i) {
		const struct flametext_glyph *g = &mask->glyphs[i];
		float dx, dy;
		jitter_offset(s, i, ctx->time, &dx, &dy);

		gs_matrix_push();
		gs_matrix_translate3f(g->x + dx * env, g->y + dy * env, 0.0f);
		if (draw_glow)
			fx_glow_render_sub(s->glow, mask->tex, ctx->width,
					   ctx->height, rgba, s->glow_amt,
					   (uint32_t)g->x, (uint32_t)g->y,
					   (uint32_t)g->w, (uint32_t)g->h);
		if (draw_outline)
			fx_outline_render_sub(s->outline, mask->tex, ctx->width,
					      ctx->height, orgba,
					      s->outline_width, (uint32_t)g->x,
					      (uint32_t)g->y, (uint32_t)g->w,
					      (uint32_t)g->h);
		fx_textfill_render_sub(s->fill, mask->tex, rgba, (uint32_t)g->x,
				       (uint32_t)g->y, (uint32_t)g->w,
				       (uint32_t)g->h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}

static void jitter_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "jit_font",
		obs_module_text("JitFontColor"));
	obs_properties_add_float_slider(p, "jit_speed",
		obs_module_text("JitSpeed"), 0.1, 5.0, 0.05);
	obs_property_t *m = obs_properties_add_list(p, "jit_mode",
		obs_module_text("JitMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(m, obs_module_text("JitModeLinear"), 0);
	obs_property_list_add_int(m, obs_module_text("JitModeBounce"), 1);
	obs_properties_add_float_slider(p, "jit_dist",
		obs_module_text("JitDist"), 1.0, 100.0, 0.5);
	obs_properties_add_float_slider(p, "jit_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "jit_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "jit_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "jit_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "jit_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "jit_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "jit_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "jit_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
	obs_properties_add_bool(p, "jit_loop", obs_module_text("LoopOn"));
	obs_properties_add_float_slider(p, "jit_active",
		obs_module_text("JitActive"), 0.5, 10.0, 0.1);
	obs_properties_add_float_slider(p, "jit_wait",
		obs_module_text("LoopWait"), 0.0, 10.0, 0.1);
}

static void jitter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "jit_font", DEFAULT_JIT_FONT);
	obs_data_set_default_double(settings, "jit_speed", 1.0);
	obs_data_set_default_int(settings, "jit_mode", 0);
	obs_data_set_default_double(settings, "jit_dist", 8.0);
	obs_data_set_default_double(settings, "jit_active", 2.0);
	obs_data_set_default_double(settings, "jit_glow", 0.0);
	obs_data_set_default_bool(settings, "jit_outline", false);
	obs_data_set_default_double(settings, "jit_outline_width", 4.0);
	obs_data_set_default_int(settings, "jit_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "jit_shadow", false);
	obs_data_set_default_int(settings, "jit_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "jit_shadow_x", 4);
	obs_data_set_default_int(settings, "jit_shadow_y", 4);
	obs_data_set_default_bool(settings, "jit_loop", false);
	obs_data_set_default_double(settings, "jit_wait", 1.5);
}

const struct text_effect fx_jitter = {
	.id             = "jitter",
	.name_key       = "EffectJitter",
	.create         = jitter_create,
	.destroy        = jitter_destroy,
	.load_graphics  = jitter_load_graphics,
	.update         = jitter_update,
	.render         = jitter_render,
	.get_properties = jitter_properties,
	.get_defaults   = jitter_defaults,
};
