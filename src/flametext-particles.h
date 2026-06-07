#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;

/* One CPU-managed ember. Position/velocity are in canvas pixels (y grows
 * downward); life counts down in seconds. */
typedef struct {
	float x, y;
	float vx, vy;
	float life;      /* remaining seconds            */
	float max_life;  /* initial life (for fade ratio) */
	float size;      /* half-extent in px            */
	float seed;      /* 0..1 per-particle variation  */
} spark_t;

/* The spark system: a fixed-capacity pool plus emission/physics parameters.
 * Live particles occupy [0, live) and are swap-removed on death. */
struct spark_system {
	spark_t *sparks;
	size_t   capacity;
	size_t   live;

	/* Tunables (set from properties). */
	float emit_rate;    /* particles per second        */
	float init_speed;   /* base upward speed (px/s)    */
	float lifetime;     /* base lifetime (seconds)     */
	float base_size;    /* base half-size (px)         */
	float spread;       /* 0..1 randomness             */

	float bloom;        /* additive glow strength (>1 = stronger) */

	/* Ember colors (linear 0..1 rgba), derived from the picked color. */
	float hot[4];
	float cool[4];

	/* Emission band in canvas px. emit_y is the edge embers lift off from;
	 * emit_dir is -1 to nudge spawns above that edge (text top), +1 below
	 * it (text bottom). */
	float emit_left, emit_right, emit_y;
	float emit_dir;

	float    emit_accum; /* fractional spawn carry        */
	uint32_t rng;        /* xorshift state                */
	float    phase;      /* accumulated time for sway     */
};

struct spark_system *flametext_particles_create(size_t capacity);
void flametext_particles_destroy(struct spark_system *sys);

/* Update emitter band (call when the text mask changes). `dir` is -1 to emit
 * above `edge_y` (off the text top) or +1 to emit below it (off the bottom). */
void flametext_particles_set_emitter(struct spark_system *sys,
				     float left, float right, float edge_y,
				     float dir);

/* Advance simulation by dt seconds: spawn, integrate, retire. */
void flametext_particles_tick(struct spark_system *sys, float dt);

/* Draw all live sparks additively. Must run under the OBS graphics lock,
 * inside the source's video_render. `effect` is the loaded spark effect. */
void flametext_particles_render(struct spark_system *sys, gs_effect_t *effect);

/* Reset the pool (e.g. on show()). */
void flametext_particles_reset(struct spark_system *sys);

#ifdef __cplusplus
}
#endif
