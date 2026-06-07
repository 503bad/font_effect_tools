#include "effect-registry.h"
#include "effect-base.h"

#include <string.h>

/* Each effect is declared in its own translation unit. */
extern const struct text_effect fx_flame;
extern const struct text_effect fx_rainbow;
extern const struct text_effect fx_neon;
extern const struct text_effect fx_bloom;

static const struct text_effect *const k_effects[] = {
	&fx_flame,
	&fx_rainbow,
	&fx_neon,
	&fx_bloom,
};

const struct text_effect *const *fx_registry(size_t *count)
{
	if (count)
		*count = sizeof(k_effects) / sizeof(k_effects[0]);
	return k_effects;
}

int fx_registry_index(const char *id)
{
	if (!id)
		return -1;
	size_t n;
	const struct text_effect *const *e = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (strcmp(e[i]->id, id) == 0)
			return (int)i;
	}
	return -1;
}
