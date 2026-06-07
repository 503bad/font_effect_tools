#include "flametext-particles.h"

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs.h>
#include <util/bmem.h>

#include <math.h>

/* Deterministic, fast PRNG so behaviour is reproducible across runs (the
 * graphics clock, not wall time, drives variation). */
static inline uint32_t xs32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x ? x : 0x9e3779b9u;
	return x;
}

static inline float frand(uint32_t *s)
{
	return (float)(xs32(s) & 0xFFFFFF) / (float)0x1000000; /* [0,1) */
}

struct spark_system *flametext_particles_create(size_t capacity)
{
	if (capacity == 0)
		capacity = 1024;
	struct spark_system *sys = bzalloc(sizeof(*sys));
	sys->sparks = bzalloc(sizeof(spark_t) * capacity);
	sys->capacity = capacity;
	sys->live = 0;
	sys->rng = 0x1234abcdu;

	/* Sensible defaults; overwritten by properties. */
	sys->emit_rate = 150.0f;
	sys->init_speed = 140.0f;
	sys->lifetime = 2.25f;
	sys->base_size = 2.0f;
	sys->spread = 1.0f;
	sys->bloom = 1.0f;
	sys->emit_dir = -1.0f;
	sys->hot[0] = 1.0f;  sys->hot[1] = 0.95f; sys->hot[2] = 0.6f;  sys->hot[3] = 1.0f;
	sys->cool[0] = 1.0f; sys->cool[1] = 0.30f; sys->cool[2] = 0.05f; sys->cool[3] = 1.0f;
	return sys;
}

void flametext_particles_destroy(struct spark_system *sys)
{
	if (!sys)
		return;
	bfree(sys->sparks);
	bfree(sys);
}

void flametext_particles_set_emitter(struct spark_system *sys,
				     float left, float right, float edge_y,
				     float dir)
{
	if (!sys)
		return;
	sys->emit_left = left;
	sys->emit_right = right;
	sys->emit_y = edge_y;
	sys->emit_dir = dir < 0.0f ? -1.0f : 1.0f;
}

void flametext_particles_reset(struct spark_system *sys)
{
	if (sys) {
		sys->live = 0;
		sys->emit_accum = 0.0f;
		sys->phase = 0.0f;
	}
}

static void spawn_one(struct spark_system *sys)
{
	if (sys->live >= sys->capacity)
		return;
	if (sys->emit_right <= sys->emit_left)
		return;

	spark_t *p = &sys->sparks[sys->live++];
	float r1 = frand(&sys->rng);
	float r2 = frand(&sys->rng);
	float r3 = frand(&sys->rng);
	float r4 = frand(&sys->rng);

	p->x = sys->emit_left + (sys->emit_right - sys->emit_left) * r1;
	/* Nudge spawns just off the chosen edge (above the top, or below the
	 * bottom) so embers appear to lift off it. */
	p->y = sys->emit_y + sys->emit_dir * sys->base_size * 0.5f * r2;

	float spd = sys->init_speed * (1.0f - sys->spread * 0.5f * r3);
	p->vy = -spd;                                  /* upward (y is down) */
	p->vx = (r4 - 0.5f) * sys->init_speed * sys->spread * 0.8f;

	p->max_life = sys->lifetime * (0.6f + 0.8f * frand(&sys->rng));
	p->life = p->max_life;
	p->size = sys->base_size * (0.6f + 0.9f * frand(&sys->rng));
	p->seed = frand(&sys->rng);
}

void flametext_particles_tick(struct spark_system *sys, float dt)
{
	if (!sys || dt <= 0.0f)
		return;
	if (dt > 0.1f)
		dt = 0.1f; /* clamp huge hitches so the sim stays stable */

	sys->phase += dt;

	/* --- emission --- */
	sys->emit_accum += sys->emit_rate * dt;
	while (sys->emit_accum >= 1.0f) {
		sys->emit_accum -= 1.0f;
		spawn_one(sys);
	}

	/* --- integrate + retire (swap-remove dead) --- */
	const float gravity = sys->init_speed * 0.15f; /* gentle cooling pull */
	for (size_t i = 0; i < sys->live;) {
		spark_t *p = &sys->sparks[i];
		p->life -= dt;
		if (p->life <= 0.0f) {
			sys->sparks[i] = sys->sparks[--sys->live];
			continue;
		}
		/* Horizontal sway: per-particle sinusoid for a flickering drift. */
		float sway = sinf(sys->phase * 6.0f + p->seed * 6.2831853f);
		p->vx += sway * sys->init_speed * 0.6f * dt;
		p->vy += gravity * dt;          /* buoyancy fades → starts to fall */

		p->x += p->vx * dt;
		p->y += p->vy * dt;
		++i;
	}
}

void flametext_particles_render(struct spark_system *sys, gs_effect_t *effect)
{
	if (!sys || !effect || sys->live == 0)
		return;

	gs_eparam_t *p_hot = gs_effect_get_param_by_name(effect, "ember_hot");
	gs_eparam_t *p_cool = gs_effect_get_param_by_name(effect, "ember_cool");
	gs_eparam_t *p_bloom = gs_effect_get_param_by_name(effect, "bloom");
	struct vec4 hot, cool;
	vec4_set(&hot, sys->hot[0], sys->hot[1], sys->hot[2], sys->hot[3]);
	vec4_set(&cool, sys->cool[0], sys->cool[1], sys->cool[2], sys->cool[3]);
	if (p_hot)
		gs_effect_set_vec4(p_hot, &hot);
	if (p_cool)
		gs_effect_set_vec4(p_cool, &cool);
	if (p_bloom)
		gs_effect_set_float(p_bloom, sys->bloom);

	/* Quads are drawn larger than the ember core so the additive bloom
	 * halo has room to spread; the shape inside is shaped by the shader. */
	const float glow_scale = 2.2f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_ONE); /* additive glow */

	while (gs_effect_loop(effect, "Draw")) {
		gs_render_start(true);
		for (size_t i = 0; i < sys->live; ++i) {
			const spark_t *p = &sys->sparks[i];
			float fade = p->max_life > 0.0f ? p->life / p->max_life
						        : 0.0f;
			/* heat = how hot the ember looks (hottest when young),
			 * bright = brightness fading toward death. */
			float heat = fade;            /* 1 young .. 0 old        */
			float bright = fade * fade;   /* fade out faster visually */
			float s = p->size * glow_scale;
			float x0 = p->x - s, x1 = p->x + s;
			float y0 = p->y - s, y1 = p->y + s;

			/* Two triangles (TL,TR,BL) (TR,BR,BL). TEXCOORD0 = corner
			 * uv, TEXCOORD1 = (heat, bright) carried per vertex. */
			#define SV(cx, cy, u, v)                       \
				gs_texcoord(u, v, 0);                  \
				gs_texcoord(heat, bright, 1);          \
				gs_vertex2f(cx, cy)

			SV(x0, y0, 0.0f, 0.0f);
			SV(x1, y0, 1.0f, 0.0f);
			SV(x0, y1, 0.0f, 1.0f);

			SV(x1, y0, 1.0f, 0.0f);
			SV(x1, y1, 1.0f, 1.0f);
			SV(x0, y1, 0.0f, 1.0f);
			#undef SV
		}
		gs_render_stop(GS_TRIS);
	}

	gs_blend_state_pop();
}
