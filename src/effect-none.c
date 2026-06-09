#include "effect-none.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <plugin-support.h>

#define DEFAULT_NONE_FONT     0xFFFFFFFFu /* opaque white            */
#define DEFAULT_NONE_OUTLINE  0xFF000000u /* opaque black            */
#define DEFAULT_NONE_SHADOW   0x80000000u /* 50% black               */
#define DEFAULT_NONE_BG       0x00000000u /* transparent (off)       */

/* Plain text: a solid fill with optional outline, drop shadow and background.
 * Mirrors the controls of OBS's built-in text source. */
struct none_state {
	gs_effect_t *fill;
	gs_effect_t *outline;

	uint32_t font_color;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;

	uint32_t bg_color;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *none_create(void)
{
	return bzalloc(sizeof(struct none_state));
}

static void none_destroy(void *data)
{
	struct none_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->outline)
		gs_effect_destroy(s->outline);
	bfree(s);
}

static void none_load_graphics(void *data)
{
	struct none_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "none: failed to load textfill.effect");
	s->outline = fx_outline_load();
}

static void none_update(void *data, obs_data_t *settings)
{
	struct none_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "none_color");
	s->outline_on = obs_data_get_bool(settings, "none_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "none_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "none_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "none_shadow");
	s->shadow_color = (uint32_t)obs_data_get_int(settings, "none_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "none_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "none_shadow_y");
	s->bg_color = (uint32_t)obs_data_get_int(settings, "none_bg_color");
}

static void none_render(void *data, const struct fx_render_ctx *ctx)
{
	struct none_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex)
		return;

	/* Background: a solid rectangle filling the whole canvas. */
	float bg[4];
	unpack_color(s->bg_color, bg);
	if (bg[3] > 0.0f) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *col = gs_effect_get_param_by_name(solid, "color");
		/* Premultiplied so it composites like every other layer (the rest
		 * of the pipeline outputs and blends premultiplied alpha). */
		struct vec4 c;
		vec4_set(&c, bg[0] * bg[3], bg[1] * bg[3], bg[2] * bg[3], bg[3]);
		gs_effect_set_vec4(col, &c);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(NULL, 0, ctx->width, ctx->height);
		gs_blend_state_pop();
	}

	/* Drop shadow: the fill offset by (x, y) in the shadow colour. */
	if (s->shadow_on && s->fill) {
		float sh[4];
		unpack_color(s->shadow_color, sh);
		if (sh[3] > 0.0f) {
			gs_matrix_push();
			gs_matrix_translate3f((float)s->shadow_x,
					      (float)s->shadow_y, 0.0f);
			fx_textfill_render(s->fill, mask->tex, sh);
			gs_matrix_pop();
		}
	}

	/* Outline ring around the glyphs. */
	if (s->outline_on && s->outline) {
		float oc[4];
		unpack_color(s->outline_color, oc);
		fx_outline_render_full(s->outline, mask->tex, ctx->width,
				       ctx->height, oc, s->outline_width);
	}

	/* The text itself. */
	if (s->fill) {
		float fc[4];
		unpack_color(s->font_color, fc);
		fx_textfill_render(s->fill, mask->tex, fc);
	}
}

static void none_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "none_color",
		obs_module_text("NoneColor"));

	obs_properties_add_bool(p, "none_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "none_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "none_outline_color",
		obs_module_text("OutlineColor"));

	obs_properties_add_bool(p, "none_shadow", obs_module_text("NoneShadow"));
	obs_properties_add_color_alpha(p, "none_shadow_color",
		obs_module_text("NoneShadowColor"));
	obs_properties_add_int_slider(p, "none_shadow_x",
		obs_module_text("NoneShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "none_shadow_y",
		obs_module_text("NoneShadowY"), -100, 100, 1);

	obs_properties_add_color_alpha(p, "none_bg_color",
		obs_module_text("NoneBgColor"));
}

static void none_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "none_color", DEFAULT_NONE_FONT);

	obs_data_set_default_bool(settings, "none_outline", false);
	obs_data_set_default_double(settings, "none_outline_width", 4.0);
	obs_data_set_default_int(settings, "none_outline_color",
				 DEFAULT_NONE_OUTLINE);

	obs_data_set_default_bool(settings, "none_shadow", false);
	obs_data_set_default_int(settings, "none_shadow_color",
				 DEFAULT_NONE_SHADOW);
	obs_data_set_default_int(settings, "none_shadow_x", 4);
	obs_data_set_default_int(settings, "none_shadow_y", 4);

	obs_data_set_default_int(settings, "none_bg_color", DEFAULT_NONE_BG);
}

const struct text_effect fx_none = {
	.id             = "none",
	.name_key       = "EffectNone",
	.create         = none_create,
	.destroy        = none_destroy,
	.load_graphics  = none_load_graphics,
	.update         = none_update,
	.render         = none_render,
	.get_properties = none_properties,
	.get_defaults   = none_defaults,
};
