#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slice: the text is cut into N parallel diagonal bands that slide in along
 * the cut from opposite sides and lock together. Optionally loops (slide
 * apart, wait, re-enter). */
extern const struct text_effect fx_slice;

#ifdef __cplusplus
}
#endif
