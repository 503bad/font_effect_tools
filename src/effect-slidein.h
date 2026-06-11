#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slide-in: letters fade in one by one, entering alternately from above and
 * below along a tiltable travel direction. Optionally loops (slide back out,
 * wait, re-enter). */
extern const struct text_effect fx_slidein;

#ifdef __cplusplus
}
#endif
