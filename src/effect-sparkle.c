#include "effect-sparkle.h"
#include "flametext-sprites.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_SPK_FONT  0xFFFFFFFFu /* white #FFFFFF        */
#define DEFAULT_SPK_COLOR 0xFFE9FFFFu /* warm white #FFFFE9   */
#define SPK_CAPACITY 2048
#define SPK_PI 3.14159265f

struct sparkle_state {
	gs_effect_t *sprite;
	gs_effect_t *fill;
	struct fx_sprite_system *sys;

	uint32_t font_color;
	uint32_t color;
	float    density; /* 0..1 spawn rate */
	float    size;    /* size multiplier */
	float    speed;   /* twinkle speed   */
	float    bloom;

	float    text_h; /* cached from mask for sizing */
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *sparkle_create(void)
{
	struct sparkle_state *s = bzalloc(sizeof(*s));
	s->sys = fx_sprites_create(SPK_CAPACITY);
	return s;
}

static void sparkle_destroy(void *data)
{
	struct sparkle_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	if (s->fill)
		gs_effect_destroy(s->fill);
	fx_sprites_destroy(s->sys);
	bfree(s);
}

static void sparkle_load_graphics(void *data)
{
	struct sparkle_state *s = data;
	char *path = obs_module_file("effects/sprite.effect");
	if (path) {
		s->sprite = gs_effect_create_from_file(path, NULL);
		if (!s->sprite)
			obs_log(LOG_ERROR, "failed to load sprite.effect (%s)",
				path);
	}
	bfree(path);
	s->fill = fx_textfill_load();
}

static void sparkle_update(void *data, obs_data_t *settings)
{
	struct sparkle_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "spk_font");
	s->color = (uint32_t)obs_data_get_int(settings, "spk_color");
	s->density = (float)obs_data_get_double(settings, "spk_density");
	s->size = (float)obs_data_get_double(settings, "spk_size");
	s->speed = (float)obs_data_get_double(settings, "spk_speed");
	s->bloom = (float)obs_data_get_double(settings, "spk_bloom");
}

static void sparkle_reset(void *data)
{
	struct sparkle_state *s = data;
	fx_sprites_reset(s->sys);
}

static void spawn_one(struct sparkle_state *s, const struct flametext_mask *m)
{
	fx_sprite_t *q = fx_sprites_spawn(s->sys);
	if (!q)
		return;

	/* Prefer a point on an actual glyph so glints land on the letters. */
	if (m->glyph_count > 0) {
		uint32_t gi = (uint32_t)(fx_sprites_frand(s->sys) *
					 (float)m->glyph_count);
		if (gi >= m->glyph_count)
			gi = (uint32_t)m->glyph_count - 1;
		const struct flametext_glyph *g = &m->glyphs[gi];
		q->x = g->x + fx_sprites_frand(s->sys) * g->w;
		q->y = g->y + fx_sprites_frand(s->sys) * g->h;
	} else {
		q->x = m->text_left +
		       fx_sprites_frand(s->sys) * (m->text_right - m->text_left);
		q->y = m->text_top +
		       fx_sprites_frand(s->sys) * (m->text_bottom - m->text_top);
	}

	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	q->max_life = (0.4f + fx_sprites_frand(s->sys) * 0.7f) / sp;
	q->life = q->max_life;

	float base = s->text_h * 0.05f * s->size *
		     (0.6f + 0.8f * fx_sprites_frand(s->sys));
	if (base < 1.0f)
		base = 1.0f;
	q->size = base;
	q->seed = base; /* remembered so we can pulse the scale each tick */

	q->rot = fx_sprites_frand(s->sys) * 6.2831853f;
	q->vrot = (fx_sprites_frand(s->sys) - 0.5f) * 3.0f;

	float rgba[4];
	unpack_color(s->color, rgba);
	q->r = rgba[0];
	q->g = rgba[1];
	q->b = rgba[2];
	q->a = 0.0f;
}

static void sparkle_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	struct sparkle_state *s = data;
	const struct flametext_mask *m = ctx->mask;
	if (!m)
		return;
	if (dt > 0.1f)
		dt = 0.1f;

	s->text_h = m->text_bottom - m->text_top;
	if (s->text_h < 1.0f)
		s->text_h = 1.0f;

	float colA = (float)((s->color >> 24) & 0xFF) / 255.0f;

	/* Spawn: density 0..1 maps to up to ~50 sparkles/sec. */
	s->sys->emit_accum += s->density * 50.0f * dt;
	while (s->sys->emit_accum >= 1.0f) {
		s->sys->emit_accum -= 1.0f;
		spawn_one(s, m);
	}

	for (size_t i = 0; i < s->sys->live;) {
		fx_sprite_t *q = &s->sys->items[i];
		q->life -= dt;
		if (q->life <= 0.0f) {
			s->sys->items[i] = s->sys->items[--s->sys->live];
			continue;
		}
		float t01 = 1.0f - q->life / q->max_life;
		float env = sinf(SPK_PI * t01); /* 0 -> 1 -> 0 */
		q->size = q->seed * (0.4f + 0.6f * env);
		q->a = env * colA;
		q->rot += q->vrot * dt;
		++i;
	}
}

static void sparkle_render(void *data, const struct fx_render_ctx *ctx)
{
	struct sparkle_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex)
		return;

	if (s->fill) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		fx_textfill_render(s->fill, mask->tex, rgba);
	}

	if (s->sprite) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		fx_sprites_render(s->sys, s->sprite, FX_SHAPE_STAR, 1.0f,
				  s->bloom, NULL);
		gs_blend_state_pop();
	}
}

static void sparkle_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "spk_font",
		obs_module_text("SpkFontColor"));
	obs_properties_add_color_alpha(p, "spk_color",
		obs_module_text("SpkColor"));
	obs_properties_add_float_slider(p, "spk_density",
		obs_module_text("SpkDensity"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "spk_size",
		obs_module_text("SpkSize"), 0.2, 4.0, 0.05);
	obs_properties_add_float_slider(p, "spk_speed",
		obs_module_text("SpkSpeed"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(p, "spk_bloom",
		obs_module_text("SpkBloom"), 0.0, 3.0, 0.05);
}

static void sparkle_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "spk_font", DEFAULT_SPK_FONT);
	obs_data_set_default_int(settings, "spk_color", DEFAULT_SPK_COLOR);
	obs_data_set_default_double(settings, "spk_density", 0.5);
	obs_data_set_default_double(settings, "spk_size", 1.0);
	obs_data_set_default_double(settings, "spk_speed", 1.0);
	obs_data_set_default_double(settings, "spk_bloom", 1.0);
}

const struct text_effect fx_sparkle = {
	.id             = "sparkle",
	.name_key       = "EffectSparkle",
	.create         = sparkle_create,
	.destroy        = sparkle_destroy,
	.load_graphics  = sparkle_load_graphics,
	.update         = sparkle_update,
	.tick           = sparkle_tick,
	.render         = sparkle_render,
	.reset          = sparkle_reset,
	.get_properties = sparkle_properties,
	.get_defaults   = sparkle_defaults,
};
