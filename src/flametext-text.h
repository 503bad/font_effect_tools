#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_texture;
typedef struct gs_texture gs_texture_t;

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
};

/* Rasterize utf-8 text with the given font file at the given pixel size into
 * a coverage mask. Bold/italic are synthesized when the face lacks them
 * (matching the dokavendor approach).
 *
 * gs_texture_create runs inside this function, so it MUST be called while
 * holding the OBS graphics lock (obs_enter_graphics()).
 *
 * Returns NULL on failure. The caller owns the result and frees it with
 * flametext_mask_free (also under the graphics lock). */
struct flametext_mask *flametext_mask_build(const char *utf8_text,
					    const char *font_path,
					    uint32_t pixel_size,
					    bool bold,
					    bool italic);

/* Free a mask. Must be called while holding the OBS graphics lock. */
void flametext_mask_free(struct flametext_mask *mask);

#ifdef __cplusplus
}
#endif
