#include "effect-arc.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_ARC_FONT  0xFFFFFFFFu /* white #FFFFFF        */
#define DEFAULT_ARC_LIGHT 0xFFFFE9AAu /* electric blue #AAE9FF */

#define ARC_MAX 16 /* must match arc_x[4] (4 floats each) in arc.effect */

/* Per-instance state for the electric arc effect. */
struct arc_state {
	gs_effect_t *effect;

	uint32_t font_color;  /* base text fill (OBS 0xAABBGGRR) */
	uint32_t arc_color;   /* discharge tint                  */
	float    glow;        /* arc core brightness             */
	float    bloom;       /* halo strength                   */
	float    rate;        /* discharges per second           */
	float    random;      /* jaggedness / timing randomness  */

	/* Derived from the mask geometry (gaps between adjacent glyphs). */
	float    gap_x[ARC_MAX]; /* canvas-px x of each inter-letter gap */
	int      gap_count;
	float    span_top;
	float    span_bottom;
	float    amp; /* bolt horizontal jag amplitude (px) */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *arc_create(void)
{
	return bzalloc(sizeof(struct arc_state));
}

static void arc_destroy(void *data)
{
	struct arc_state *s = data;
	if (!s)
		return;
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s);
}

static void arc_load_graphics(void *data)
{
	struct arc_state *s = data;
	char *path = obs_module_file("effects/arc.effect");
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load arc.effect (%s)",
				path);
	}
	bfree(path);
}

static void arc_update(void *data, obs_data_t *settings)
{
	struct arc_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "arc_color_font");
	s->arc_color = (uint32_t)obs_data_get_int(settings, "arc_color");
	s->glow = (float)obs_data_get_double(settings, "arc_glow");
	s->bloom = (float)obs_data_get_double(settings, "arc_bloom");
	s->rate = (float)obs_data_get_double(settings, "arc_rate");
	s->random = (float)obs_data_get_double(settings, "arc_random");
}

/* Recompute inter-letter gap anchors when the text mask changes. */
static void arc_set_mask(void *data, const struct flametext_mask *mask)
{
	struct arc_state *s = data;
	s->gap_count = 0;
	if (!mask || mask->glyph_count < 2)
		return;

	s->span_top = mask->text_top;
	s->span_bottom = mask->text_bottom;
	s->amp = (mask->text_bottom - mask->text_top) * 0.12f;

	for (size_t i = 0; i + 1 < mask->glyph_count && s->gap_count < ARC_MAX;
	     ++i) {
		const struct flametext_glyph *a = &mask->glyphs[i];
		const struct flametext_glyph *b = &mask->glyphs[i + 1];
		float right = a->x + a->w;
		float left = b->x;
		s->gap_x[s->gap_count++] = (right + left) * 0.5f;
	}
}

static void arc_render(void *data, const struct fx_render_ctx *ctx)
{
	struct arc_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->effect)
		return;

	gs_effect_t *e = s->effect;
	gs_eparam_t *p;

	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, mask->tex);
	if ((p = gs_effect_get_param_by_name(e, "canvas"))) {
		struct vec2 c;
		vec2_set(&c, (float)ctx->width, (float)ctx->height);
		gs_effect_set_vec2(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "font_color"))) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &col);
	}
	if ((p = gs_effect_get_param_by_name(e, "arc_color"))) {
		float rgba[4];
		unpack_color(s->arc_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &col);
	}
	if ((p = gs_effect_get_param_by_name(e, "time")))
		gs_effect_set_float(p, fmodf(ctx->time, 1000.0f));
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, s->glow);
	if ((p = gs_effect_get_param_by_name(e, "bloom")))
		gs_effect_set_float(p, s->bloom);
	if ((p = gs_effect_get_param_by_name(e, "rate")))
		gs_effect_set_float(p, s->rate);
	if ((p = gs_effect_get_param_by_name(e, "random")))
		gs_effect_set_float(p, s->random);
	if ((p = gs_effect_get_param_by_name(e, "amp")))
		gs_effect_set_float(p, s->amp);
	if ((p = gs_effect_get_param_by_name(e, "arc_count")))
		gs_effect_set_float(p, (float)s->gap_count);
	if ((p = gs_effect_get_param_by_name(e, "span"))) {
		struct vec2 sp;
		vec2_set(&sp, s->span_top, s->span_bottom);
		gs_effect_set_vec2(p, &sp);
	}
	/* Gap x-positions packed as float4[4] (== 16 contiguous floats). */
	if ((p = gs_effect_get_param_by_name(e, "arc_x"))) {
		float packed[ARC_MAX];
		for (int i = 0; i < ARC_MAX; ++i)
			packed[i] = (i < s->gap_count) ? s->gap_x[i] : -1000.0f;
		gs_effect_set_val(p, packed, sizeof(packed));
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void arc_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "arc_color_font",
		obs_module_text("ArcFontColor"));
	obs_properties_add_color_alpha(p, "arc_color",
		obs_module_text("ArcColor"));
	obs_properties_add_float_slider(p, "arc_glow",
		obs_module_text("ArcGlow"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "arc_bloom",
		obs_module_text("ArcBloom"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "arc_rate",
		obs_module_text("ArcRate"), 0.0, 20.0, 0.1);
	obs_properties_add_float_slider(p, "arc_random",
		obs_module_text("ArcRandom"), 0.0, 1.0, 0.01);
}

static void arc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "arc_color_font", DEFAULT_ARC_FONT);
	obs_data_set_default_int(settings, "arc_color", DEFAULT_ARC_LIGHT);
	obs_data_set_default_double(settings, "arc_glow", 1.2);
	obs_data_set_default_double(settings, "arc_bloom", 1.0);
	obs_data_set_default_double(settings, "arc_rate", 6.0);
	obs_data_set_default_double(settings, "arc_random", 0.6);
}

const struct text_effect fx_arc = {
	.id             = "arc",
	.name_key       = "EffectArc",
	.create         = arc_create,
	.destroy        = arc_destroy,
	.load_graphics  = arc_load_graphics,
	.update         = arc_update,
	.set_mask       = arc_set_mask,
	.render         = arc_render,
	.get_properties = arc_properties,
	.get_defaults   = arc_defaults,
};
