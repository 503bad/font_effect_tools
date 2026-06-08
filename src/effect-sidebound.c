#include "effect-sidebound.h"
#include "flametext-sprites.h" /* fx_textfill_* */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SB_FONT 0xFFFFFFFFu /* white #FFFFFF */
#define SB_PI 3.14159265f

struct sidebound_state {
	gs_effect_t *fill;

	uint32_t font_color;
	int      dir;    /* 0 = from left, 1 = from right */
	float    speed;
	float    bounce; /* overshoot amount 0..1          */
	float    hold;   /* seconds held in place          */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *sidebound_create(void)
{
	return bzalloc(sizeof(struct sidebound_state));
}

static void sidebound_destroy(void *data)
{
	struct sidebound_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	bfree(s);
}

static void sidebound_load_graphics(void *data)
{
	struct sidebound_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "sidebound: failed to load textfill.effect");
}

static void sidebound_update(void *data, obs_data_t *settings)
{
	struct sidebound_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "sb_font");
	s->dir = (int)obs_data_get_int(settings, "sb_dir");
	s->speed = (float)obs_data_get_double(settings, "sb_speed");
	s->bounce = (float)obs_data_get_double(settings, "sb_bounce");
	s->hold = (float)obs_data_get_double(settings, "sb_hold");
}

/* Ease-out-back: overshoots past 1 then settles, like a bouncy collision. */
static float ease_out_back(float p, float overshoot)
{
	float c1 = 1.70158f * (0.3f + 1.7f * overshoot);
	float c3 = c1 + 1.0f;
	float d = p - 1.0f;
	return 1.0f + c3 * d * d * d + c1 * d * d;
}

static void sidebound_render(void *data, const struct fx_render_ctx *ctx)
{
	struct sidebound_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;
	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.55f / sp;
	float exit_dur = 0.45f / sp;
	float stag = 0.10f / sp;

	float all_in = (float)(n - 1) * stag + enter_dur;
	float hold_end = all_in + s->hold;
	float exit_all = (float)(n - 1) * stag + exit_dur;
	float cycle = hold_end + exit_all + 0.3f;

	float lt = fmodf(ctx->time, cycle);

	float off = (float)ctx->width; /* off-screen distance */
	float enter_from = (s->dir == 1) ? off : -off; /* right vs left */
	float exit_to = -enter_from;                   /* opposite side */
	float text_h = mask->text_bottom - mask->text_top;

	float rgba[4];
	unpack_color(s->font_color, rgba);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	for (int i = 0; i < n; ++i) {
		const struct flametext_glyph *g = &mask->glyphs[i];
		float es = (float)i * stag;
		float ee = es + enter_dur;
		float xs = hold_end + (float)i * stag;
		float xe = xs + exit_dur;

		float dx, dy = 0.0f;
		if (lt < es) {
			dx = enter_from;
		} else if (lt < ee) {
			float p = (lt - es) / enter_dur;
			float e = ease_out_back(p, s->bounce);
			dx = enter_from * (1.0f - e);
			dy = -text_h * 0.18f * s->bounce * sinf(SB_PI * p);
		} else if (lt < xs) {
			dx = 0.0f;
		} else if (lt < xe) {
			float p = (lt - xs) / exit_dur;
			float e = p * p * p; /* ease-in */
			dx = exit_to * e;
		} else {
			dx = exit_to;
		}

		gs_matrix_push();
		gs_matrix_translate3f(g->x + dx, g->y + dy, 0.0f);
		fx_textfill_render_sub(s->fill, mask->tex, rgba, (uint32_t)g->x,
				       (uint32_t)g->y, (uint32_t)g->w,
				       (uint32_t)g->h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}

static void sidebound_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "sb_font",
		obs_module_text("SbFontColor"));
	obs_property_t *d = obs_properties_add_list(p, "sb_dir",
		obs_module_text("SbDir"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("SbFromLeft"), 0);
	obs_property_list_add_int(d, obs_module_text("SbFromRight"), 1);
	obs_properties_add_float_slider(p, "sb_speed",
		obs_module_text("SbSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "sb_bounce",
		obs_module_text("SbBounce"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "sb_hold",
		obs_module_text("SbHold"), 0.0, 6.0, 0.1);
}

static void sidebound_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "sb_font", DEFAULT_SB_FONT);
	obs_data_set_default_int(settings, "sb_dir", 0);
	obs_data_set_default_double(settings, "sb_speed", 1.0);
	obs_data_set_default_double(settings, "sb_bounce", 0.6);
	obs_data_set_default_double(settings, "sb_hold", 1.5);
}

const struct text_effect fx_sidebound = {
	.id             = "sidebound",
	.name_key       = "EffectSidebound",
	.create         = sidebound_create,
	.destroy        = sidebound_destroy,
	.load_graphics  = sidebound_load_graphics,
	.update         = sidebound_update,
	.render         = sidebound_render,
	.get_properties = sidebound_properties,
	.get_defaults   = sidebound_defaults,
};
