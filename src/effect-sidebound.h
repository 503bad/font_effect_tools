#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Side bounce align: letters bounce in from one side, collide with the previous
 * letter and stop in place; after a hold they slide out the opposite side. The
 * whole sequence loops. */
extern const struct text_effect fx_sidebound;

#ifdef __cplusplus
}
#endif
