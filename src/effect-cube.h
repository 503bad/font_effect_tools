#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cube unfold: the text rides the faces of a cube that tumbles forward; each
 * face carries the text. Optionally ping-pongs (forward then reverse) to loop. */
extern const struct text_effect fx_cube;

#ifdef __cplusplus
}
#endif
