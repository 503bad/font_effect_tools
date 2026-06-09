#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_effect;
typedef struct gs_effect gs_effect_t;
struct gs_texture;
typedef struct gs_texture gs_texture_t;

/* Shared "stroke the text outline" helper. An effect that wants an optional
 * outline loads the shader once in load_graphics (fx_outline_load) and draws it
 * *before* its own fill so the fill covers the interior and only the grown ring
 * remains visible. The helper manages its own (premultiplied) blend state.
 * `rgba` is linear 0..1; `width` is the stroke half-width in pixels. Must run
 * under the OBS graphics lock. */
gs_effect_t *fx_outline_load(void);

/* Outline the whole coverage mask across the full canvas. Used by effects that
 * draw the entire mask (rainbow, spotlight, sparkle, cube faces). `cw`/`ch` are
 * the canvas size in pixels. */
void fx_outline_render_full(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			    uint32_t ch, const float rgba[4], float width);

/* Outline a single glyph sub-region using a quad enlarged by `width` so the
 * grown stroke is not clipped. Drawn at the current matrix (the caller
 * positions/transforms the glyph), letting the outline follow per-character
 * animation. x/y/cx/cy are the glyph rect in texels; cw/ch the canvas size. */
void fx_outline_render_sub(gs_effect_t *e, gs_texture_t *tex, uint32_t cw,
			   uint32_t ch, const float rgba[4], float width,
			   uint32_t x, uint32_t y, uint32_t cx, uint32_t cy);

#ifdef __cplusplus
}
#endif
