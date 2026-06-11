#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;
struct gs_texture;
typedef struct gs_texture gs_texture_t;

/* Shared "soft halo behind the text" helper, the glow counterpart of
 * flametext-outline. An effect that wants an optional glow loads the shader
 * once in load_graphics (fx_glow_load) and draws it *before* its own fill so
 * the haze sits behind the letters. The helper manages its own (premultiplied)
 * blend state. `rgba` is the halo colour (linear 0..1); `intensity` is the
 * strength (0 disables, ~1 typical, 3 max). The halo radius scales with the
 * intensity. Must run under the OBS graphics lock. */
gs_effect_t *fx_glow_load(void);

/* Glow the whole coverage mask across the full canvas. */
void fx_glow_render_full(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			 uint32_t ch, const float rgba[4], float intensity);

/* Glow a single glyph sub-region using a quad enlarged by the halo radius so
 * the haze is not clipped. Drawn at the current matrix (the caller
 * positions/transforms the glyph). x/y/cx/cy are the glyph rect in texels;
 * cw/ch the texture size. */
void fx_glow_render_sub(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			uint32_t ch, const float rgba[4], float intensity,
			uint32_t x, uint32_t y, uint32_t cx, uint32_t cy);

#ifdef __cplusplus
}
#endif
