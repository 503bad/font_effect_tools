#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scanlines: CRT-style scan lines with bleed, gentle wobble and a faint
 * flicker. The overall strength is configurable. */
extern const struct text_effect fx_scanline;

#ifdef __cplusplus
}
#endif
