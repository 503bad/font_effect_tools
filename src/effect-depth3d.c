#include "effect-depth3d.h"
#include "flametext-sprites.h" /* fx_textfill_* */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_D3_FONT 0xFFFFFFFFu /* white #FFFFFF        */
#define DEFAULT_D3_SIDE 0xFF606060u /* dim grey side #606060 */
#define D3_LAYERS 10
#define D3_PI 3.14159265f

struct depth3d_state {
	gs_effect_t *fill;

	uint32_t font_color;
	uint32_t side_color;
	float    thickness; /* extrusion depth in px       */
	float    sway;      /* sway amplitude 0..1          */
	float    bow_freq;  /* bows per second (0 = none)   */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *depth3d_create(void)
{
	return bzalloc(sizeof(struct depth3d_state));
}

static void depth3d_destroy(void *data)
{
	struct depth3d_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	bfree(s);
}

static void depth3d_load_graphics(void *data)
{
	struct depth3d_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "depth3d: failed to load textfill.effect");
}

static void depth3d_update(void *data, obs_data_t *settings)
{
	struct depth3d_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "d3_font");
	s->side_color = (uint32_t)obs_data_get_int(settings, "d3_side");
	s->thickness = (float)obs_data_get_double(settings, "d3_thickness");
	s->sway = (float)obs_data_get_double(settings, "d3_sway");
	s->bow_freq = (float)obs_data_get_double(settings, "d3_bow_freq");
}

/* Periodic bow envelope: 0 most of the time, rising to ~1 (flat forward) for a
 * short window each cycle. */
static float bow_angle(float t, float freq)
{
	if (freq <= 0.0f)
		return 0.0f;
	float cycle = 1.0f / freq;
	float tt = fmodf(t, cycle);
	float dur = cycle < 1.2f ? cycle * 0.8f : 1.2f; /* bow duration */
	if (tt >= dur)
		return 0.0f;
	float e = sinf(D3_PI * (tt / dur)); /* 0 -> 1 -> 0 */
	return e * (D3_PI * 0.5f) * 0.95f;  /* up to ~85 degrees */
}

static void depth3d_render(void *data, const struct fx_render_ctx *ctx)
{
	struct depth3d_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill)
		return;

	float cx = (float)ctx->width * 0.5f;
	float baseY = mask->text_bottom; /* pivot at the baseline */

	float sway = sinf(ctx->time * 1.3f) * s->sway * (D3_PI / 180.0f) * 18.0f;
	float bow = bow_angle(ctx->time, s->bow_freq);

	float off = (D3_LAYERS > 0) ? s->thickness / (float)D3_LAYERS : 0.0f;

	float frgba[4], srgba[4];
	unpack_color(s->font_color, frgba);
	unpack_color(s->side_color, srgba);

	gs_matrix_push();
	gs_matrix_translate3f(cx, baseY, 0.0f);
	gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, sway); /* gentle turn */
	gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f, bow);  /* bow forward */
	gs_matrix_translate3f(-cx, -baseY, 0.0f);

	/* Back-to-front extruded layers. */
	for (int i = D3_LAYERS; i >= 1; --i) {
		gs_matrix_push();
		gs_matrix_translate3f(off * (float)i, off * (float)i, 0.0f);
		fx_textfill_render(s->fill, mask->tex, srgba);
		gs_matrix_pop();
	}
	/* Front face. */
	fx_textfill_render(s->fill, mask->tex, frgba);

	gs_matrix_pop();
}

static void depth3d_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "d3_font",
		obs_module_text("D3FontColor"));
	obs_properties_add_color_alpha(p, "d3_side",
		obs_module_text("D3SideColor"));
	obs_properties_add_float_slider(p, "d3_thickness",
		obs_module_text("D3Thickness"), 0.0, 40.0, 0.5);
	obs_properties_add_float_slider(p, "d3_sway",
		obs_module_text("D3Sway"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "d3_bow_freq",
		obs_module_text("D3BowFreq"), 0.0, 1.0, 0.01);
}

static void depth3d_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "d3_font", DEFAULT_D3_FONT);
	obs_data_set_default_int(settings, "d3_side", DEFAULT_D3_SIDE);
	obs_data_set_default_double(settings, "d3_thickness", 14.0);
	obs_data_set_default_double(settings, "d3_sway", 0.4);
	obs_data_set_default_double(settings, "d3_bow_freq", 0.15);
}

const struct text_effect fx_depth3d = {
	.id             = "depth3d",
	.name_key       = "EffectDepth3D",
	.create         = depth3d_create,
	.destroy        = depth3d_destroy,
	.load_graphics  = depth3d_load_graphics,
	.update         = depth3d_update,
	.render         = depth3d_render,
	.get_properties = depth3d_properties,
	.get_defaults   = depth3d_defaults,
};
