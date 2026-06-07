#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rainbow fill effect: fills the glyphs with an animated, scrolling
 * hue gradient. A deliberately simple second effect that shares the same text
 * mask as the flame effect, demonstrating the multi-effect structure. */
extern const struct text_effect fx_rainbow;

#ifdef __cplusplus
}
#endif
