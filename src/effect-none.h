#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No effect: plain static text equivalent to OBS's standard text source —
 * solid fill colour with an optional outline, drop shadow, and background. */
extern const struct text_effect fx_none;

#ifdef __cplusplus
}
#endif
