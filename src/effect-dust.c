#include "effect-dust.h"
#include "flametext-sprites.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_DUST_FONT  0xFFFFFFFFu /* white #FFFFFF       */
#define DEFAULT_DUST_COLOR 0xFFB9D6E8u /* sandy #E8D6B9        */
#define DUST_CAPACITY 2048

struct dust_state {
	gs_effect_t *sprite;
	gs_effect_t *fill;
	struct fx_sprite_system *sys;

	uint32_t font_color;
	uint32_t color;
	float    amount; /* 0..1 spawn rate    */
	float    rise;   /* upward speed mult  */
	float    drift;  /* sideways flow      */
	float    life;   /* particle lifetime  */
	float    bloom;

	float    text_h;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *dust_create(void)
{
	struct dust_state *s = bzalloc(sizeof(*s));
	s->sys = fx_sprites_create(DUST_CAPACITY);
	return s;
}

static void dust_destroy(void *data)
{
	struct dust_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	if (s->fill)
		gs_effect_destroy(s->fill);
	fx_sprites_destroy(s->sys);
	bfree(s);
}

static void dust_load_graphics(void *data)
{
	struct dust_state *s = data;
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

static void dust_update(void *data, obs_data_t *settings)
{
	struct dust_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "dust_font");
	s->color = (uint32_t)obs_data_get_int(settings, "dust_color");
	s->amount = (float)obs_data_get_double(settings, "dust_amount");
	s->rise = (float)obs_data_get_double(settings, "dust_rise");
	s->drift = (float)obs_data_get_double(settings, "dust_drift");
	s->life = (float)obs_data_get_double(settings, "dust_life");
	s->bloom = (float)obs_data_get_double(settings, "dust_bloom");
}

static void dust_reset(void *data)
{
	struct dust_state *s = data;
	fx_sprites_reset(s->sys);
}

static void spawn_one(struct dust_state *s, const struct flametext_mask *m)
{
	fx_sprite_t *q = fx_sprites_spawn(s->sys);
	if (!q)
		return;

	float r = fx_sprites_frand(s->sys);
	q->x = m->text_left + r * (m->text_right - m->text_left);
	q->y = m->text_bottom - fx_sprites_frand(s->sys) * s->text_h * 0.1f;

	float spd = s->text_h * (0.6f + 0.6f * fx_sprites_frand(s->sys)) *
		    s->rise;
	q->vy = -spd; /* upward */
	q->vx = (fx_sprites_frand(s->sys) - 0.5f) * s->text_h * s->drift;

	q->max_life = s->life * (0.6f + 0.6f * fx_sprites_frand(s->sys));
	q->life = q->max_life;

	q->size = s->text_h * (0.06f + 0.05f * fx_sprites_frand(s->sys));
	q->seed = fx_sprites_frand(s->sys);
	q->rot = fx_sprites_frand(s->sys) * 6.2831853f;
	q->vrot = (fx_sprites_frand(s->sys) - 0.5f) * 1.0f;

	float rgba[4];
	unpack_color(s->color, rgba);
	q->r = rgba[0];
	q->g = rgba[1];
	q->b = rgba[2];
	q->a = 0.0f;
}

static void dust_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	struct dust_state *s = data;
	const struct flametext_mask *m = ctx->mask;
	if (!m)
		return;
	if (dt > 0.1f)
		dt = 0.1f;

	s->text_h = m->text_bottom - m->text_top;
	if (s->text_h < 1.0f)
		s->text_h = 1.0f;

	float colA = (float)((s->color >> 24) & 0xFF) / 255.0f;

	s->sys->emit_accum += s->amount * 120.0f * dt;
	while (s->sys->emit_accum >= 1.0f) {
		s->sys->emit_accum -= 1.0f;
		spawn_one(s, m);
	}

	float side = s->text_h * s->drift * 1.2f;
	for (size_t i = 0; i < s->sys->live;) {
		fx_sprite_t *q = &s->sys->items[i];
		q->life -= dt;
		if (q->life <= 0.0f) {
			s->sys->items[i] = s->sys->items[--s->sys->live];
			continue;
		}
		/* Rising air slows; sideways flow accelerates as it lifts. */
		q->vy *= (1.0f - 0.6f * dt);
		q->vx += (q->seed - 0.5f) * side * dt;
		q->x += q->vx * dt;
		q->y += q->vy * dt;
		q->size += s->text_h * 0.12f * dt; /* puff expands */
		q->rot += q->vrot * dt;

		float t01 = 1.0f - q->life / q->max_life;
		float env = sinf(3.14159265f * t01); /* fade in & out */
		q->a = env * 0.5f * colA;
		++i;
	}
}

static void dust_render(void *data, const struct fx_render_ctx *ctx)
{
	struct dust_state *s = data;
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
		fx_sprites_render(s->sys, s->sprite, FX_SHAPE_SOFT, 0.0f,
				  s->bloom, NULL);
		gs_blend_state_pop();
	}
}

static void dust_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "dust_font",
		obs_module_text("DustFontColor"));
	obs_properties_add_color_alpha(p, "dust_color",
		obs_module_text("DustColor"));
	obs_properties_add_float_slider(p, "dust_amount",
		obs_module_text("DustAmount"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "dust_rise",
		obs_module_text("DustRise"), 0.1, 3.0, 0.05);
	obs_properties_add_float_slider(p, "dust_drift",
		obs_module_text("DustDrift"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(p, "dust_life",
		obs_module_text("DustLife"), 0.3, 4.0, 0.05);
	obs_properties_add_float_slider(p, "dust_bloom",
		obs_module_text("DustBloom"), 0.0, 3.0, 0.05);
}

static void dust_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "dust_font", DEFAULT_DUST_FONT);
	obs_data_set_default_int(settings, "dust_color", DEFAULT_DUST_COLOR);
	obs_data_set_default_double(settings, "dust_amount", 0.5);
	obs_data_set_default_double(settings, "dust_rise", 1.0);
	obs_data_set_default_double(settings, "dust_drift", 0.6);
	obs_data_set_default_double(settings, "dust_life", 1.6);
	obs_data_set_default_double(settings, "dust_bloom", 0.4);
}

const struct text_effect fx_dust = {
	.id             = "dust",
	.name_key       = "EffectDust",
	.create         = dust_create,
	.destroy        = dust_destroy,
	.load_graphics  = dust_load_graphics,
	.update         = dust_update,
	.tick           = dust_tick,
	.render         = dust_render,
	.reset          = dust_reset,
	.get_properties = dust_properties,
	.get_defaults   = dust_defaults,
};
