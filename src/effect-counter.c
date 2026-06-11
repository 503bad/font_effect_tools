#include "effect-counter.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-outline.h"
#include "flametext-glow.h"
#include "flametext-font-resolve.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_CTR_FONT 0xFFFFFFFFu      /* white #FFFFFF */
#define DEFAULT_OUTLINE_COLOR 0xFF000000u /* opaque black  */
#define DEFAULT_SHADOW_COLOR 0x80000000u  /* 50% black     */

#define CTR_CHAIN_MAX 20 /* intermediate characters per letter      */
#define CTR_CELL_GAP 40  /* empty px around each atlas cell so the
			  * outline/glow sampling discs of one cell
			  * never reach a neighbour                  */
#define CTR_TEX_MAX 8192 /* conservative atlas dimension cap        */

/* One rasterized character in the spin atlas, with the FreeType bearings
 * needed to sit it on the final glyph's baseline. */
struct ctr_cell {
	int w, h;     /* bitmap size in px                  */
	int left, top; /* bitmap_left / bitmap_top bearings */
	int ax, ay;   /* bitmap position inside the atlas   */
};

/* Spin chain for one visible glyph of the text: up to CTR_CHAIN_MAX
 * intermediates in ascending codepoint order, then the final character. */
struct ctr_glyph {
	int count; /* cells in the chain incl. the final char (0 = none) */
	struct ctr_cell cells[CTR_CHAIN_MAX + 1];
};

struct counter_state {
	gs_effect_t *fill;
	gs_effect_t *outline;
	gs_effect_t *glow;

	uint32_t font_color;
	float    duration; /* seconds until the whole text locks in */
	float    glow_amt;

	bool     outline_on;
	float    outline_width;
	uint32_t outline_color;

	bool     shadow_on;
	uint32_t shadow_color;
	int      shadow_x;
	int      shadow_y;

	bool  loop;
	float wait;

	/* Requested vs. built text/font snapshots. update() (no graphics lock,
	 * possibly another thread) only writes the fixed-size request buffers;
	 * render() compares them against what the atlas was built from and
	 * rebuilds under the graphics lock when they differ — the same
	 * lock-free hand-off the image decoration effect uses for its path. */
	char     text[2048];
	char     face[256];
	uint32_t size;
	bool     bold;
	bool     italic;
	char     built_text[2048];
	char     built_face[256];
	uint32_t built_size;
	bool     built_bold;
	bool     built_italic;

	gs_texture_t *atlas;
	uint32_t atlas_w, atlas_h;
	struct ctr_glyph *glyphs; /* parallel to mask->glyphs */
	size_t glyph_count;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

/* Decode the next UTF-8 codepoint starting at *p, advancing *p. Returns 0 at
 * end of string, 0xFFFD on malformed input. (Same logic as flametext-text.) */
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

/* A rasterized candidate kept on the CPU until the atlas is assembled. */
struct ctr_bitmap {
	unsigned char *gray; /* w*h, owned */
	int w, h, left, top;
};

/* Rasterize one codepoint with the same synthesis as flametext-text.c
 * (embolden, sheared face for italics). Returns true only for a glyph with
 * visible pixels; `by_index` renders missing characters as the font's
 * .notdef box (like the host) instead of skipping them. */
static bool ctr_raster(FT_Face face, uint32_t pixel_size, bool bold,
		       uint32_t cp, bool require_mapped, struct ctr_bitmap *out)
{
	FT_UInt gi = FT_Get_Char_Index(face, cp);
	if (require_mapped && gi == 0)
		return false; /* tofu: the font has no glyph for it */
	if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0)
		return false;
	if (bold)
		FT_Outline_Embolden(&face->glyph->outline,
				    (FT_Pos)(pixel_size / 24 + 1) << 6);
	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
		return false;

	FT_Bitmap *bm = &face->glyph->bitmap;
	int w = (int)bm->width;
	int h = (int)bm->rows;
	if (w <= 0 || h <= 0)
		return false;

	out->gray = bmalloc((size_t)w * h);
	for (int y = 0; y < h; ++y)
		memcpy(out->gray + (size_t)y * w,
		       bm->buffer + (size_t)y * bm->pitch, w);
	out->w = w;
	out->h = h;
	out->left = face->glyph->bitmap_left;
	out->top = face->glyph->bitmap_top;
	return true;
}

static void ctr_free_atlas(struct counter_state *s)
{
	if (s->atlas) {
		gs_texture_destroy(s->atlas);
		s->atlas = NULL;
	}
	bfree(s->glyphs);
	s->glyphs = NULL;
	s->glyph_count = 0;
	s->atlas_w = s->atlas_h = 0;
}

/* Build the spin atlas: one row per visible glyph of the text, one column per
 * chain step. Runs under the graphics lock (called from render). */
static void ctr_build_atlas(struct counter_state *s)
{
	ctr_free_atlas(s);

	/* Build from the stable `built_*` copies, never the request buffers
	 * update() may be rewriting concurrently. */
	if (!s->built_text[0] || !s->built_face[0] || !s->built_size)
		return;

	char path[1024];
	if (!flametext_resolve_font(s->built_face, s->built_bold,
				    s->built_italic, path, sizeof(path)))
		return;

	FT_Library lib;
	if (FT_Init_FreeType(&lib) != 0)
		return;
	FT_Face face;
	if (FT_New_Face(lib, path, 0, &face) != 0) {
		FT_Done_FreeType(lib);
		return;
	}
	if (FT_Set_Pixel_Sizes(face, 0, s->built_size) != 0) {
		FT_Done_Face(face);
		FT_Done_FreeType(lib);
		return;
	}
	if (s->built_italic && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
		FT_Matrix shear = {0x10000, (FT_Fixed)(0.21 * 0x10000), 0,
				   0x10000};
		FT_Set_Transform(face, &shear, NULL);
	}

	/* Upper bound on visible glyphs = codepoints in the text. */
	size_t cap = 0;
	for (const char *q = s->built_text; *q;) {
		uint32_t cp = utf8_next(&q);
		if (cp == 0)
			break;
		if (cp != '\n' && cp != '\r')
			++cap;
	}
	if (cap == 0) {
		FT_Done_Face(face);
		FT_Done_FreeType(lib);
		return;
	}

	struct ctr_glyph *glyphs = bzalloc(sizeof(*glyphs) * cap);
	struct ctr_bitmap *bitmaps =
		bzalloc(sizeof(*bitmaps) * cap * (CTR_CHAIN_MAX + 1));
	size_t nvis = 0;
	int max_w = 1, max_h = 1;

	for (const char *q = s->built_text; *q;) {
		uint32_t cp = utf8_next(&q);
		if (cp == 0)
			break;
		if (cp == '\n' || cp == '\r')
			continue;

		/* The final character decides whether this is a visible glyph
		 * (mirrors which characters made it into mask->glyphs). */
		struct ctr_bitmap fin;
		if (!ctr_raster(face, s->built_size, s->built_bold, cp, false,
				&fin))
			continue;

		struct ctr_glyph *g = &glyphs[nvis];
		struct ctr_bitmap *row =
			&bitmaps[nvis * (CTR_CHAIN_MAX + 1)];

		/* Walk down from the final codepoint collecting up to
		 * CTR_CHAIN_MAX drawable predecessors (newest first). */
		struct ctr_bitmap rev[CTR_CHAIN_MAX];
		int got = 0;
		int examined = 0;
		for (uint32_t cand = cp; cand > 0x21 && got < CTR_CHAIN_MAX &&
					 examined < 300;
		     ++examined) {
			--cand;
			struct ctr_bitmap bm;
			if (ctr_raster(face, s->built_size, s->built_bold, cand,
				       true, &bm))
				rev[got++] = bm;
		}

		/* Store ascending (furthest codepoint first), final last, so
		 * the spin counts up into the real character. */
		for (int k = 0; k < got; ++k)
			row[k] = rev[got - 1 - k];
		row[got] = fin;
		g->count = got + 1;

		for (int k = 0; k < g->count; ++k) {
			if (row[k].w > max_w)
				max_w = row[k].w;
			if (row[k].h > max_h)
				max_h = row[k].h;
		}
		++nvis;
	}

	FT_Done_Face(face);
	FT_Done_FreeType(lib);

	if (nvis == 0) {
		bfree(bitmaps);
		bfree(glyphs);
		return;
	}

	/* Slot layout with a guard gap so per-cell outline/glow sampling never
	 * bleeds into a neighbouring character. Oversized fonts degrade
	 * gracefully: fewer intermediates (columns) and, past the row cap,
	 * letters that simply lock in without spinning. */
	int slot_w = max_w + 2 * CTR_CELL_GAP;
	int slot_h = max_h + 2 * CTR_CELL_GAP;
	int max_cols = CTR_TEX_MAX / slot_w;
	int max_rows = CTR_TEX_MAX / slot_h;
	if (max_cols < 1 || max_rows < 1) {
		obs_log(LOG_WARNING,
			"counter: font too large for the spin atlas");
		bfree(bitmaps);
		bfree(glyphs);
		return;
	}

	int cols = 0;
	for (size_t i = 0; i < nvis; ++i) {
		struct ctr_glyph *g = &glyphs[i];
		struct ctr_bitmap *row = &bitmaps[i * (CTR_CHAIN_MAX + 1)];
		if ((int)i >= max_rows) {
			/* No atlas room: this letter locks in instantly. */
			for (int k = 0; k < g->count; ++k)
				bfree(row[k].gray);
			g->count = 0;
			continue;
		}
		if (g->count > max_cols) {
			/* Keep the cells nearest the final character. */
			int drop = g->count - max_cols;
			for (int k = 0; k < drop; ++k)
				bfree(row[k].gray);
			memmove(row, row + drop,
				sizeof(*row) * (size_t)max_cols);
			g->count = max_cols;
		}
		if (g->count > cols)
			cols = g->count;
	}
	if ((int)nvis > max_rows)
		obs_log(LOG_WARNING,
			"counter: only the first %d letters spin (atlas cap)",
			max_rows);

	int rows = (int)nvis < max_rows ? (int)nvis : max_rows;
	if (cols < 1) {
		bfree(bitmaps);
		bfree(glyphs);
		return;
	}

	uint32_t aw = (uint32_t)(cols * slot_w);
	uint32_t ah = (uint32_t)(rows * slot_h);
	unsigned char *buf = bzalloc((size_t)aw * ah);

	for (size_t i = 0; i < nvis; ++i) {
		struct ctr_glyph *g = &glyphs[i];
		struct ctr_bitmap *row = &bitmaps[i * (CTR_CHAIN_MAX + 1)];
		for (int k = 0; k < g->count; ++k) {
			struct ctr_bitmap *bm = &row[k];
			int ax = k * slot_w + CTR_CELL_GAP;
			int ay = (int)i * slot_h + CTR_CELL_GAP;
			for (int y = 0; y < bm->h; ++y)
				memcpy(buf + (size_t)(ay + y) * aw + ax,
				       bm->gray + (size_t)y * bm->w, bm->w);
			struct ctr_cell *c = &g->cells[k];
			c->w = bm->w;
			c->h = bm->h;
			c->left = bm->left;
			c->top = bm->top;
			c->ax = ax;
			c->ay = ay;
			bfree(bm->gray);
		}
	}
	bfree(bitmaps);

	const uint8_t *data_ptrs[1] = {(const uint8_t *)buf};
	gs_texture_t *tex = gs_texture_create(aw, ah, GS_R8, 1, data_ptrs, 0);
	bfree(buf);
	if (!tex) {
		obs_log(LOG_ERROR, "counter: spin atlas texture failed");
		bfree(glyphs);
		return;
	}

	s->atlas = tex;
	s->atlas_w = aw;
	s->atlas_h = ah;
	s->glyphs = glyphs;
	s->glyph_count = nvis;
}

static void *counter_create(void)
{
	return bzalloc(sizeof(struct counter_state));
}

static void counter_destroy(void *data)
{
	struct counter_state *s = data;
	if (!s)
		return;
	ctr_free_atlas(s);
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->outline)
		gs_effect_destroy(s->outline);
	if (s->glow)
		gs_effect_destroy(s->glow);
	bfree(s);
}

static void counter_load_graphics(void *data)
{
	struct counter_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "counter: failed to load textfill.effect");
	s->outline = fx_outline_load();
	s->glow = fx_glow_load();
}

static void counter_update(void *data, obs_data_t *settings)
{
	struct counter_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "ctr_font");
	s->duration = (float)obs_data_get_double(settings, "ctr_duration");
	s->glow_amt = (float)obs_data_get_double(settings, "ctr_glow");
	s->outline_on = obs_data_get_bool(settings, "ctr_outline");
	s->outline_width =
		(float)obs_data_get_double(settings, "ctr_outline_width");
	s->outline_color =
		(uint32_t)obs_data_get_int(settings, "ctr_outline_color");
	s->shadow_on = obs_data_get_bool(settings, "ctr_shadow");
	s->shadow_color =
		(uint32_t)obs_data_get_int(settings, "ctr_shadow_color");
	s->shadow_x = (int)obs_data_get_int(settings, "ctr_shadow_x");
	s->shadow_y = (int)obs_data_get_int(settings, "ctr_shadow_y");
	s->loop = obs_data_get_bool(settings, "ctr_loop");
	s->wait = (float)obs_data_get_double(settings, "ctr_wait");

	/* Snapshot the shared text/font into the fixed request buffers; render
	 * compares them against the built copies and rebuilds the atlas under
	 * the graphics lock when they differ. */
	const char *text = obs_data_get_string(settings, "text");
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	const char *face =
		font_obj ? obs_data_get_string(font_obj, "face") : "";
	long long flags = font_obj ? obs_data_get_int(font_obj, "flags") : 0;

	snprintf(s->text, sizeof(s->text), "%s", text ? text : "");
	snprintf(s->face, sizeof(s->face), "%s", face);
	s->size = font_obj ? (uint32_t)obs_data_get_int(font_obj, "size") : 0;
	s->bold = (flags & OBS_FONT_BOLD) != 0;
	s->italic = (flags & OBS_FONT_ITALIC) != 0;

	if (font_obj)
		obs_data_release(font_obj);
}

/* Draw one cell (glow/outline/fill or just the shadow silhouette) at the
 * top-left position (x, y), sampling from `tex` of size cw x ch. */
static void ctr_draw_cell(struct counter_state *s, gs_texture_t *tex,
			  uint32_t cw, uint32_t ch, float x, float y,
			  uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh,
			  const float fill[4], const float oline[4],
			  bool shadow_only)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	if (!shadow_only) {
		if (s->glow_amt > 0.001f && s->glow)
			fx_glow_render_sub(s->glow, tex, cw, ch, fill,
					   s->glow_amt, rx, ry, rw, rh);
		if (s->outline_on && s->outline)
			fx_outline_render_sub(s->outline, tex, cw, ch, oline,
					      s->outline_width, rx, ry, rw,
					      rh);
	}
	fx_textfill_render_sub(s->fill, tex, fill, rx, ry, rw, rh);
	gs_matrix_pop();
}

/* Resolve which cell glyph `i` shows at loop-local time `lt`, plus where it
 * sits. Returns false when the final mask glyph should be drawn instead. */
static bool ctr_pick_cell(const struct counter_state *s,
			  const struct flametext_glyph *g, int i, int n,
			  float lt, float dur, float *x, float *y,
			  uint32_t *rx, uint32_t *ry, uint32_t *rw,
			  uint32_t *rh)
{
	float settle = dur * (float)(i + 1) / (float)n;
	if (lt >= settle)
		return false;
	if (!s->atlas || (size_t)i >= s->glyph_count)
		return false;
	const struct ctr_glyph *cg = &s->glyphs[i];
	if (cg->count <= 1)
		return false;

	float p = lt / settle;
	int idx = (int)(p * (float)cg->count);
	if (idx >= cg->count)
		idx = cg->count - 1;
	const struct ctr_cell *c = &cg->cells[idx];
	const struct ctr_cell *fin = &cg->cells[cg->count - 1];

	/* Sit the spinning character on the final glyph's pen position and
	 * baseline so the swap to the real letter is seamless. */
	float pen_x = g->x - (float)fin->left;
	float base_y = g->y + (float)fin->top;
	*x = pen_x + (float)c->left;
	*y = base_y - (float)c->top;
	*rx = (uint32_t)c->ax;
	*ry = (uint32_t)c->ay;
	*rw = (uint32_t)c->w;
	*rh = (uint32_t)c->h;
	return true;
}

static void counter_render(void *data, const struct fx_render_ctx *ctx)
{
	struct counter_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill || mask->glyph_count == 0)
		return;

	/* Rebuild the spin atlas (graphics lock held here) when the requested
	 * text/font differs from what it was built from. The request buffers
	 * are copied into the stable `built_*` set first so a concurrent
	 * update() rewrite at worst triggers one more rebuild next frame. */
	if (strcmp(s->text, s->built_text) != 0 ||
	    strcmp(s->face, s->built_face) != 0 || s->size != s->built_size ||
	    s->bold != s->built_bold || s->italic != s->built_italic) {
		snprintf(s->built_text, sizeof(s->built_text), "%s", s->text);
		snprintf(s->built_face, sizeof(s->built_face), "%s", s->face);
		s->built_size = s->size;
		s->built_bold = s->bold;
		s->built_italic = s->italic;
		ctr_build_atlas(s);
	}

	int n = (int)mask->glyph_count;
	float dur = s->duration < 0.05f ? 0.05f : s->duration;
	float cycle = dur + s->wait;
	float lt = s->loop ? fmodf(ctx->time, cycle) : ctx->time;

	float rgba[4], orgba[4], shrgba[4];
	unpack_color(s->font_color, rgba);
	unpack_color(s->outline_color, orgba);
	unpack_color(s->shadow_color, shrgba);
	bool draw_shadow = s->shadow_on && shrgba[3] > 0.0f;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	for (int pass = draw_shadow ? 0 : 1; pass < 2; ++pass) {
		bool shadow_only = pass == 0;
		float ox = shadow_only ? (float)s->shadow_x : 0.0f;
		float oy = shadow_only ? (float)s->shadow_y : 0.0f;
		const float *col = shadow_only ? shrgba : rgba;

		for (int i = 0; i < n; ++i) {
			const struct flametext_glyph *g = &mask->glyphs[i];
			float x, y;
			uint32_t rx, ry, rw, rh;
			if (ctr_pick_cell(s, g, i, n, lt, dur, &x, &y, &rx,
					  &ry, &rw, &rh)) {
				ctr_draw_cell(s, s->atlas, s->atlas_w,
					      s->atlas_h, x + ox, y + oy, rx,
					      ry, rw, rh, col, orgba,
					      shadow_only);
			} else {
				ctr_draw_cell(s, mask->tex, ctx->width,
					      ctx->height, g->x + ox,
					      g->y + oy, (uint32_t)g->x,
					      (uint32_t)g->y, (uint32_t)g->w,
					      (uint32_t)g->h, col, orgba,
					      shadow_only);
			}
		}
	}

	gs_blend_state_pop();
}

static void counter_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "ctr_font",
		obs_module_text("CtrFontColor"));
	obs_properties_add_float_slider(p, "ctr_duration",
		obs_module_text("CtrDuration"), 0.2, 10.0, 0.1);
	obs_properties_add_float_slider(p, "ctr_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "ctr_outline", obs_module_text("OutlineShow"));
	obs_properties_add_float_slider(p, "ctr_outline_width",
		obs_module_text("OutlineWidth"), 1.0, 20.0, 0.5);
	obs_properties_add_color_alpha(p, "ctr_outline_color",
		obs_module_text("OutlineColor"));
	obs_properties_add_bool(p, "ctr_shadow", obs_module_text("DropShadow"));
	obs_properties_add_color_alpha(p, "ctr_shadow_color",
		obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(p, "ctr_shadow_x",
		obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(p, "ctr_shadow_y",
		obs_module_text("ShadowY"), -100, 100, 1);
	obs_properties_add_bool(p, "ctr_loop", obs_module_text("LoopOn"));
	obs_properties_add_float_slider(p, "ctr_wait",
		obs_module_text("LoopWait"), 0.0, 10.0, 0.1);
}

static void counter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "ctr_font", DEFAULT_CTR_FONT);
	obs_data_set_default_double(settings, "ctr_duration", 2.0);
	obs_data_set_default_double(settings, "ctr_glow", 0.0);
	obs_data_set_default_bool(settings, "ctr_outline", false);
	obs_data_set_default_double(settings, "ctr_outline_width", 4.0);
	obs_data_set_default_int(settings, "ctr_outline_color",
				 DEFAULT_OUTLINE_COLOR);
	obs_data_set_default_bool(settings, "ctr_shadow", false);
	obs_data_set_default_int(settings, "ctr_shadow_color",
				 DEFAULT_SHADOW_COLOR);
	obs_data_set_default_int(settings, "ctr_shadow_x", 4);
	obs_data_set_default_int(settings, "ctr_shadow_y", 4);
	obs_data_set_default_bool(settings, "ctr_loop", true);
	obs_data_set_default_double(settings, "ctr_wait", 2.0);
}

const struct text_effect fx_counter = {
	.id             = "counter",
	.name_key       = "EffectCounter",
	.create         = counter_create,
	.destroy        = counter_destroy,
	.load_graphics  = counter_load_graphics,
	.update         = counter_update,
	.render         = counter_render,
	.get_properties = counter_properties,
	.get_defaults   = counter_defaults,
};
