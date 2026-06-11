#include "effect-slidein.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"
#include "flametext-glow.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SLD_FONT 0xFFFFFFFFu      /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */
#define SLD_PI 3.14159265f

struct slidein_state {
	gs_effect_t *fill;
	gs_effect_t *outline;
	gs_effect_t *glow;

	uint32_t font_color;
	int      start;  /* 0 = stagger from left, 1 = from right */
	float    angle;  /* travel tilt in degrees (0 = vertical) */
	float    speed;
	float    dist;   /* travel distance in px                 */
	float    glow_amt;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;

	bool  loop;
	float wait; /* seconds held assembled before sliding out */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *slidein_create(void)
{
	return bzalloc(sizeof(struct slidein_state));
}

static void slidein_destroy(void *data)
{
	struct slidein_state *s = data;
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

static void slidein_load_graphics(void *data)
{
	struct slidein_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "slidein: failed to load textfill.effect");
	s->outline = fx_outline_load();
	s->glow = fx_glow_load();
}

static void slidein_update(void *data, obs_data_t *settings)
{
	struct slidein_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "sld_font");
	s->start = (int)obs_data_get_int(settings, "sld_start");
	s->angle = (float)obs_data_get_double(settings, "sld_angle");
	s->speed = (float)obs_data_get_double(settings, "sld_speed");
	s->dist = (float)obs_data_get_double(settings, "sld_dist");
	s->glow_amt = (float)obs_data_get_double(settings, "sld_glow");
	s->outline_on = obs_data_get_bool(settings, "sld_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "sld_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "sld_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "sld_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "sld_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "sld_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "sld_shadow_y");
	s->loop = obs_data_get_bool(settings, "sld_loop");
	s->wait = (float)obs_data_get_double(settings, "sld_wait");
}

/* Offset and opacity of glyph `i` at loop-local time `lt`. The stagger order
 * runs from the chosen side; glyphs alternate entering from above and below
 * along the tilted travel direction. The exit replays the entrance in
 * reverse (each glyph slides back out the way it came). */
static void slidein_pose(const struct slidein_state *s, int i, int n, float lt,
			 float enter_dur, float stag, float hold_end,
			 float *dx, float *dy, float *alpha)
{
	int oi = (s->start == 1) ? (n - 1 - i) : i;
	float vsgn = (oi & 1) ? 1.0f : -1.0f; /* even order: from above */

	float arad = s->angle * SLD_PI / 180.0f;
	float fx = s->dist * sinf(arad) * vsgn;
	float fy = s->dist * cosf(arad) * vsgn;

	float es = (float)oi * stag;
	float ee = es + enter_dur;
	float xs = hold_end + (float)oi * stag;
	float xe = xs + enter_dur;

	if (lt < es) {
		*dx = fx;
		*dy = fy;
		*alpha = 0.0f;
	} else if (lt < ee) {
		float p = (lt - es) / enter_dur;
		float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
		*dx = fx * (1.0f - e);
		*dy = fy * (1.0f - e);
		*alpha = p;
	} else if (lt < xs) {
		*dx = 0.0f;
		*dy = 0.0f;
		*alpha = 1.0f;
	} else if (lt < xe) {
		float p = (lt - xs) / enter_dur;
		float e = p * p * p;
		*dx = fx * e;
		*dy = fy * e;
		*alpha = 1.0f - p;
	} else {
		*dx = fx;
		*dy = fy;
		*alpha = 0.0f;
	}
}

static void slidein_render(void *data, const struct fx_render_ctx *ctx)
{
	struct slidein_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;
	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.55f / sp;
	float stag = 0.14f / sp;

	float all_in = (float)(n - 1) * stag + enter_dur;
	float hold_end = all_in + s->wait;
	float exit_all = (float)(n - 1) * stag + enter_dur;
	float cycle = hold_end + exit_all + 0.3f;

	/* Loop off: enter once and stay assembled. */
	float lt = s->loop ? fmodf(ctx->time, cycle)
			   : fminf(ctx->time, all_in);

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);
	bool draw_outline = s->outline_on && s->outline;
	bool draw_glow = s->glow_amt > 0.001f && s->glow;
	bool draw_shadow = s->shadow_on && shrgba[3] > 0.0f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	/* Shadows first so every glyph's letterform stays above them. */
	if (draw_shadow) {
		for (int i = 0; i < n; ++i) {
			const struct flametext_glyph *g = &mask->glyphs[i];
			float dx, dy, a;
			slidein_pose(s, i, n, lt, enter_dur, stag, hold_end,
				     &dx, &dy, &a);
			if (a <= 0.0f)
				continue;
			float c[4] = {shrgba[0], shrgba[1], shrgba[2],
				      shrgba[3] * a};
			gs_matrix_push();
			gs_matrix_translate3f(g->x + dx + (float)s->shadow_x,
					      g->y + dy + (float)s->shadow_y,
					      0.0f);
			fx_textfill_render_sub(s->fill, mask->tex, c,
					       (uint32_t)g->x, (uint32_t)g->y,
					       (uint32_t)g->w, (uint32_t)g->h);
			gs_matrix_pop();
		}
	}

	for (int i = 0; i < n; ++i) {
		const struct flametext_glyph *g = &mask->glyphs[i];
		float dx, dy, a;
		slidein_pose(s, i, n, lt, enter_dur, stag, hold_end, &dx, &dy,
			     &a);
		if (a <= 0.0f)
			continue;

		gs_matrix_push();
		gs_matrix_translate3f(g->x + dx, g->y + dy, 0.0f);
		if (draw_glow) {
			float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * a};
			fx_glow_render_sub(s->glow, mask->tex, ctx->width,
					   ctx->height, c, s->glow_amt,
					   (uint32_t)g->x, (uint32_t)g->y,
					   (uint32_t)g->w, (uint32_t)g->h);
		}
		if (draw_outline) {
			float c[4] = {orgba[0], orgba[1], orgba[2],
				      orgba[3] * a};
			fx_outline_render_sub(s->outline, mask->tex, ctx->width,
					      ctx->height, c, s->outline_width,
					      (uint32_t)g->x, (uint32_t)g->y,
					      (uint32_t)g->w, (uint32_t)g->h);
		}
		float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * a};
		fx_textfill_render_sub(s->fill, mask->tex, c, (uint32_t)g->x,
				       (uint32_t)g->y, (uint32_t)g->w,
				       (uint32_t)g->h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}

static void slidein_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "sld_font",
		obs_module_text("SldFontColor"));
	obs_property_t *d = obs_properties_add_list(p, "sld_start",
		obs_module_text("SldStart"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("SldFromLeft"), 0);
	obs_property_list_add_int(d, obs_module_text("SldFromRight"), 1);
	obs_properties_add_float_slider(p, "sld_angle",
		obs_module_text("SldAngle"), -80.0, 80.0, 1.0);
	obs_properties_add_float_slider(p, "sld_speed",
		obs_module_text("SldSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "sld_dist",
		obs_module_text("SldDist"), 20.0, 2000.0, 1.0);
	obs_properties_add_float_slider(p, "sld_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "sld_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "sld_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "sld_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "sld_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "sld_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "sld_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "sld_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
	obs_properties_add_bool(p, "sld_loop", obs_module_text("LoopOn"));
	obs_properties_add_float_slider(p, "sld_wait",
		obs_module_text("LoopWait"), 0.0, 10.0, 0.1);
}

static void slidein_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "sld_font", DEFAULT_SLD_FONT);
	obs_data_set_default_int(settings, "sld_start", 0);
	obs_data_set_default_double(settings, "sld_angle", 0.0);
	obs_data_set_default_double(settings, "sld_speed", 1.0);
	obs_data_set_default_double(settings, "sld_dist", 200.0);
	obs_data_set_default_double(settings, "sld_glow", 0.0);
	obs_data_set_default_bool(settings, "sld_outline", false);
	obs_data_set_default_double(settings, "sld_outline_width", 4.0);
	obs_data_set_default_int(settings, "sld_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "sld_shadow", false);
	obs_data_set_default_int(settings, "sld_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "sld_shadow_x", 4);
	obs_data_set_default_int(settings, "sld_shadow_y", 4);
	obs_data_set_default_bool(settings, "sld_loop", true);
	obs_data_set_default_double(settings, "sld_wait", 1.5);
}

const struct text_effect fx_slidein = {
	.id             = "slidein",
	.name_key       = "EffectSlideIn",
	.create         = slidein_create,
	.destroy        = slidein_destroy,
	.load_graphics  = slidein_load_graphics,
	.update         = slidein_update,
	.render         = slidein_render,
	.get_properties = slidein_properties,
	.get_defaults   = slidein_defaults,
};
