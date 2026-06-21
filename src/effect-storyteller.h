#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storyteller: a Star-Wars-style perspective crawl. The text recedes toward a
 * vanishing point near the top of the canvas, scrolling from the foreground
 * (bottom) into the distance. Speed, fade in/out and loop/one-shot are
 * configurable. */
extern const struct text_effect fx_storyteller;

#ifdef __cplusplus
}
#endif
