#include "effect-registry.h"
#include "effect-base.h"

#include <string.h>

/* Each effect is declared in its own translation unit. */
extern const struct text_effect fx_none;
extern const struct text_effect fx_flame;
extern const struct text_effect fx_rainbow;
extern const struct text_effect fx_neon;
extern const struct text_effect fx_bloom;
extern const struct text_effect fx_water;
extern const struct text_effect fx_glitch;
extern const struct text_effect fx_watersurface;
extern const struct text_effect fx_spotlight;
extern const struct text_effect fx_chroma;
extern const struct text_effect fx_godray;
extern const struct text_effect fx_scanline;
extern const struct text_effect fx_arc;
extern const struct text_effect fx_circuit;
extern const struct text_effect fx_sparkle;
extern const struct text_effect fx_dust;
extern const struct text_effect fx_imagedeco;
extern const struct text_effect fx_depth3d;
extern const struct text_effect fx_cube;
extern const struct text_effect fx_sidebound;
extern const struct text_effect fx_hop;
extern const struct text_effect fx_counter;
extern const struct text_effect fx_slice;
extern const struct text_effect fx_slidein;
extern const struct text_effect fx_jitter;
extern const struct text_effect fx_slime;
extern const struct text_effect fx_wave;
extern const struct text_effect fx_flip;
extern const struct text_effect fx_storyteller;
extern const struct text_effect fx_fontswitch;
extern const struct text_effect fx_roll;
extern const struct text_effect fx_outliner;
extern const struct text_effect fx_puzzle;
extern const struct text_effect fx_cartoon;

static const struct text_effect *const k_effects[] = {
	&fx_none,
	&fx_flame,
	&fx_rainbow,
	&fx_neon,
	&fx_bloom,
	&fx_water,
	&fx_glitch,
	&fx_watersurface,
	&fx_spotlight,
	&fx_chroma,
	&fx_godray,
	&fx_scanline,
	&fx_arc,
	&fx_circuit,
	&fx_sparkle,
	&fx_dust,
	&fx_imagedeco,
	&fx_depth3d,
	&fx_cube,
	&fx_sidebound,
	&fx_hop,
	&fx_counter,
	&fx_slice,
	&fx_slidein,
	&fx_jitter,
	&fx_slime,
	&fx_wave,
	&fx_flip,
	&fx_storyteller,
	&fx_fontswitch,
	&fx_roll,
	&fx_outliner,
	&fx_puzzle,
	&fx_cartoon,
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
