#pragma once

#include <obs-module.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flametext_mask;

/* Per-instance state for a text-effect source.
 *
 * The host owns the shared text/font and the rasterized coverage mask; the
 * actual look is delegated to the selected effect (see effect-base.h). One
 * opaque state blob is held per registered effect so that switching effects
 * keeps each effect's parameters and resources intact. */
struct flametext_source {
	obs_source_t *source;

	/* Shared text / font settings (used across every effect). */
	char    *text;
	char    *font_face;
	uint32_t font_size;
	bool     bold;
	bool     italic;
	char     font_path[1024];

	/* Shared rasterized text coverage mask. */
	struct flametext_mask *mask;

	/* One state blob per registered effect, parallel to fx_registry(). */
	void **states;
	size_t effect_count;
	int    active; /* index of the selected effect in fx_registry() */

	float clock; /* seconds since show() */
};

void flametext_register_source(void);

#ifdef __cplusplus
}
#endif
