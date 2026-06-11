#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Jitter: every letter floats around its home position independently, hopping
 * between random nearby targets. Loop off shakes forever; loop on alternates
 * a shaking burst with a resting pause. */
extern const struct text_effect fx_jitter;

#ifdef __cplusplus
}
#endif
