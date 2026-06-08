#include "flametext-sprites.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

/* Quad is drawn larger than the sprite's core so the bloom halo has room. */
#define HALO_SCALE 1.9f

struct fx_sprite_system *fx_sprites_create(size_t capacity)
{
	struct fx_sprite_system *s = bzalloc(sizeof(*s));
	s->capacity = capacity;
	s->items = bzalloc(sizeof(fx_sprite_t) * capacity);
	s->rng = 0x1234567u;
	return s;
}

void fx_sprites_destroy(struct fx_sprite_system *s)
{
	if (!s)
		return;
	bfree(s->items);
	bfree(s);
}

void fx_sprites_reset(struct fx_sprite_system *s)
{
	if (!s)
		return;
	s->live = 0;
	s->emit_accum = 0.0f;
}

static uint32_t xs32(uint32_t *st)
{
	uint32_t x = *st ? *st : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*st = x;
	return x;
}

float fx_sprites_frand(struct fx_sprite_system *s)
{
	return (float)(xs32(&s->rng) & 0xFFFFFF) / (float)0x1000000;
}

fx_sprite_t *fx_sprites_spawn(struct fx_sprite_system *s)
{
	if (!s || s->live >= s->capacity)
		return NULL;
	fx_sprite_t *p = &s->items[s->live++];
	memset(p, 0, sizeof(*p));
	return p;
}

static uint32_t pack_rgba(float r, float g, float b, float a)
{
	uint32_t R = (uint32_t)(r < 0 ? 0 : (r > 1 ? 255 : r * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(g < 0 ? 0 : (g > 1 ? 255 : g * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(b < 0 ? 0 : (b > 1 ? 255 : b * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(a < 0 ? 0 : (a > 1 ? 255 : a * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* OBS 0xAABBGGRR */
}

void fx_sprites_render(struct fx_sprite_system *s, gs_effect_t *e, int shape,
		       float glow, float bloom, gs_texture_t *img)
{
	if (!s || !e || s->live == 0)
		return;

	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "shape")))
		gs_effect_set_float(p, (float)shape);
	if ((p = gs_effect_get_param_by_name(e, "glow")))
		gs_effect_set_float(p, glow);
	if ((p = gs_effect_get_param_by_name(e, "bloom")))
		gs_effect_set_float(p, bloom);
	if ((p = gs_effect_get_param_by_name(e, "use_image")))
		gs_effect_set_float(p, (shape == FX_SHAPE_IMAGE && img) ? 1.0f
								       : 0.0f);
	if ((p = gs_effect_get_param_by_name(e, "image")) && img)
		gs_effect_set_texture(p, img);

	while (gs_effect_loop(e, "Draw")) {
		gs_render_start(true);
		for (size_t i = 0; i < s->live; ++i) {
			fx_sprite_t *q = &s->items[i];
			float h = q->size * HALO_SCALE;
			float c = cosf(q->rot);
			float sn = sinf(q->rot);
			float fade = q->max_life > 0.0f
					     ? q->life / q->max_life
					     : 1.0f;
			uint32_t col = pack_rgba(q->r, q->g, q->b, q->a);

			/* Rotated quad corners (local -h..h). */
			float cx[4] = {-h, h, h, -h};
			float cy[4] = {-h, -h, h, h};
			float ux[4] = {0.0f, 1.0f, 1.0f, 0.0f};
			float uy[4] = {0.0f, 0.0f, 1.0f, 1.0f};
			float px[4], py[4];
			for (int k = 0; k < 4; ++k) {
				px[k] = q->x + cx[k] * c - cy[k] * sn;
				py[k] = q->y + cx[k] * sn + cy[k] * c;
			}
			int idx[6] = {0, 1, 2, 0, 2, 3};
			for (int t = 0; t < 6; ++t) {
				int k = idx[t];
				gs_texcoord(ux[k], uy[k], 0);
				gs_texcoord(fade, q->seed, 1);
				gs_color(col);
				gs_vertex2f(px[k], py[k]);
			}
		}
		gs_render_stop(GS_TRIS);
	}
}

gs_effect_t *fx_textfill_load(void)
{
	char *path = obs_module_file("effects/textfill.effect");
	gs_effect_t *e = NULL;
	if (path)
		e = gs_effect_create_from_file(path, NULL);
	bfree(path);
	return e;
}

void fx_textfill_render(gs_effect_t *e, gs_texture_t *tex, const float rgba[4])
{
	if (!e || !tex)
		return;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, tex);
	if ((p = gs_effect_get_param_by_name(e, "font_color"))) {
		struct vec4 c;
		vec4_set(&c, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &c);
	}
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_blend_state_pop();
}

void fx_textfill_render_sub(gs_effect_t *e, gs_texture_t *tex,
			    const float rgba[4], uint32_t x, uint32_t y,
			    uint32_t cx, uint32_t cy)
{
	if (!e || !tex)
		return;
	gs_eparam_t *p;
	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, tex);
	if ((p = gs_effect_get_param_by_name(e, "font_color"))) {
		struct vec4 c;
		vec4_set(&c, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p, &c);
	}
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite_subregion(tex, 0, x, y, cx, cy);
}
