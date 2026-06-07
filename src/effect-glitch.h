#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Glitch effect: renders the text in a chosen color and, at a configurable
 * rate, snaps it into a short digital "glitch" burst — RGB channel split
 * (chromatic aberration), horizontal slice displacement, and block dropout.
 * Between bursts the text sits clean and still. */
extern const struct text_effect fx_glitch;

#ifdef __cplusplus
}
#endif
