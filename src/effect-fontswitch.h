#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Font switch: the same text is re-rasterized in several user-listed fonts and
 * cycled over time, in order or at random, with a configurable dwell time. */
extern const struct text_effect fx_fontswitch;

#ifdef __cplusplus
}
#endif
