#include "effect-flip.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"
#include "flametext-glow.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_FLIP_FONT 0xFFFFFFFFu     /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */
#define FLIP_PI 3.14159265f

/* One spin lasts FLIP_SPIN_DUR seconds (shortened when the wave interval is
 * smaller); adjacent glyphs start FLIP_STAGGER seconds apart. */
#define FLIP_SPIN_DUR 0.45f
#define FLIP_STAGGER 0.07f
#define FLIP_BACK_SHADE 0.55f /* brightness of the mirrored back face */

struct flip_state {
	gs_effect_t *fill;
	gs_effect_t *outline;
	gs_effect_t *glow;

	uint32_t font_color;
	float    freq;  /* spin waves per second           */
	int      order; /* 0 = from left, 1 = from right, 2 = random */
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

static void *flip_create(void)
{
	return bzalloc(sizeof(struct flip_state));
}

static void flip_destroy(void *data)
{
	struct flip_state *s = data;
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

static void flip_load_graphics(void *data)
{
	struct flip_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "flip: failed to load textfill.effect");
	s->outline = fx_outline_load();
	s->glow = fx_glow_load();
}

static void flip_update(void *data, obs_data_t *settings)
{
	struct flip_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "flip_font");
	s->freq = (float)obs_data_get_double(settings, "flip_freq");
	s->order = (int)obs_data_get_int(settings, "flip_order");
	s->glow_amt = (float)obs_data_get_double(settings, "flip_glow");
	s->outline_on = obs_data_get_bool(settings, "flip_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "flip_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "flip_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "flip_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "flip_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "flip_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "flip_shadow_y");
}

/* Stable pseudo-random fraction in [0,1) for glyph index `i`. */
static float flip_rand01(uint32_t i)
{
	uint32_t h = i * 2654435761u + 0x9E3779B9u;
	h ^= h >> 13;
	h *= 0x5BD1E995u;
	h ^= h >> 15;
	return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

/* Spin start delay of glyph `i` (of `n`) within each wave. */
static float flip_delay(const struct flip_state *s, int i, int n)
{
	switch (s->order) {
	case 1:
		return (float)(n - 1 - i) * FLIP_STAGGER;
	case 2:
		return flip_rand01((uint32_t)i) * (float)(n - 1) * FLIP_STAGGER;
	default:
		return (float)i * FLIP_STAGGER;
	}
}

/* Y-rotation angle in [0, 2pi). The glyph rests readable (angle 0) and does
 * one full spin lasting `spin` seconds once every `period` seconds. */
static float flip_angle_at(float t, float delay, float period, float spin)
{
	float ph = fmodf(t - delay, period);
	if (ph < 0.0f)
		ph += period;
	if (ph >= spin)
		return 0.0f;
	return ph / spin * (2.0f * FLIP_PI);
}

static void flip_render(void *data, const struct fx_render_ctx *ctx)
{
	struct flip_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;
	float freq = s->freq < 0.05f ? 0.05f : s->freq;
	float period = 1.0f / freq;
	float spin = FLIP_SPIN_DUR;
	if (spin > period * 0.8f)
		spin = period * 0.8f;

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);
	bool draw_outline = s->outline_on && s->outline;
	bool draw_glow = s->glow_amt > 0.001f && s->glow;
	bool draw_shadow = s->shadow_on && shrgba[3] > 0.0f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	/* Shadows first so every glyph's letterform stays above them. The
	 * shadow follows the spin (horizontal squeeze + mirror). */
	if (draw_shadow) {
		for (int i = 0; i < n; ++i) {
			const struct flametext_glyph *g = &mask->glyphs[i];
			float ang = flip_angle_at(ctx->time,
						  flip_delay(s, i, n), period,
						  spin);
			float sx = cosf(ang);
			if (fabsf(sx) < 0.02f)
				continue; /* edge-on sliver */
			float pivx = g->w * 0.5f;
			float pivy = g->h * 0.5f;
			gs_matrix_push();
			/* The shadow compresses about its own centre so the
			 * screen-space offset stays constant during the spin. */
			gs_matrix_translate3f(g->x + (float)s->shadow_x + pivx,
					      g->y + (float)s->shadow_y + pivy,
					      0.0f);
			gs_matrix_scale3f(sx, 1.0f, 1.0f);
			gs_matrix_translate3f(-pivx, -pivy, 0.0f);
			fx_textfill_render_sub(s->fill, mask->tex, shrgba,
					       (uint32_t)g->x, (uint32_t)g->y,
					       (uint32_t)g->w, (uint32_t)g->h);
			gs_matrix_pop();
		}
	}

	for (int i = 0; i < n; ++i) {
		const struct flametext_glyph *g = &mask->glyphs[i];
		float ang = flip_angle_at(ctx->time, flip_delay(s, i, n),
					  period, spin);
		/* cos < 0 = back half; the negative x-scale both squeezes the
		 * glyph (orthographic projection of the turn) and mirrors it. */
		float sx = cosf(ang);
		if (fabsf(sx) < 0.02f)
			continue; /* edge-on sliver */
		float shade = sx < 0.0f ? FLIP_BACK_SHADE : 1.0f;

		float pivx = g->w * 0.5f;
		float pivy = g->h * 0.5f;
		gs_matrix_push();
		gs_matrix_translate3f(g->x + pivx, g->y + pivy, 0.0f);
		gs_matrix_scale3f(sx, 1.0f, 1.0f);
		gs_matrix_translate3f(-pivx, -pivy, 0.0f);
		if (draw_glow) {
			float c[4] = {rgba[0] * shade, rgba[1] * shade,
				      rgba[2] * shade, rgba[3]};
			fx_glow_render_sub(s->glow, mask->tex, ctx->width,
					   ctx->height, c, s->glow_amt,
					   (uint32_t)g->x, (uint32_t)g->y,
					   (uint32_t)g->w, (uint32_t)g->h);
		}
		if (draw_outline) {
			float c[4] = {orgba[0] * shade, orgba[1] * shade,
				      orgba[2] * shade, orgba[3]};
			fx_outline_render_sub(s->outline, mask->tex, ctx->width,
					      ctx->height, c, s->outline_width,
					      (uint32_t)g->x, (uint32_t)g->y,
					      (uint32_t)g->w, (uint32_t)g->h);
		}
		float c[4] = {rgba[0] * shade, rgba[1] * shade,
			      rgba[2] * shade, rgba[3]};
		fx_textfill_render_sub(s->fill, mask->tex, c, (uint32_t)g->x,
				       (uint32_t)g->y, (uint32_t)g->w,
				       (uint32_t)g->h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}

static void flip_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "flip_font",
		obs_module_text("FlpFontColor"));
	obs_property_t *d = obs_properties_add_list(p, "flip_order",
		obs_module_text("FlpOrder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("FlpOrderL2R"), 0);
	obs_property_list_add_int(d, obs_module_text("FlpOrderR2L"), 1);
	obs_property_list_add_int(d, obs_module_text("FlpOrderRandom"), 2);
	obs_properties_add_float_slider(p, "flip_freq",
		obs_module_text("FlpFreq"), 0.05, 3.0, 0.05);
	obs_properties_add_float_slider(p, "flip_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "flip_outline",
		obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "flip_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "flip_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "flip_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "flip_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "flip_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "flip_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
}

static void flip_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "flip_font", DEFAULT_FLIP_FONT);
	obs_data_set_default_int(settings, "flip_order", 0);
	obs_data_set_default_double(settings, "flip_freq", 0.5);
	obs_data_set_default_double(settings, "flip_glow", 0.0);
	obs_data_set_default_bool(settings, "flip_outline", false);
	obs_data_set_default_double(settings, "flip_outline_width", 4.0);
	obs_data_set_default_int(settings, "flip_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "flip_shadow", false);
	obs_data_set_default_int(settings, "flip_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "flip_shadow_x", 4);
	obs_data_set_default_int(settings, "flip_shadow_y", 4);
}

const struct text_effect fx_flip = {
	.id             = "flip",
	.name_key       = "EffectFlip",
	.create         = flip_create,
	.destroy        = flip_destroy,
	.load_graphics  = flip_load_graphics,
	.update         = flip_update,
	.render         = flip_render,
	.get_properties = flip_properties,
	.get_defaults   = flip_defaults,
};
