#pragma once

#include <obs-module.h>
#include <stdint.h>

#include "flametext-text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only frame context handed to an effect each tick/render. The text
 * coverage mask is owned by the host and shared across every effect — it is
 * the one piece of state that is common to all effect types (the text input).
 * `mask` may be NULL when there is no text yet. */
struct fx_render_ctx {
	const struct flametext_mask *mask;
	float    time;   /* seconds since the source was last shown */
	uint32_t width;  /* canvas width  (== mask->width  when present) */
	uint32_t height; /* canvas height (== mask->height when present) */
};

/* A selectable text effect. Each effect owns a per-source-instance, opaque
 * `state` blob and a set of namespaced settings keys; the host owns the shared
 * text/font/mask and decides which effect is active.
 *
 * Threading contract (mirrors OBS source callbacks):
 *   - load_graphics / render / destroy run with the OBS graphics lock held
 *     (the host wraps them in obs_enter_graphics()).
 *   - create / update / tick / set_mask / reset / get_properties /
 *     get_defaults run without the graphics lock.
 * Any callback may be NULL if the effect does not need it. */
struct text_effect {
	const char *id;       /* stable id stored in settings, e.g. "flame" */
	const char *name_key; /* locale key shown in the effect selector     */

	void *(*create)(void);
	void  (*destroy)(void *state);

	/* Load GPU resources (shaders, etc.). */
	void  (*load_graphics)(void *state);

	/* Pull this effect's own parameters out of `settings`. */
	void  (*update)(void *state, obs_data_t *settings);

	/* How much empty room (in pixels) this effect wants reserved below the
	 * text on the shared canvas, given the current font pixel size. Called
	 * after update(), so it may depend on settings. Return 0 (or leave NULL)
	 * for the host default. The water drip effect uses this to make room for
	 * drops to fall the requested distance. */
	uint32_t (*wanted_bottom_pad)(void *state, uint32_t font_size);

	/* The shared text mask was (re)built; refresh anything derived from
	 * its geometry (emitter bands, etc.). `mask` may be NULL. */
	void  (*set_mask)(void *state, const struct flametext_mask *mask);

	/* Advance any simulation by `dt` seconds. */
	void  (*tick)(void *state, const struct fx_render_ctx *ctx, float dt);

	/* Draw one frame. */
	void  (*render)(void *state, const struct fx_render_ctx *ctx);

	/* Reset transient state, e.g. on show(). */
	void  (*reset)(void *state);

	/* Add this effect's properties to `group`. */
	void  (*get_properties)(obs_properties_t *group);

	/* Provide this effect's setting defaults. */
	void  (*get_defaults)(obs_data_t *settings);
};

#ifdef __cplusplus
}
#endif
