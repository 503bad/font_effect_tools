#include "effect-cube.h"
#include "flametext-sprites.h" /* fx_textfill_* */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_CUBE_FONT 0xFFFFFFFFu /* white #FFFFFF       */
#define DEFAULT_CUBE_SIDE 0xFF707070u /* shaded face #707070  */
#define CUBE_PI 3.14159265f

struct cube_state {
	gs_effect_t *fill;

	uint32_t font_color;
	uint32_t side_color;
	float    speed;
	bool     reverse; /* ping-pong (unfold then reverse) */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *cube_create(void)
{
	return bzalloc(sizeof(struct cube_state));
}

static void cube_destroy(void *data)
{
	struct cube_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	bfree(s);
}

static void cube_load_graphics(void *data)
{
	struct cube_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "cube: failed to load textfill.effect");
}

static void cube_update(void *data, obs_data_t *settings)
{
	struct cube_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "cube_font");
	s->side_color = (uint32_t)obs_data_get_int(settings, "cube_side");
	s->speed = (float)obs_data_get_double(settings, "cube_speed");
	s->reverse = obs_data_get_bool(settings, "cube_reverse");
}

static void cube_render(void *data, const struct fx_render_ctx *ctx)
{
	struct cube_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill)
		return;

	float t = ctx->time;
	float a;
	if (s->reverse) {
		float phase = t * s->speed * 0.25f;
		phase -= floorf(phase);
		float tri = 1.0f - fabsf(2.0f * phase - 1.0f); /* 0..1..0 */
		a = tri * (2.0f * CUBE_PI);
	} else {
		a = t * s->speed * (CUBE_PI * 0.5f);
	}

	float cx = (float)ctx->width * 0.5f;
	float cy = (mask->text_top + mask->text_bottom) * 0.5f;
	float L = mask->text_bottom - mask->text_top;

	float frgba[4], srgba[4];
	unpack_color(s->font_color, frgba);
	unpack_color(s->side_color, srgba);

	/* Compute facing for the four faces and draw back-to-front. */
	float cf[4];
	int order[4] = {0, 1, 2, 3};
	for (int k = 0; k < 4; ++k)
		cf[k] = cosf(a + (float)k * (CUBE_PI * 0.5f));
	for (int i = 0; i < 4; ++i)
		for (int j = i + 1; j < 4; ++j)
			if (cf[order[j]] < cf[order[i]]) {
				int tmp = order[i];
				order[i] = order[j];
				order[j] = tmp;
			}

	for (int o = 0; o < 4; ++o) {
		int k = order[o];
		float facing = cf[k];
		if (facing <= 0.03f)
			continue;

		float shade = 0.35f + 0.65f * facing;
		float rgba[4];
		for (int c = 0; c < 3; ++c)
			rgba[c] = (srgba[c] + (frgba[c] - srgba[c]) * facing) *
				  shade;
		rgba[3] = frgba[3];

		gs_matrix_push();
		gs_matrix_translate3f(cx, cy, 0.0f);
		gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f,
				  a + (float)k * (CUBE_PI * 0.5f));
		gs_matrix_translate3f(0.0f, 0.0f, L * 0.5f);
		gs_matrix_translate3f(-cx, -cy, 0.0f);
		fx_textfill_render(s->fill, mask->tex, rgba);
		gs_matrix_pop();
	}
}

static void cube_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "cube_font",
		obs_module_text("CubeFontColor"));
	obs_properties_add_color_alpha(p, "cube_side",
		obs_module_text("CubeSideColor"));
	obs_properties_add_float_slider(p, "cube_speed",
		obs_module_text("CubeSpeed"), 0.1, 3.0, 0.05);
	obs_properties_add_bool(p, "cube_reverse",
		obs_module_text("CubeReverse"));
}

static void cube_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "cube_font", DEFAULT_CUBE_FONT);
	obs_data_set_default_int(settings, "cube_side", DEFAULT_CUBE_SIDE);
	obs_data_set_default_double(settings, "cube_speed", 0.6);
	obs_data_set_default_bool(settings, "cube_reverse", false);
}

const struct text_effect fx_cube = {
	.id             = "cube",
	.name_key       = "EffectCube",
	.create         = cube_create,
	.destroy        = cube_destroy,
	.load_graphics  = cube_load_graphics,
	.update         = cube_update,
	.render         = cube_render,
	.get_properties = cube_properties,
	.get_defaults   = cube_defaults,
};
