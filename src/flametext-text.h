#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_texture;
typedef struct gs_texture gs_texture_t;

/* Canvas-space rectangle (pixels) of a single rasterized glyph within the
 * shared mask texture. Lets per-character effects draw and animate each glyph
 * as its own quad sampling its sub-region of the one coverage texture. Empty
 * glyphs (spaces) are not included. Owned by the mask. */
struct flametext_glyph {
	float x, y;  /* top-left of the glyph bitmap on the canvas */
	float w, h;  /* glyph bitmap size in pixels                */
};

/* A rasterized text "coverage" mask used as the source shape for the flame
 * shader and the spark emitter.
 *
 * The mask is a single GS_R8 texture sized to the full padded canvas: the
 * glyphs are baked into it (8-bit coverage in the red channel) and placed so
 * that generous empty space remains above the text for the flame to rise into
 * and for sparks to travel through.
 *
 * All geometry is in canvas pixels with (0,0) at the top-left. */
struct flametext_mask {
	gs_texture_t *tex;

	uint32_t width;   /* canvas width  (text + horizontal padding) */
	uint32_t height;  /* canvas height (text + top/bottom padding) */

	/* Bounding band of the actual text within the canvas. The spark
	 * emitter releases particles from near `text_top` between
	 * text_left..text_right. */
	float text_left;
	float text_right;
	float text_top;     /* topmost text pixel (small y) */
	float text_bottom;  /* lowest text pixel (large y) */

	/* Per-column bottom contour: for each canvas column x, the y of the
	 * lowest inked pixel in that column, or -1 where the column is empty.
	 * Length == `width`. Lets effects find the genuine lower tips of the
	 * glyphs (and avoid emitting from blank gaps). Owned by the mask. */
	int *bottom_y;

	/* Per visible glyph, in left-to-right order. Used by per-character
	 * effects (bounce-in, hop, ...) to move each letter independently.
	 * Length == `glyph_count` (may be 0). Owned by the mask. */
	struct flametext_glyph *glyphs;
	size_t glyph_count;
};

/* Rasterize utf-8 text with the given font file at the given pixel size into
 * a coverage mask. Bold/italic are synthesized when the face lacks them
 * (matching the dokavendor approach).
 *
 * gs_texture_create runs inside this function, so it MUST be called while
 * holding the OBS graphics lock (obs_enter_graphics()).
 *
 * Text is laid out across multiple lines on embedded '\n' characters, with each
 * line centered horizontally. `line_spacing` is the baseline-to-baseline pitch
 * in pixels; pass <= 0 to use the font's natural line height (auto).
 *
 * `bottom_pad` is the empty room (in pixels) reserved below the text; pass 0
 * to use a small default. Effects that need drops/embers to travel downward
 * ask for a larger value. `extra_left`/`extra_right`/`extra_top` add room on
 * the respective sides beyond the host defaults (pass 0 for none); effects whose
 * output streams sideways or upward (god rays, etc.) ask for more.
 *
 * Returns NULL on failure. The caller owns the result and frees it with
 * flametext_mask_free (also under the graphics lock). */
struct flametext_mask *flametext_mask_build(const char *utf8_text,
					    const char *font_path,
					    uint32_t pixel_size,
					    bool bold,
					    bool italic,
					    int line_spacing,
					    uint32_t bottom_pad,
					    uint32_t extra_left,
					    uint32_t extra_right,
					    uint32_t extra_top);

/* Free a mask. Must be called while holding the OBS graphics lock. */
void flametext_mask_free(struct flametext_mask *mask);

#ifdef __cplusplus
}
#endif
