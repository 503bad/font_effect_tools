#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Neon-tube effect: traces the glyph contour as a glowing neon tube. The text
 * interior is left hollow (invisible) — only the outline glows. */
extern const struct text_effect fx_neon;

#ifdef __cplusplus
}
#endif
