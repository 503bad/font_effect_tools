#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Water surface effect: draws the text upright in the chosen color and, below
 * it, a vertically flipped reflection on a rippling water surface. The
 * reflection wobbles (either as horizontal ripples or as concentric rings
 * spreading from the centre) and fades out with depth. Glow and bloom are
 * applied to both the upright text and its reflection. */
extern const struct text_effect fx_watersurface;

#ifdef __cplusplus
}
#endif
