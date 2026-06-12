#include "flametext-text.h"
#include "flametext-ftload.h"

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
	float pen_x;           /* advance origin along the line baseline */
	int left;              /* bitmap_left  (px right of pen)     */
	int top;               /* bitmap_top   (px above baseline)   */
	int line;              /* line index (0-based) for layout    */
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

/* A glyph rasterized for the vertical (tategaki) layout, captured with its
 * column/row cell so it can be placed once the column heights are known. */
struct vtmp_glyph {
	unsigned char *gray; /* w*h, owned */
	int w, h;
	int left;            /* bitmap_left (px right of pen)   */
	int top;             /* bitmap_top  (px above baseline) */
	int col;             /* column index (0 = first = rightmost) */
	int row;             /* cell index from the column top  */
};

/* Vertical (tategaki) layout. Characters advance downward by one em
 * (+ letter_spacing) inside each column; '\n' starts a new column and the
 * columns stack right-to-left with `col_pitch` between column centres. Each
 * glyph keeps its horizontal-baseline metrics: a per-cell baseline sits at
 * the face ascender below the cell top, and the bitmap is centred
 * horizontally on the column axis. `align` picks top/center/bottom of each
 * column against the tallest one. glyphs[] is filled in reading order
 * (top-to-bottom, then right-to-left). Mirrors the horizontal build path;
 * the caller still owns `face`. */
static struct flametext_mask *build_vertical(FT_Face face,
					     const char *utf8_text,
					     uint32_t pixel_size,
					     bool bold,
					     int col_pitch,
					     int letter_spacing,
					     int align,
					     uint32_t bottom_pad,
					     uint32_t extra_left,
					     uint32_t extra_right,
					     uint32_t extra_top)
{
	int step = (int)pixel_size + letter_spacing;
	if (step < 1)
		step = 1;
	int asc = (int)(face->size->metrics.ascender >> 6);
	if (asc <= 0)
		asc = (int)(pixel_size * 4 / 5);

	/* Count codepoints (upper bound on glyphs) and columns ('\n'). */
	size_t cap = 0;
	int col_total = 1;
	for (const char *q = utf8_text; *q;) {
		uint32_t cp = utf8_next(&q);
		if (cp == 0)
			break;
		if (cp == '\n')
			++col_total;
		else if (cp != '\r')
			++cap;
	}
	struct vtmp_glyph *tmp = bzalloc(sizeof(*tmp) * (cap + 1));
	size_t tmp_count = 0;
	int *col_cells = bzalloc(sizeof(int) * (size_t)col_total);

	int cur_col = 0;
	const char *p = utf8_text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		if (cp == 0)
			break;
		if (cp == '\r')
			continue;
		if (cp == '\n') {
			++cur_col;
			continue;
		}
		/* Every character (including spaces and failed glyphs)
		 * consumes one cell so gaps survive in the column. */
		int row = col_cells[cur_col]++;
		FT_UInt gi = FT_Get_Char_Index(face, cp);
		if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0)
			continue;
		if (bold)
			FT_Outline_Embolden(&face->glyph->outline,
					    (FT_Pos)(pixel_size / 24 + 1) << 6);
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
			continue;
		FT_Bitmap *bm = &face->glyph->bitmap;
		int w = (int)bm->width;
		int h = (int)bm->rows;
		if (w <= 0 || h <= 0)
			continue;
		struct vtmp_glyph *g = &tmp[tmp_count];
		g->w = w;
		g->h = h;
		g->left = face->glyph->bitmap_left;
		g->top = face->glyph->bitmap_top;
		g->col = cur_col;
		g->row = row;
		g->gray = bmalloc((size_t)w * h);
		for (int y = 0; y < h; ++y)
			memcpy(g->gray + (size_t)y * w,
			       bm->buffer + (size_t)y * bm->pitch, w);
		++tmp_count;
	}

	if (tmp_count == 0) {
		bfree(tmp);
		bfree(col_cells);
		return NULL;
	}

	/* The tallest column drives the block height; trailing letter
	 * spacing does not count (same rule as the horizontal line width). */
	int max_col_h = 0;
	int *col_h = bmalloc(sizeof(int) * (size_t)col_total);
	for (int c = 0; c < col_total; ++c) {
		int h = col_cells[c] > 0 ? col_cells[c] * step - letter_spacing
					 : 0;
		if (h < 0)
			h = 0;
		col_h[c] = h;
		if (h > max_col_h)
			max_col_h = h;
	}
	if (max_col_h < 1)
		max_col_h = 1;

	const int pad_x = (int)(pixel_size * 0.8f);
	const int pad_left = pad_x + (int)extra_left;
	const int pad_right = pad_x + (int)extra_right;
	const int pad_top = (int)(pixel_size * 2.4f) + (int)extra_top;
	const int pad_bottom = bottom_pad > 0 ? (int)bottom_pad
					      : (int)(pixel_size * 0.6f);

	int text_w = (col_total - 1) * col_pitch + (int)pixel_size;
	int text_h = max_col_h;

	int cw = text_w + pad_left + pad_right;
	int ch = text_h + pad_top + pad_bottom;

	struct flametext_glyph *glyphs =
		bmalloc(sizeof(*glyphs) * tmp_count);
	size_t glyph_count = 0;

	unsigned char *canvas = bzalloc((size_t)cw * ch);
	for (size_t i = 0; i < tmp_count; ++i) {
		const struct vtmp_glyph *g = &tmp[i];
		float slack = (float)(max_col_h - col_h[g->col]);
		float top_off = 0.0f;
		if (align == FLAMETEXT_ALIGN_RIGHT) /* bottom */
			top_off = slack;
		else if (align != FLAMETEXT_ALIGN_LEFT) /* center */
			top_off = slack * 0.5f;
		/* Column 0 (first in reading order) is the rightmost. */
		int col_cx = pad_left + text_w - (int)pixel_size / 2 -
			     g->col * col_pitch;
		int gx0 = col_cx - g->w / 2;
		int gy0 = pad_top + (int)top_off + g->row * step + asc -
			  g->top;
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
		/* Clamp the exported rect to the canvas: a glyph wider than one
		 * em sticks out of its column and per-character effects cast
		 * these to unsigned texel offsets. */
		int rx0 = gx0 < 0 ? 0 : gx0;
		int ry0 = gy0 < 0 ? 0 : gy0;
		int rx1 = gx0 + g->w > cw ? cw : gx0 + g->w;
		int ry1 = gy0 + g->h > ch ? ch : gy0 + g->h;
		if (rx1 > rx0 && ry1 > ry0) {
			struct flametext_glyph *gl = &glyphs[glyph_count++];
			gl->x = (float)rx0;
			gl->y = (float)ry0;
			gl->w = (float)(rx1 - rx0);
			gl->h = (float)(ry1 - ry0);
		}
		bfree(g->gray);
	}
	bfree(tmp);
	bfree(col_cells);
	bfree(col_h);

	/* Bottom contour: lowest inked pixel per column (-1 = empty), same
	 * threshold as the horizontal path. */
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
		bfree(glyphs);
		return NULL;
	}

	struct flametext_mask *m = bzalloc(sizeof(*m));
	m->tex = tex;
	m->width = (uint32_t)cw;
	m->height = (uint32_t)ch;
	m->text_left = (float)pad_left;
	m->text_right = (float)(pad_left + text_w);
	m->text_top = (float)pad_top;
	m->text_bottom = (float)(pad_top + text_h);
	m->bottom_y = bottom_y;
	m->glyphs = glyphs;
	m->glyph_count = glyph_count;
	return m;
}

struct flametext_mask *flametext_mask_build(const char *utf8_text,
					    const char *font_path,
					    uint32_t pixel_size,
					    bool bold,
					    bool italic,
					    int line_spacing,
					    int letter_spacing,
					    int align,
					    uint32_t bottom_pad,
					    uint32_t extra_left,
					    uint32_t extra_right,
					    uint32_t extra_top,
					    bool vertical)
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
	void *face_data = NULL;
	if (!flametext_ft_new_face_utf8(lib, font_path, 0, &face, &face_data)) {
		obs_log(LOG_ERROR, "could not open font file %s", font_path);
		FT_Done_FreeType(lib);
		return NULL;
	}
	if (FT_Set_Pixel_Sizes(face, 0, pixel_size) != 0) {
		obs_log(LOG_ERROR, "FT_Set_Pixel_Sizes failed (size=%u)", pixel_size);
		FT_Done_Face(face);
		bfree(face_data);
		FT_Done_FreeType(lib);
		return NULL;
	}
	if (italic && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
		FT_Matrix shear = {0x10000, (FT_Fixed)(0.21 * 0x10000), 0, 0x10000};
		FT_Set_Transform(face, &shear, NULL);
	}

	/* Line pitch (baseline-to-baseline). Auto uses FreeType's recommended
	 * line spacing for the face; a positive line_spacing overrides it with an
	 * explicit pixel value. */
	int pitch = line_spacing > 0 ? line_spacing
				     : (int)(face->size->metrics.height >> 6);
	if (pitch < 1)
		pitch = (int)pixel_size;

	/* Vertical writing takes its own, self-contained layout path; the
	 * pitch above becomes the column-to-column distance. */
	if (vertical) {
		struct flametext_mask *m = build_vertical(face, utf8_text,
							  pixel_size, bold,
							  pitch, letter_spacing,
							  align, bottom_pad,
							  extra_left,
							  extra_right,
							  extra_top);
		FT_Done_Face(face);
		bfree(face_data);
		FT_Done_FreeType(lib);
		return m;
	}

	/* Count codepoints (upper bound on glyphs) and lines (split on '\n'). */
	size_t cap = 0;
	int line_total = 1;
	for (const char *q = utf8_text; *q;) {
		uint32_t cp = utf8_next(&q);
		if (cp == 0)
			break;
		if (cp == '\n')
			++line_total;
		else if (cp != '\r')
			++cap;
	}
	struct tmp_glyph *tmp = bzalloc(sizeof(*tmp) * (cap + 1));
	size_t tmp_count = 0;
	float *line_w = bzalloc(sizeof(float) * (size_t)line_total);

	/* The last glyph on a line contributes its advance but no trailing
	 * letter spacing, so the measured width drops one spacing unit when
	 * the line holds at least one advanced character. */
	const float lsp = (float)letter_spacing;
	float pen_x = 0.0f;
	int cur_line = 0;
	bool line_any = false;

	const char *p = utf8_text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		if (cp == 0)
			break;
		if (cp == '\r')
			continue;
		if (cp == '\n') {
			line_w[cur_line] = line_any ? pen_x - lsp : pen_x;
			if (line_w[cur_line] < 0.0f)
				line_w[cur_line] = 0.0f;
			++cur_line;
			pen_x = 0.0f;
			line_any = false;
			continue;
		}
		FT_UInt gi = FT_Get_Char_Index(face, cp);
		if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0)
			continue;
		if (bold)
			FT_Outline_Embolden(&face->glyph->outline,
					    (FT_Pos)(pixel_size / 24 + 1) << 6);
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
			pen_x += (float)(face->glyph->advance.x >> 6) + lsp;
			line_any = true;
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
		g->line = cur_line;
		if (w > 0 && h > 0) {
			g->gray = bmalloc((size_t)w * h);
			for (int y = 0; y < h; ++y)
				memcpy(g->gray + (size_t)y * w,
				       bm->buffer + (size_t)y * bm->pitch, w);
		}
		++tmp_count;
		pen_x += (float)(face->glyph->advance.x >> 6) + lsp;
		line_any = true;
	}
	line_w[cur_line] = line_any ? pen_x - lsp : pen_x;
	if (line_w[cur_line] < 0.0f)
		line_w[cur_line] = 0.0f;

	FT_Done_Face(face);
	bfree(face_data);
	FT_Done_FreeType(lib);

	/* Widest line drives the canvas width; the block's vertical extent is
	 * measured in baseline-local space where line i's baseline sits at
	 * i*pitch and a glyph's top pixel (canvas-down) is i*pitch - bitmap_top. */
	float max_line_w = 0.0f;
	for (int i = 0; i < line_total; ++i)
		if (line_w[i] > max_line_w)
			max_line_w = line_w[i];

	int min_y = 0;
	int max_y = 0;
	bool any = false;
	for (size_t i = 0; i < tmp_count; ++i) {
		if (!tmp[i].gray)
			continue;
		int top = tmp[i].line * pitch - tmp[i].top;
		int bottom = top + tmp[i].h;
		if (!any || top < min_y)
			min_y = top;
		if (!any || bottom > max_y)
			max_y = bottom;
		any = true;
	}

	if (tmp_count == 0 || !any) {
		for (size_t i = 0; i < tmp_count; ++i)
			bfree(tmp[i].gray);
		bfree(tmp);
		bfree(line_w);
		return NULL;
	}
	/* Strongly negative letter spacing can collapse the measured width even
	 * though glyph bitmaps remain visible; keep a minimal canvas instead of
	 * failing the build. */
	if (max_line_w < 1.0f)
		max_line_w = 1.0f;

	/* Padding generous enough for the flame to rise and sparks to travel.
	 * Top gets the most room; sides are modest. The bottom room is chosen
	 * by the caller (effects that drip downward ask for more); fall back to
	 * a small default. */
	const int pad_x = (int)(pixel_size * 0.8f);
	const int pad_left = pad_x + (int)extra_left;
	const int pad_right = pad_x + (int)extra_right;
	const int pad_top = (int)(pixel_size * 2.4f) + (int)extra_top;
	const int pad_bottom = bottom_pad > 0 ? (int)bottom_pad
					      : (int)(pixel_size * 0.6f);

	int text_w = (int)(max_line_w + 0.5f);
	int text_h = max_y - min_y;
	if (text_h < 1)
		text_h = 1;

	int cw = text_w + pad_left + pad_right;
	int ch = text_h + pad_top + pad_bottom;

	/* Capture each visible glyph's canvas rectangle so per-character effects
	 * can address letters individually. Rect math mirrors blit_max(). Lines
	 * are aligned within the block per `align`; line i's baseline is placed
	 * at pad_top - min_y + i*pitch so the topmost pixel of the whole block
	 * lands at y = pad_top. */
	struct flametext_glyph *glyphs =
		bmalloc(sizeof(*glyphs) * (tmp_count ? tmp_count : 1));
	size_t glyph_count = 0;

	unsigned char *canvas = bzalloc((size_t)cw * ch);
	for (size_t i = 0; i < tmp_count; ++i) {
		if (tmp[i].gray) {
			int line = tmp[i].line;
			float slack = max_line_w - line_w[line];
			int origin_x = pad_left;
			if (align == FLAMETEXT_ALIGN_RIGHT)
				origin_x += (int)slack;
			else if (align != FLAMETEXT_ALIGN_LEFT)
				origin_x += (int)(slack * 0.5f);
			float baseline_y =
				(float)(pad_top - min_y + line * pitch);
			blit_max(canvas, cw, ch, &tmp[i], origin_x, baseline_y);
			struct flametext_glyph *gl = &glyphs[glyph_count++];
			gl->x = (float)(origin_x + (int)tmp[i].pen_x +
					tmp[i].left);
			gl->y = baseline_y - (float)tmp[i].top;
			gl->w = (float)tmp[i].w;
			gl->h = (float)tmp[i].h;
		}
		bfree(tmp[i].gray);
	}
	bfree(tmp);
	bfree(line_w);

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
		bfree(glyphs);
		return NULL;
	}

	struct flametext_mask *m = bzalloc(sizeof(*m));
	m->tex = tex;
	m->width = (uint32_t)cw;
	m->height = (uint32_t)ch;
	m->text_left = (float)pad_left;
	m->text_right = (float)(pad_left + text_w);
	m->text_top = (float)pad_top;
	m->text_bottom = (float)(pad_top + text_h);
	m->bottom_y = bottom_y;
	m->glyphs = glyphs;
	m->glyph_count = glyph_count;
	return m;
}

void flametext_mask_free(struct flametext_mask *mask)
{
	if (!mask)
		return;
	if (mask->tex)
		gs_texture_destroy(mask->tex);
	bfree(mask->bottom_y);
	bfree(mask->glyphs);
	bfree(mask);
}
