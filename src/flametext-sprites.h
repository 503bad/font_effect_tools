#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;
struct gs_texture;
typedef struct gs_texture gs_texture_t;

/* Built-in sprite shapes drawn procedurally by sprite.effect, plus an
 * arbitrary image. */
enum fx_sprite_shape {
	FX_SHAPE_SOFT = 0,   /* soft round glow blob   */
	FX_SHAPE_CIRCLE = 1, /* hard-ish disc           */
	FX_SHAPE_STAR = 2,   /* 5-point star            */
	FX_SHAPE_HEART = 3,  /* heart                   */
	FX_SHAPE_PETAL = 4,  /* leaf/petal              */
	FX_SHAPE_IMAGE = 5,  /* user-supplied texture   */
};

/* One generic billboard particle. Position/velocity are in canvas pixels
 * (y grows downward); life counts down in seconds. Colour is linear 0..1 and
 * `a` is the current (already-faded) alpha. */
typedef struct {
	float x, y;
	float vx, vy;
	float life, max_life;
	float size; /* half-extent in px (visual core)  */
	float rot, vrot;
	float seed; /* 0..1 per-particle variation       */
	float r, g, b, a;
} fx_sprite_t;

/* A fixed-capacity pool. Live particles occupy [0, live); effects integrate
 * and swap-remove them like the spark system. */
struct fx_sprite_system {
	fx_sprite_t *items;
	size_t capacity;
	size_t live;
	uint32_t rng;
	float emit_accum; /* fractional spawn carry */
};

struct fx_sprite_system *fx_sprites_create(size_t capacity);
void fx_sprites_destroy(struct fx_sprite_system *s);
void fx_sprites_reset(struct fx_sprite_system *s);

/* Grab a free slot (returns NULL when full). The caller fills it in. */
fx_sprite_t *fx_sprites_spawn(struct fx_sprite_system *s);

/* Uniform random in [0,1) from the pool's xorshift state. */
float fx_sprites_frand(struct fx_sprite_system *s);

/* Draw every live sprite as a rotated billboard. The caller selects the blend
 * state; the shader outputs premultiplied alpha (use GS_BLEND_ONE,
 * GS_BLEND_INVSRCALPHA, or GS_BLEND_ONE/GS_BLEND_ONE for pure additive).
 * `glow` brightens the core, `bloom` adds an outer halo; `img` is required only
 * for FX_SHAPE_IMAGE. Must run under the OBS graphics lock. */
void fx_sprites_render(struct fx_sprite_system *s, gs_effect_t *e, int shape,
		       float glow, float bloom, gs_texture_t *img);

/* Shared "draw the text in a flat colour" helper used by effects that overlay
 * particles/decorations on the base text. Load the shader once in
 * load_graphics; render it under the graphics lock. `rgba` is linear 0..1. */
gs_effect_t *fx_textfill_load(void);
void fx_textfill_render(gs_effect_t *e, gs_texture_t *tex, const float rgba[4]);

/* Draw a single glyph sub-region of the mask texture in a flat colour. The
 * caller sets the blend state and the matrix stack (for per-glyph position,
 * scale and rotation). x/y/cx/cy are the glyph rect in texels. */
void fx_textfill_render_sub(gs_effect_t *e, gs_texture_t *tex,
			    const float rgba[4], uint32_t x, uint32_t y,
			    uint32_t cx, uint32_t cy);

#ifdef __cplusplus
}
#endif
