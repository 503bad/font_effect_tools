#include "effect-roll.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_ROLL_FONT 0xFFFFFFFFu     /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define ROLL_PI 3.14159265f
#define ROLL_TWO_PI 6.2831853f

struct roll_state {
	gs_effect_t *fill;
	gs_effect_t *outline;

	uint32_t font_color;
	int      dir;     /* 0 = roll in from left, 1 = from right */
	float    speed;
	float    bounce;  /* bounce height (fraction of text height) */
	bool     loop;
	bool     exit;    /* roll out on exit                        */
	bool     fade_in;
	bool     fade_out;
	float    hold;    /* seconds held assembled                  */

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

static void *roll_create(void)
{
	return bzalloc(sizeof(struct roll_state));
}

static void roll_destroy(void *data)
{
	struct roll_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->outline)
		gs_effect_destroy(s->outline);
	bfree(s);
}

static void roll_load_graphics(void *data)
{
	struct roll_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "roll: failed to load textfill.effect");
	s->outline = fx_outline_load();
}

static void roll_update(void *data, obs_data_t *settings)
{
	struct roll_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "roll_font");
	s->dir = (int)obs_data_get_int(settings, "roll_dir");
	s->speed = (float)obs_data_get_double(settings, "roll_speed");
	s->bounce = (float)obs_data_get_double(settings, "roll_bounce");
	s->loop = obs_data_get_bool(settings, "roll_loop");
	s->exit = obs_data_get_bool(settings, "roll_exit");
	s->fade_in = obs_data_get_bool(settings, "roll_fade_in");
	s->fade_out = obs_data_get_bool(settings, "roll_fade_out");
	s->hold = (float)obs_data_get_double(settings, "roll_hold");
	s->outline_on = obs_data_get_bool(settings, "roll_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "roll_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "roll_outline_color");
}

/* Decaying bounce as a glyph rolls in/out across 0..1 progress (<=0, up). */
static float roll_bounce_off(float prog, float bounce, float text_h)
{
	float amp = bounce * text_h * 0.7f;
	float env = (1.0f - prog);        /* settles toward the ground   */
	float b = fabsf(sinf(prog * ROLL_PI * 3.0f)); /* three hops      */
	return -amp * env * b;
}

static void roll_render(void *data, const struct fx_render_ctx *ctx)
{
	struct roll_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	int n = (int)mask->glyph_count;
	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.9f / sp;
	float exit_dur = 0.8f / sp;
	float stag = 0.13f / sp;

	float all_in = (float)(n - 1) * stag + enter_dur;
	float hold_end = all_in + s->hold;
	float exit_all = (float)(n - 1) * stag + exit_dur;

	/* A cycle always carries an exit phase when looping; one-shot keeps an
	 * exit only when the letters should roll/fade away rather than stay. */
	bool do_exit = s->loop || s->exit;
	float cycle = do_exit ? hold_end + exit_all + 0.3f : hold_end;
	float lt = s->loop ? fmodf(ctx->time, cycle)
			   : fminf(ctx->time, do_exit ? cycle : all_in);

	float off = (float)ctx->width;
	float enter_from = (s->dir == 1) ? off : -off;
	float exit_to = -enter_from;
	float text_h = mask->text_bottom - mask->text_top;
	if (text_h < 1.0f)
		text_h = 1.0f;
	float radius = text_h * 0.5f;
	if (radius < 1.0f)
		radius = 1.0f;

	/* Roll a whole number of turns over the entrance so each letter settles
	 * exactly upright (angle 0) at its home position. */
	float circ = ROLL_TWO_PI * radius;
	int turns = (int)(off / circ + 0.5f);
	if (turns < 2)
		turns = 2;
	float full = (float)turns * ROLL_TWO_PI;
	float rollsign = (s->dir == 0) ? 1.0f : -1.0f;

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

		float dx = 0.0f, dy = 0.0f, a = 1.0f, angle = 0.0f;
		if (lt < es) {
			dx = enter_from;
			a = s->fade_in ? 0.0f : 1.0f;
			angle = -rollsign * full;
		} else if (lt < ee) {
			float p = (lt - es) / enter_dur;
			float e = 1.0f - (1.0f - p) * (1.0f - p); /* ease out */
			dx = enter_from * (1.0f - e);
			dy = roll_bounce_off(p, s->bounce, text_h);
			a = s->fade_in ? p : 1.0f;
			/* Spins down to upright (0) as it reaches home. */
			angle = -rollsign * full * (1.0f - e);
		} else if (lt < xs || !do_exit) {
			dx = 0.0f;
			angle = 0.0f; /* at rest: upright */
		} else if (lt < xe) {
			float p = (lt - xs) / exit_dur;
			if (s->exit) {
				float e = p * p; /* ease in */
				dx = exit_to * e;
				dy = roll_bounce_off(1.0f - p, s->bounce, text_h);
				angle = rollsign * full * e; /* rolls away */
			}
			a = s->fade_out ? 1.0f - p : (s->exit ? 1.0f : 0.0f);
		} else {
			dx = s->exit ? exit_to : 0.0f;
			a = (s->fade_out || !s->exit) ? 0.0f : 1.0f;
			angle = s->exit ? rollsign * full : 0.0f;
		}
		if (a <= 0.0f)
			continue;

		float fc[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * a};
		float oc[4] = {orgba[0], orgba[1], orgba[2], orgba[3] * a};
		float pivx = g->w * 0.5f;
		float pivy = g->h * 0.5f;

		gs_matrix_push();
		gs_matrix_translate3f(g->x + dx, g->y + dy, 0.0f);
		gs_matrix_translate3f(pivx, pivy, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, angle);
		gs_matrix_translate3f(-pivx, -pivy, 0.0f);
		if (draw_outline)
			fx_outline_render_sub(s->outline, mask->tex, ctx->width,
					      ctx->height, oc, s->outline_width,
					      (uint32_t)g->x, (uint32_t)g->y,
					      (uint32_t)g->w, (uint32_t)g->h);
		fx_textfill_render_sub(s->fill, mask->tex, fc, (uint32_t)g->x,
				       (uint32_t)g->y, (uint32_t)g->w,
				       (uint32_t)g->h);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
}

static void roll_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "roll_font",
		obs_module_text("RollFontColor"));
	obs_property_t *d = obs_properties_add_list(p, "roll_dir",
		obs_module_text("RollDir"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("RollFromLeft"), 0);
	obs_property_list_add_int(d, obs_module_text("RollFromRight"), 1);
	obs_properties_add_float_slider(p, "roll_speed",
		obs_module_text("RollSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "roll_bounce",
		obs_module_text("RollBounce"), 0.0, 4.0, 0.05);
	obs_properties_add_float_slider(p, "roll_hold",
		obs_module_text("RollHold"), 0.0, 6.0, 0.1);
	obs_properties_add_bool(p, "roll_loop", obs_module_text("LoopOn"));
	obs_properties_add_bool(p, "roll_exit", obs_module_text("RollExit"));
	obs_properties_add_bool(p, "roll_fade_in", obs_module_text("HopFadeIn"));
	obs_properties_add_bool(p, "roll_fade_out",
		obs_module_text("HopFadeOut"));
	obs_properties_add_bool(p, "roll_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "roll_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "roll_outline_color",
		obs_module_text("OutlineColor"));
}

static void roll_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "roll_font", DEFAULT_ROLL_FONT);
	obs_data_set_default_int(settings, "roll_dir", 0);
	obs_data_set_default_double(settings, "roll_speed", 1.0);
	obs_data_set_default_double(settings, "roll_bounce", 0.5);
	obs_data_set_default_double(settings, "roll_hold", 1.5);
	obs_data_set_default_bool(settings, "roll_loop", true);
	obs_data_set_default_bool(settings, "roll_exit", true);
	obs_data_set_default_bool(settings, "roll_fade_in", true);
	obs_data_set_default_bool(settings, "roll_fade_out", true);
	obs_data_set_default_bool(settings, "roll_outline", false);
	obs_data_set_default_double(settings, "roll_outline_width", 4.0);
	obs_data_set_default_int(settings, "roll_outline_color",
				 DEFAULT_OUTLINE_COLOR);
}

const struct text_effect fx_roll = {
	.id             = "roll",
	.name_key       = "EffectRoll",
	.create         = roll_create,
	.destroy        = roll_destroy,
	.load_graphics  = roll_load_graphics,
	.update         = roll_update,
	.render         = roll_render,
	.get_properties = roll_properties,
	.get_defaults   = roll_defaults,
};
