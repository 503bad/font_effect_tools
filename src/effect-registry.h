#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct text_effect;

/* The static list of selectable effects. Add a new effect by implementing the
 * text_effect interface and appending it in effect-registry.c. */
const struct text_effect *const *fx_registry(size_t *count);

/* Index of the effect with the given id, or -1 if unknown / NULL. */
int fx_registry_index(const char *id);

#ifdef __cplusplus
}
#endif
