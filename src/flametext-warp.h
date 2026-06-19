#pragma once

#include <obs-module.h>
#include <stdint.h>

#include "effect-base.h" /* struct fx_margins, struct flametext_mask (via text.h) */

#ifdef __cplusplus
extern "C" {
#endif

/* Host-level geometric warp applied to whichever text effect is active. The
 * host captures the effect's frame (and its outer glow) into an offscreen
 * texture, then redraws it through a tessellated grid mesh whose vertices are
 * displaced in a vertex shader. Because it works on the composited frame, the
 * same four deformations apply uniformly to every effect with no per-effect
 * code:
 *
 *   - stretch : scale the text independently in the horizontal and vertical
 *               directions (1 = neutral; below 1 squashes, above 1 stretches).
 *   - arch    : bend the text along a circular arc. One fader curves it up
 *               (∩) for positive values and down (∪) for negative, reaching a
 *               full circle at the extremes.
 *   - persp   : taper the top edge (positive) or the bottom edge (negative)
 *               so that one horizontal edge is narrower (a trapezoid).
 *   - shear_h : slide the top and bottom edges in opposite horizontal
 *               directions (left/right parallelogram).
 *   - shear_v : slide the left and right edges in opposite vertical
 *               directions (up/down parallelogram).
 *
 * Usage from the host render callback (wraps the whole effect+glow):
 *   bool warping = fx_warp_begin(&s->warp, w, h);  // redirects to a capture
 *   ... render effect (+ outer glow) ...
 *   if (warping) fx_warp_end(&s->warp, w, h, s->mask); // warp to the scene
 */
struct fx_warp {
	bool  enabled;
	float scale_x; /* horizontal stretch factor (1 = neutral)        */
	float scale_y; /* vertical stretch factor (1 = neutral)          */
	float arch;    /* -1..1; |1| == full circle (∩ for +, ∪ for -) */
	float persp;   /* -1..1; + tapers the top edge, - the bottom    */
	float shear_h; /* slope; opposite horizontal shift top vs bottom */
	float shear_v; /* slope; opposite vertical shift left vs right   */

	gs_effect_t      *effect;
	gs_vertbuffer_t  *mesh;    /* static tessellated grid (uv in pos+tex) */
	gs_texrender_t   *capture; /* the active effect's composited frame    */
};

/* Load / free GPU resources. Both must run under the OBS graphics lock. */
void fx_warp_load(struct fx_warp *g);
void fx_warp_free(struct fx_warp *g);

void fx_warp_update(struct fx_warp *g, obs_data_t *settings);

/* True when the warp would actually move pixels (enabled and non-neutral). */
bool fx_warp_active(const struct fx_warp *g);

/* Extra canvas room the warp needs so the deformed frame is not clipped.
 * Depends on the text band geometry, so it is computed from a built mask. */
void fx_warp_margins(const struct fx_warp *g, const struct flametext_mask *mask,
		     struct fx_margins *out);

/* Add the shared "warp" checkable group / its setting defaults. */
void fx_warp_get_properties(obs_properties_t *props);
void fx_warp_get_defaults(obs_data_t *settings);

/* Redirect rendering into the capture texture. Returns false (and leaves the
 * render target untouched) when the warp is neutral or resources failed. */
bool fx_warp_begin(struct fx_warp *g, uint32_t w, uint32_t h);

/* Finish the capture and draw the deformed frame to the scene. */
void fx_warp_end(struct fx_warp *g, uint32_t w, uint32_t h,
		 const struct flametext_mask *mask);

#ifdef __cplusplus
}
#endif
