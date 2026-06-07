#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Water drip effect: drops form at the lower tips of the glyphs, swell under
 * surface tension, then run downward leaving a fading wet trail before they
 * dim out. Drops only originate from genuine glyph tips (never from the blank
 * gaps between letters). Amount, frequency, lifetime and color are adjustable. */
extern const struct text_effect fx_water;

#ifdef __cplusplus
}
#endif
