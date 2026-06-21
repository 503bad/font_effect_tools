#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Puzzle: the text is cut into a grid of rectangular pieces that slide in from
 * the sides to assemble the word. Piece granularity, speed, slide order
 * (from left / from right / all), bloom and glow are configurable. */
extern const struct text_effect fx_puzzle;

#ifdef __cplusplus
}
#endif
