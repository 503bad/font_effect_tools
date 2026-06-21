#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cartoon: retro low-frame-rate "line boil" — the text wobbles in stepped jumps
 * like a hand-drawn animation, with an inky bleed around the edges. Wobble
 * period (frame rate), distortion strength and bleed are configurable. */
extern const struct text_effect fx_cartoon;

#ifdef __cplusplus
}
#endif
