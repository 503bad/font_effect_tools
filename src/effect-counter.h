#pragma once

#include "effect-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Counter: every letter spins through the characters that precede it in
 * codepoint order (skipping ones the font cannot draw) and the text locks in
 * from the left, slot-machine style, over a configurable duration. */
extern const struct text_effect fx_counter;

#ifdef __cplusplus
}
#endif
