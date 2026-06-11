#pragma once

#include <obs-module.h>
#include <stddef.h>

#include "flametext-outerglow.h"

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

	/* Baseline-to-baseline line pitch in pixels for multi-line text;
	 * 0 = auto (use the font's natural line height). Shared across effects. */
	int      line_spacing;

	/* Extra pixels added to every glyph advance (letter spacing); may be
	 * negative to tighten. 0 = auto (the font's natural advances). Shared
	 * across effects. `letter_manual` remembers the UI mode so the pixel
	 * slider stays visible even at an effective spacing of 0. */
	int      letter_spacing;
	bool     letter_manual;

	/* Horizontal alignment of each line within the text block
	 * (enum flametext_align). In vertical mode the same values align
	 * each column top/center/bottom. Shared across effects. */
	int      align;

	/* Writing direction: 0 = horizontal, 1 = vertical (tategaki).
	 * Shared across effects. */
	int      writing_dir;

	/* Shared rasterized text coverage mask. */
	struct flametext_mask *mask;

	/* Host-level outer glow layered behind the active effect's output. */
	struct fx_outerglow oglow;

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
