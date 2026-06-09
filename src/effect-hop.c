#include "effect-hop.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_HOP_FONT 0xFFFFFFFFu /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black */
#define HOP_PI 3.14159265f

struct hop_state {
	gs_effect_t *fill;
	gs_effect_t *outline;

	uint32_t font_color;
	int      dir;     /* 0 = from left, 1 = from right */
	float    speed;
	int      hops;    /* hops per entrance/exit         */
	float    height;  /* hop height (fraction of text)  */
	float    squash;  /* squash & stretch amount 0..1   */
	float    hold;    /* seconds held in place          */

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *hop_create(void)
{
	return bzalloc(sizeof(struct hop_state));
}

static void hop_destroy(void *data)
{
	struct hop_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->outline)
		gs_effect_destroy(s->outline);
	bfree(s);
}

static void hop_load_graphics(void *data)
{
	struct hop_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "hop: failed to load textfill.effect");
	s->outline = fx_outline_load();
}

static void hop_update(void *data, obs_data_t *settings)
{
	struct hop_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "hop_font");
	s->dir = (int)obs_data_get_int(settings, "hop_dir");
	s->speed = (float)obs_data_get_double(settings, "hop_speed");
	s->hops = (int)obs_data_get_int(settings, "hop_count");
	s->height = (float)obs_data_get_double(settings, "hop_height");
	s->squash = (float)obs_data_get_double(settings, "hop_squash");
	s->hold = (float)obs_data_get_double(settings, "hop_hold");
	s->outline_on = obs_data_get_bool(settings, "hop_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "hop_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "hop_outline_color");
}

/* Vertical hop offset (<=0, up) and squash/stretch scales for a 0..1 progress
 * across `hops` jumps. */
static void hop_pose(float prog, int hops, float height, float squash,
		     float text_h, float *yoff, float *sx, float *sy)
{
	if (hops < 1)
		hops = 1;
	float hp = prog * (float)hops;
	float frac = hp - floorf(hp);
	float air = sinf(HOP_PI * frac); /* 0 ground -> 1 apex -> 0 */
	float ground = 1.0f - air;

	*yoff = -height * text_h * air;
	float sYy = 1.0f + squash * (air * 0.35f - ground * ground * 0.45f);
	if (sYy < 0.2f)
		sYy = 0.2f;
	*sy = sYy;
	*sx = 1.0f / sYy; /* preserve volume-ish */
}

static void hop_render(void *data, const struct fx_render_ctx *ctx)
{
	struct hop_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;
	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.8f / sp;
	float exit_dur = 0.7f / sp;
	float stag = 0.12f / sp;

	float all_in = (float)(n - 1) * stag + enter_dur;
	float hold_end = all_in + s->hold;
	float exit_all = (float)(n - 1) * stag + exit_dur;
	float cycle = hold_end + exit_all + 0.3f;

	float lt = fmodf(ctx->time, cycle);

	float off = (float)ctx->width;
	float enter_from = (s->dir == 1) ? off : -off;
	float exit_to = -enter_from;
	float text_h = mask->text_bottom - mask->text_top;

	float rgba[4], orgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	bool draw_outline = s->outline_on && s->outline;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	for (int i = 0; i < n; ++i) {
		const struct flametext_glyph *g = &mask->glyphs[i];
		float es = (float)i * stag;
		float ee = es + enter_dur;
		float xs = hold_end + (float)i * stag;
		float xe = xs + exit_dur;

		float dx, dy = 0.0f, sx = 1.0f, sy = 1.0f;
		if (lt < es) {
			dx = enter_from;
		} else if (lt < ee) {
			float p = (lt - es) / enter_dur;
			float e = 1.0f - (1.0f - p) * (1.0f - p); /* ease out */
			dx = enter_from * (1.0f - e);
			hop_pose(p, s->hops, s->height, s->squash, text_h, &dy,
				 &sx, &sy);
		} else if (lt < xs) {
			dx = 0.0f;
		} else if (lt < xe) {
			float p = (lt - xs) / exit_dur;
			float e = p * p; /* ease in */
			dx = exit_to * e;
			hop_pose(p, s->hops, s->height, s->squash, text_h, &dy,
				 &sx, &sy);
		} else {
			dx = exit_to;
		}

		/* Squash about the glyph's feet (bottom-centre). */
		float pivx = g->w * 0.5f;
		float pivy = g->h;

		gs_matrix_push();
		gs_matrix_translate3f(g->x + dx, g->y + dy, 0.0f);
		gs_matrix_translate3f(pivx, pivy, 0.0f);
		gs_matrix_scale3f(sx, sy, 1.0f);
		gs_matrix_translate3f(-pivx, -pivy, 0.0f);
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

static void hop_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "hop_font",
		obs_module_text("HopFontColor"));
	obs_property_t *d = obs_properties_add_list(p, "hop_dir",
		obs_module_text("HopDir"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("HopFromLeft"), 0);
	obs_property_list_add_int(d, obs_module_text("HopFromRight"), 1);
	obs_properties_add_float_slider(p, "hop_speed",
		obs_module_text("HopSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_int_slider(p, "hop_count",
		obs_module_text("HopCount"), 1, 6, 1);
	obs_properties_add_float_slider(p, "hop_height",
		obs_module_text("HopHeight"), 0.1, 1.5, 0.05);
	obs_properties_add_float_slider(p, "hop_squash",
		obs_module_text("HopSquash"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "hop_hold",
		obs_module_text("HopHold"), 0.0, 6.0, 0.1);
	obs_properties_add_bool(p, "hop_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "hop_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "hop_outline_color",
		obs_module_text("OutlineColor"));
}

static void hop_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "hop_font", DEFAULT_HOP_FONT);
	obs_data_set_default_int(settings, "hop_dir", 0);
	obs_data_set_default_double(settings, "hop_speed", 1.0);
	obs_data_set_default_int(settings, "hop_count", 3);
	obs_data_set_default_double(settings, "hop_height", 0.6);
	obs_data_set_default_double(settings, "hop_squash", 0.5);
	obs_data_set_default_double(settings, "hop_hold", 1.5);
	obs_data_set_default_bool(settings, "hop_outline", false);
	obs_data_set_default_double(settings, "hop_outline_width", 4.0);
	obs_data_set_default_int(settings, "hop_outline_color",
				 DEFAULT_OUTLINE_COLOR);
}

const struct text_effect fx_hop = {
	.id             = "hop",
	.name_key       = "EffectHop",
	.create         = hop_create,
	.destroy        = hop_destroy,
	.load_graphics  = hop_load_graphics,
	.update         = hop_update,
	.render         = hop_render,
	.get_properties = hop_properties,
	.get_defaults   = hop_defaults,
};
