#pragma once

#include <obs-module.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Photoshop-style "outer glow" layer effect owned by the host and applied to
 * whichever text effect is active: the host captures the effect's frame into
 * an offscreen texture, blurs its alpha into a glow field, lays the coloured
 * halo down on the scene first (so it sits behind everything the effect
 * drew), then composites the captured frame on top.
 *
 * Usage from the host render callback:
 *   if (fx_oglow_begin(&s->oglow, w, h)) {   // redirects to the capture
 *           fx->render(state, &ctx);
 *           fx_oglow_end(&s->oglow, w, h);   // glow + body to the scene
 *   } else {
 *           fx->render(state, &ctx);         // disabled: direct path
 *   }
 */

enum oglow_blend {
	OGLOW_BLEND_NORMAL = 0,
	OGLOW_BLEND_SCREEN,
	OGLOW_BLEND_MULTIPLY,
	OGLOW_BLEND_HARDLIGHT,
	OGLOW_BLEND_COLORBURN,
};

struct fx_outerglow {
	bool  enabled;
	float rgba[4]; /* rgb = glow colour, a = opacity 0..1 */
	float size;    /* reach of the halo in pixels         */
	float spread;  /* 0..1 chokes the falloff outward     */
	float blur;    /* 0..1 softness of the gradient       */
	int   blend;   /* enum oglow_blend                    */

	gs_effect_t    *effect;
	gs_texrender_t *capture; /* the active effect's frame      */
	gs_texrender_t *blur_a;  /* horizontal blur of its alpha   */
	gs_texrender_t *blur_b;  /* vertical blur -> the glow field */
};

/* Load / free GPU resources. Both must run under the OBS graphics lock. */
void fx_oglow_load(struct fx_outerglow *g);
void fx_oglow_free(struct fx_outerglow *g);

void fx_oglow_update(struct fx_outerglow *g, obs_data_t *settings);

/* Extra canvas padding (px per side) needed so the halo is not clipped. */
uint32_t fx_oglow_margin(const struct fx_outerglow *g);

/* Add the shared "outer glow" checkable group / its setting defaults. */
void fx_oglow_get_properties(obs_properties_t *props);
void fx_oglow_get_defaults(obs_data_t *settings);

/* Redirect rendering into the capture texture. Returns false (and leaves the
 * render target untouched) when the glow is disabled or resources failed. */
bool fx_oglow_begin(struct fx_outerglow *g, uint32_t w, uint32_t h);

/* Finish the capture, draw the glow then the captured frame to the scene. */
void fx_oglow_end(struct fx_outerglow *g, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif
