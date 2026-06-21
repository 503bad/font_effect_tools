#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Outliner: a glowing pen traces the glyph contours, leaving a soft tail that
 * gently fades. Only the line is shown (no fill). Direction, speed, line width,
 * tail length/color/lifetime, bloom and glow are configurable. */
extern const struct text_effect fx_outliner;

#ifdef __cplusplus
}
#endif
