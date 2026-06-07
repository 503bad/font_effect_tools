#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bloom effect: fills the glyph solid in the chosen color, whitens its core,
 * and radiates a soft halo (bloom) outward. The bloom strength varies randomly
 * over time and a slight spatial shimmer wavers across it. */
extern const struct text_effect fx_bloom;

#ifdef __cplusplus
}
#endif
