#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Korokoro: each letter rolls in one at a time, tumbling and bouncing while it
 * fades in, then optionally rolls back out. Speed, bounce height, roll
 * direction, loop/one-shot, roll-out, and fade in/out are configurable. */
extern const struct text_effect fx_roll;

#ifdef __cplusplus
}
#endif
