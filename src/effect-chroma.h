#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chromatic glow: an RGB-split (chromatic aberration) glow whose hue rotates
 * over time. Hue speed and intensity are configurable. */
extern const struct text_effect fx_chroma;

#ifdef __cplusplus
}
#endif
