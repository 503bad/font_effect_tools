#include "flametext-text.h"

#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include <obs.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <plugin-support.h>

/* Decode the next UTF-8 codepoint starting at *p, advancing *p. Returns 0 at
 * end of string, 0xFFFD on malformed input. (Same logic as dokavendor.) */
static uint32_t utf8_next(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	if (s[0] == 0)
		return 0;
	uint32_t cp;
	int extra;
	if (s[0] < 0x80) {
		cp = s[0];
		extra = 0;
	} else if ((s[0] & 0xE0) == 0xC0) {
		cp = s[0] & 0x1F;
		extra = 1;
	} else if ((s[0] & 0xF0) == 0xE0) {
		cp = s[0] & 0x0F;
		extra = 2;
	} else if ((s[0] & 0xF8) == 0xF0) {
		cp = s[0] & 0x07;
		extra = 3;
	} else {
		*p = (const char *)(s + 1);
		return 0xFFFD;
	}
	for (int i = 0; i < extra; ++i) {
		if ((s[1 + i] & 0xC0) != 0x80) {
			*p = (const char *)(s + 1 + i);
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[1 + i] & 0x3F);
	}
	*p = (const char *)(s + 1 + extra);
	return cp;
}

/* A glyph rasterized to an 8-bit coverage bitmap, with its pen placement
 * captured so we can blit it after the overall bounds are known. */
struct tmp_glyph {
	unsigned char *gray;   /* w*h, owned */
	int w, h;
	float pen_x;           /* advance origin along the baseline */
	int left;              /* bitmap_left  (px right of pen)     */
	int top;               /* bitmap_top   (px above baseline)   */
};

/* Composite one glyph into the canvas using max-blend so overlapping coverage
 * never darkens. canvas is row-major width*height. */
static void blit_max(unsigned char *canvas, int cw, int ch,
		     const struct tmp_glyph *g, int origin_x, float baseline_y)
{
	int gx0 = origin_x + (int)g->pen_x + g->left;
	int gy0 = (int)baseline_y - g->top;
	for (int y = 0; y < g->h; ++y) {
		int cy = gy0 + y;
		if (cy < 0 || cy >= ch)
			continue;
		const unsigned char *srow = g->gray + (size_t)y * g->w;
		unsigned char *drow = canvas + (size_t)cy * cw;
		for (int x = 0; x < g->w; ++x) {
			int cx = gx0 + x;
			if (cx < 0 || cx >= cw)
				continue;
			if (srow[x] > drow[cx])
				drow[cx] = srow[x];
		}
	}
}

struct flametext_mask *flametext_mask_build(const char *utf8_text,
					    const char *font_path,
					    uint32_t pixel_size,
					    bool bold,
					    bool italic,
					    uint32_t bottom_pad)
{
	if (!utf8_text || !utf8_text[0] || !font_path || !font_path[0] ||
	    pixel_size == 0)
		return NULL;

	FT_Library lib;
	if (FT_Init_FreeType(&lib) != 0) {
		obs_log(LOG_ERROR, "FT_Init_FreeType failed");
		return NULL;
	}
	FT_Face face;
	if (FT_New_Face(lib, font_path, 0, &face) != 0) {
		obs_log(LOG_ERROR, "FT_New_Face failed for %s", font_path);
		FT_Done_FreeType(lib);
		return NULL;
	}
	if (FT_Set_Pixel_Sizes(face, 0, pixel_size) != 0) {
		obs_log(LOG_ERROR, "FT_Set_Pixel_Sizes failed (size=%u)", pixel_size);
		FT_Done_Face(face);
		FT_Done_FreeType(lib);
		return NULL;
	}
	if (italic && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
		FT_Matrix shear = {0x10000, (FT_Fixed)(0.21 * 0x10000), 0, 0x10000};
		FT_Set_Transform(face, &shear, NULL);
	}

	/* Count codepoints to size the temp glyph array. */
	size_t cap = 0;
	for (const char *p = utf8_text; *p;) {
		if (utf8_next(&p) == 0)
			break;
		++cap;
	}
	struct tmp_glyph *tmp = bzalloc(sizeof(*tmp) * (cap + 1));
	size_t tmp_count = 0;

	float pen_x = 0.0f;
	int min_top = 0;     /* most negative top (above baseline)   */
	int max_bottom = 0;  /* most positive bottom (below baseline) */

	const char *p = utf8_text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		if (cp == 0)
			break;
		FT_UInt gi = FT_Get_Char_Index(face, cp);
		if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0)
			continue;
		if (bold)
			FT_Outline_Embolden(&face->glyph->outline,
					    (FT_Pos)(pixel_size / 24 + 1) << 6);
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
			pen_x += (float)(face->glyph->advance.x >> 6);
			continue;
		}

		FT_Bitmap *bm = &face->glyph->bitmap;
		int w = (int)bm->width;
		int h = (int)bm->rows;
		struct tmp_glyph *g = &tmp[tmp_count];
		g->w = w;
		g->h = h;
		g->pen_x = pen_x;
		g->left = face->glyph->bitmap_left;
		g->top = face->glyph->bitmap_top;
		if (w > 0 && h > 0) {
			g->gray = bmalloc((size_t)w * h);
			for (int y = 0; y < h; ++y)
				memcpy(g->gray + (size_t)y * w,
				       bm->buffer + (size_t)y * bm->pitch, w);

			int top = -g->top;          /* canvas-up is negative */
			int bottom = top + h;
			if (top < min_top)
				min_top = top;
			if (bottom > max_bottom)
				max_bottom = bottom;
		}
		++tmp_count;
		pen_x += (float)(face->glyph->advance.x >> 6);
	}

	FT_Done_Face(face);
	FT_Done_FreeType(lib);

	if (tmp_count == 0 || pen_x < 1.0f) {
		for (size_t i = 0; i < tmp_count; ++i)
			bfree(tmp[i].gray);
		bfree(tmp);
		return NULL;
	}

	/* Padding generous enough for the flame to rise and sparks to travel.
	 * Top gets the most room; sides are modest. The bottom room is chosen
	 * by the caller (effects that drip downward ask for more); fall back to
	 * a small default. */
	const int pad_x = (int)(pixel_size * 0.8f);
	const int pad_top = (int)(pixel_size * 2.4f);
	const int pad_bottom = bottom_pad > 0 ? (int)bottom_pad
					      : (int)(pixel_size * 0.6f);

	int text_w = (int)(pen_x + 0.5f);
	int text_h = max_bottom - min_top;
	if (text_h < 1)
		text_h = 1;

	int cw = text_w + pad_x * 2;
	int ch = text_h + pad_top + pad_bottom;

	/* baseline so the topmost text pixel lands at y = pad_top. */
	float baseline_y = (float)(pad_top - min_top);

	unsigned char *canvas = bzalloc((size_t)cw * ch);
	for (size_t i = 0; i < tmp_count; ++i) {
		if (tmp[i].gray)
			blit_max(canvas, cw, ch, &tmp[i], pad_x, baseline_y);
		bfree(tmp[i].gray);
	}
	bfree(tmp);

	/* Bottom contour: lowest inked pixel per column (-1 = empty column).
	 * "Inked" uses a low coverage threshold so faint antialiased edges do
	 * not register as drip-worthy tips. */
	int *bottom_y = bmalloc(sizeof(int) * (size_t)cw);
	for (int x = 0; x < cw; ++x) {
		int lowest = -1;
		for (int y = ch - 1; y >= 0; --y) {
			if (canvas[(size_t)y * cw + x] >= 40) {
				lowest = y;
				break;
			}
		}
		bottom_y[x] = lowest;
	}

	const uint8_t *data_ptrs[1] = {(const uint8_t *)canvas};
	gs_texture_t *tex = gs_texture_create((uint32_t)cw, (uint32_t)ch,
					      GS_R8, 1, data_ptrs, 0);
	bfree(canvas);
	if (!tex) {
		obs_log(LOG_ERROR, "gs_texture_create failed for text mask");
		bfree(bottom_y);
		return NULL;
	}

	struct flametext_mask *m = bzalloc(sizeof(*m));
	m->tex = tex;
	m->width = (uint32_t)cw;
	m->height = (uint32_t)ch;
	m->text_left = (float)pad_x;
	m->text_right = (float)(pad_x + text_w);
	m->text_top = (float)pad_top;
	m->text_bottom = (float)(pad_top + text_h);
	m->bottom_y = bottom_y;
	return m;
}

void flametext_mask_free(struct flametext_mask *mask)
{
	if (!mask)
		return;
	if (mask->tex)
		gs_texture_destroy(mask->tex);
	bfree(mask->bottom_y);
	bfree(mask);
}
