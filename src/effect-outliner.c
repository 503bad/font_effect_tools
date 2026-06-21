#include "effect-outliner.h"
#include "flametext-sprites.h"
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

#define DEFAULT_OTL_COLOR 0xFFFFFFFFu /* white #FFFFFF */
#define OTL_CAPACITY 8000
#define OTL_TWO_PI 6.2831853f

struct otl_pt {
	float x, y;
};

struct otl_seg {
	float x0, y0, x1, y1;
	float len;
	float cum; /* cumulative length at the segment start */
};

struct outliner_state {
	gs_effect_t *sprite;
	struct fx_sprite_system *sys;

	uint32_t color;
	int      dir;    /* 0 = forward (CW), 1 = reverse (CCW) */
	float    speed;
	float    width;    /* line width in px                       */
	float    tail_pct; /* tail length as a % of the outline path */
	float    life;     /* tail lifetime in seconds               */
	float    glow;
	float    bloom;

	/* Text/font snapshot the path was built from. */
	char     text[2048];
	char     face[256];
	uint32_t size;
	bool     bold;
	bool     italic;

	/* Path geometry signature so a host mask rebuild re-extracts the path. */
	size_t   sig_gc;
	float    sig_g0x, sig_g0y, sig_glx, sig_gly;
	char     sig_text[2048];
	char     sig_face[256];
	uint32_t sig_size;
	bool     sig_bold;
	bool     sig_italic;

	struct otl_seg *segs;
	size_t   seg_count;
	float    total_len;

	float    head_total; /* monotonic distance the pen has travelled */
	float    emit_total; /* last distance a tail sprite was emitted   */
};

/* ----- outline flattening (FreeType decompose) -------------------------- */

struct dec_ctx {
	struct otl_pt *pts;
	uint8_t       *brk; /* 1 = starts a new contour (no segment in) */
	size_t         n, cap;
	float          ox, oy; /* canvas transform for the current glyph */
	struct otl_pt  last;
};

static void dec_push(struct dec_ctx *c, float x, float y, uint8_t brk)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 512;
		c->pts = brealloc(c->pts, c->cap * sizeof(*c->pts));
		c->brk = brealloc(c->brk, c->cap);
	}
	c->pts[c->n].x = x;
	c->pts[c->n].y = y;
	c->brk[c->n] = brk;
	c->n++;
}

static struct otl_pt dec_xform(struct dec_ctx *c, const FT_Vector *v)
{
	struct otl_pt p = {c->ox + (float)v->x / 64.0f,
			   c->oy - (float)v->y / 64.0f};
	return p;
}

static int dec_move_to(const FT_Vector *to, void *user)
{
	struct dec_ctx *c = user;
	struct otl_pt p = dec_xform(c, to);
	dec_push(c, p.x, p.y, 1);
	c->last = p;
	return 0;
}

static int dec_line_to(const FT_Vector *to, void *user)
{
	struct dec_ctx *c = user;
	struct otl_pt p = dec_xform(c, to);
	dec_push(c, p.x, p.y, 0);
	c->last = p;
	return 0;
}

static int dec_conic_to(const FT_Vector *ctrl, const FT_Vector *to, void *user)
{
	struct dec_ctx *c = user;
	struct otl_pt s = c->last;
	struct otl_pt k = dec_xform(c, ctrl);
	struct otl_pt e = dec_xform(c, to);
	const int N = 6;
	for (int i = 1; i <= N; ++i) {
		float t = (float)i / (float)N;
		float a = 1.0f - t;
		float x = a * a * s.x + 2.0f * a * t * k.x + t * t * e.x;
		float y = a * a * s.y + 2.0f * a * t * k.y + t * t * e.y;
		dec_push(c, x, y, 0);
	}
	c->last = e;
	return 0;
}

static int dec_cubic_to(const FT_Vector *c1, const FT_Vector *c2,
			const FT_Vector *to, void *user)
{
	struct dec_ctx *c = user;
	struct otl_pt s = c->last;
	struct otl_pt a1 = dec_xform(c, c1);
	struct otl_pt a2 = dec_xform(c, c2);
	struct otl_pt e = dec_xform(c, to);
	const int N = 8;
	for (int i = 1; i <= N; ++i) {
		float t = (float)i / (float)N;
		float u = 1.0f - t;
		float b0 = u * u * u;
		float b1 = 3.0f * u * u * t;
		float b2 = 3.0f * u * t * t;
		float b3 = t * t * t;
		float x = b0 * s.x + b1 * a1.x + b2 * a2.x + b3 * e.x;
		float y = b0 * s.y + b1 * a1.y + b2 * a2.y + b3 * e.y;
		dec_push(c, x, y, 0);
	}
	c->last = e;
	return 0;
}

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

/* Decode the next UTF-8 codepoint (same logic as flametext-text). */
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

static void otl_free_path(struct outliner_state *s)
{
	bfree(s->segs);
	s->segs = NULL;
	s->seg_count = 0;
	s->total_len = 0.0f;
}

/* Extract and flatten the glyph contours of the current text/font into a single
 * arc-length-parameterized polyline aligned to the host mask glyphs. CPU only
 * (FreeType, no GPU calls), so it is safe to run outside the graphics lock. */
static void otl_build_path(struct outliner_state *s,
			   const struct flametext_mask *mask)
{
	otl_free_path(s);
	if (!mask || mask->glyph_count == 0 || !s->face[0] || !s->size)
		return;

	char path[1024];
	if (!flametext_resolve_font(s->face, s->bold, s->italic, path,
				    sizeof(path)))
		return;

	FT_Library lib;
	if (FT_Init_FreeType(&lib) != 0)
		return;
	FT_Face face;
	if (FT_New_Face(lib, path, 0, &face) != 0) {
		FT_Done_FreeType(lib);
		return;
	}
	if (FT_Set_Pixel_Sizes(face, 0, s->size) != 0) {
		FT_Done_Face(face);
		FT_Done_FreeType(lib);
		return;
	}
	if (s->italic && !(face->style_flags & FT_STYLE_FLAG_ITALIC)) {
		FT_Matrix shear = {0x10000, (FT_Fixed)(0.21 * 0x10000), 0,
				   0x10000};
		FT_Set_Transform(face, &shear, NULL);
	}

	FT_Outline_Funcs funcs;
	memset(&funcs, 0, sizeof(funcs));
	funcs.move_to = dec_move_to;
	funcs.line_to = dec_line_to;
	funcs.conic_to = dec_conic_to;
	funcs.cubic_to = dec_cubic_to;

	struct dec_ctx dc;
	memset(&dc, 0, sizeof(dc));

	size_t gi_index = 0;
	for (const char *q = s->text; *q && gi_index < mask->glyph_count;) {
		uint32_t cp = utf8_next(&q);
		if (cp == 0)
			break;
		if (cp == '\n' || cp == '\r')
			continue;

		FT_UInt gi = FT_Get_Char_Index(face, cp);
		if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0)
			continue;
		if (s->bold)
			FT_Outline_Embolden(&face->glyph->outline,
					    (FT_Pos)(s->size / 24 + 1) << 6);
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
			continue;
		FT_Bitmap *bm = &face->glyph->bitmap;
		if ((int)bm->width <= 0 || (int)bm->rows <= 0)
			continue; /* space / blank: not a mask glyph */

		const struct flametext_glyph *g = &mask->glyphs[gi_index++];
		dc.ox = g->x - (float)face->glyph->bitmap_left;
		dc.oy = g->y + (float)face->glyph->bitmap_top;
		dc.last.x = dc.ox;
		dc.last.y = dc.oy;
		FT_Outline_Decompose(&face->glyph->outline, &funcs, &dc);
	}

	FT_Done_Face(face);
	FT_Done_FreeType(lib);

	/* Turn the points into drawable segments (skip the "pen up" jumps). */
	if (dc.n >= 2) {
		struct otl_seg *segs = bmalloc(sizeof(*segs) * dc.n);
		size_t sc = 0;
		float cum = 0.0f;
		for (size_t k = 1; k < dc.n; ++k) {
			if (dc.brk[k])
				continue; /* contour break: no segment */
			float x0 = dc.pts[k - 1].x, y0 = dc.pts[k - 1].y;
			float x1 = dc.pts[k].x, y1 = dc.pts[k].y;
			float dx = x1 - x0, dy = y1 - y0;
			float len = sqrtf(dx * dx + dy * dy);
			if (len < 0.01f)
				continue;
			segs[sc].x0 = x0;
			segs[sc].y0 = y0;
			segs[sc].x1 = x1;
			segs[sc].y1 = y1;
			segs[sc].len = len;
			segs[sc].cum = cum;
			cum += len;
			++sc;
		}
		s->segs = segs;
		s->seg_count = sc;
		s->total_len = cum;
	}

	bfree(dc.pts);
	bfree(dc.brk);
}

/* Position at arc-length distance d (clamped into [0,total_len)). */
static struct otl_pt otl_pos_at(const struct outliner_state *s, float d)
{
	struct otl_pt p = {0.0f, 0.0f};
	if (s->seg_count == 0 || s->total_len <= 0.0f)
		return p;
	if (d < 0.0f)
		d = 0.0f;
	if (d >= s->total_len)
		d = s->total_len - 0.001f;
	size_t lo = 0, hi = s->seg_count - 1;
	while (lo < hi) {
		size_t mid = (lo + hi + 1) / 2;
		if (s->segs[mid].cum <= d)
			lo = mid;
		else
			hi = mid - 1;
	}
	const struct otl_seg *sg = &s->segs[lo];
	float t = sg->len > 0.0f ? (d - sg->cum) / sg->len : 0.0f;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	p.x = sg->x0 + (sg->x1 - sg->x0) * t;
	p.y = sg->y0 + (sg->y1 - sg->y0) * t;
	return p;
}

static void otl_ensure_path(struct outliner_state *s,
			    const struct flametext_mask *mask)
{
	if (!mask || mask->glyph_count == 0)
		return;
	size_t gc = mask->glyph_count;
	float g0x = mask->glyphs[0].x, g0y = mask->glyphs[0].y;
	float glx = mask->glyphs[gc - 1].x, gly = mask->glyphs[gc - 1].y;

	bool same = s->segs && s->sig_gc == gc && s->sig_g0x == g0x &&
		    s->sig_g0y == g0y && s->sig_glx == glx &&
		    s->sig_gly == gly && s->sig_size == s->size &&
		    s->sig_bold == s->bold && s->sig_italic == s->italic &&
		    strcmp(s->sig_text, s->text) == 0 &&
		    strcmp(s->sig_face, s->face) == 0;
	if (same)
		return;

	otl_build_path(s, mask);

	s->sig_gc = gc;
	s->sig_g0x = g0x;
	s->sig_g0y = g0y;
	s->sig_glx = glx;
	s->sig_gly = gly;
	s->sig_size = s->size;
	s->sig_bold = s->bold;
	s->sig_italic = s->italic;
	snprintf(s->sig_text, sizeof(s->sig_text), "%s", s->text);
	snprintf(s->sig_face, sizeof(s->sig_face), "%s", s->face);
}

static void *outliner_create(void)
{
	struct outliner_state *s = bzalloc(sizeof(*s));
	s->sys = fx_sprites_create(OTL_CAPACITY);
	return s;
}

static void outliner_destroy(void *data)
{
	struct outliner_state *s = data;
	if (!s)
		return;
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	fx_sprites_destroy(s->sys);
	otl_free_path(s);
	bfree(s);
}

static void outliner_load_graphics(void *data)
{
	struct outliner_state *s = data;
	char *path = obs_module_file("effects/sprite.effect");
	if (path) {
		s->sprite = gs_effect_create_from_file(path, NULL);
		if (!s->sprite)
			obs_log(LOG_ERROR, "failed to load sprite.effect (%s)",
				path);
	}
	bfree(path);
}

static void outliner_update(void *data, obs_data_t *settings)
{
	struct outliner_state *s = data;
	s->color = (uint32_t)obs_data_get_int(settings, "otl_color");
	s->dir = (int)obs_data_get_int(settings, "otl_dir");
	s->speed = (float)obs_data_get_double(settings, "otl_speed");
	s->width = (float)obs_data_get_double(settings, "otl_width");
	s->tail_pct = (float)obs_data_get_double(settings, "otl_tail_pct");
	s->life = (float)obs_data_get_double(settings, "otl_life");
	s->glow = (float)obs_data_get_double(settings, "otl_glow");
	s->bloom = (float)obs_data_get_double(settings, "otl_bloom");

	const char *text = obs_data_get_string(settings, "text");
	snprintf(s->text, sizeof(s->text), "%s", text ? text : "");
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	const char *face =
		font_obj ? obs_data_get_string(font_obj, "face") : "";
	long long flags = font_obj ? obs_data_get_int(font_obj, "flags") : 0;
	snprintf(s->face, sizeof(s->face), "%s", face);
	s->size = font_obj ? (uint32_t)obs_data_get_int(font_obj, "size") : 0;
	s->bold = (flags & OBS_FONT_BOLD) != 0;
	s->italic = (flags & OBS_FONT_ITALIC) != 0;
	if (font_obj)
		obs_data_release(font_obj);
}

static void outliner_reset(void *data)
{
	struct outliner_state *s = data;
	fx_sprites_reset(s->sys);
	s->head_total = 0.0f;
	s->emit_total = 0.0f;
}

static void otl_margins(void *data, uint32_t font_size, struct fx_margins *out)
{
	struct outliner_state *s = data;
	UNUSED_PARAMETER(font_size);
	uint32_t m = (uint32_t)(s->width * 1.5f + s->bloom * 16.0f + 14.0f);
	out->left = out->right = out->top = out->bottom = m;
}

static void outliner_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	struct outliner_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask)
		return;
	if (dt > 0.1f)
		dt = 0.1f;

	otl_ensure_path(s, mask);
	if (s->total_len < 1.0f)
		return;

	float text_h = mask->text_bottom - mask->text_top;
	if (text_h < 1.0f)
		text_h = 1.0f;
	float head_speed = s->speed * text_h * 5.0f; /* px / second */

	float lifetime = s->life < 0.05f ? 0.05f : s->life;
	/* Tail length is a fraction of the whole outline path; lifetime also
	 * caps how far back the tail can reach (length = min of the two). */
	float tail = s->total_len * (s->tail_pct * 0.01f);
	float max_reach = lifetime * head_speed;
	if (tail > max_reach)
		tail = max_reach;
	if (tail < 1.0f)
		tail = 1.0f;
	float colA;
	{
		float c[4];
		unpack_color(s->color, c);
		colA = c[3];
	}

	s->head_total += head_speed * dt;

	/* Emit tail sprites along the newly travelled arc length. */
	float step = s->width * 0.4f;
	if (step < 2.0f)
		step = 2.0f;
	float rgba[4];
	unpack_color(s->color, rgba);
	while (s->emit_total + step <= s->head_total) {
		s->emit_total += step;
		float d = fmodf(s->emit_total, s->total_len);
		if (s->dir == 1)
			d = s->total_len - d;
		struct otl_pt pos = otl_pos_at(s, d);

		fx_sprite_t *q = fx_sprites_spawn(s->sys);
		if (!q)
			break; /* pool full: catch up next frame */
		q->x = pos.x;
		q->y = pos.y;
		q->vx = s->emit_total; /* emit marker for the tail fade */
		q->size = s->width * 0.5f;
		q->rot = fx_sprites_frand(s->sys) * OTL_TWO_PI;
		q->seed = fx_sprites_frand(s->sys);
		q->r = rgba[0];
		q->g = rgba[1];
		q->b = rgba[2];
		q->max_life = lifetime;
		q->life = lifetime;
		q->a = colA;
	}

	/* Fade the tail: brightest at the head, falling off over `tail` px and
	 * culled past its lifetime. */
	for (size_t i = 0; i < s->sys->live;) {
		fx_sprite_t *q = &s->sys->items[i];
		q->life -= dt;
		float dist_behind = s->head_total - q->vx;
		float al = 1.0f - dist_behind / tail;
		if (q->life <= 0.0f || al <= 0.0f) {
			s->sys->items[i] = s->sys->items[--s->sys->live];
			continue;
		}
		q->a = al * colA;
		++i;
	}
}

static void outliner_render(void *data, const struct fx_render_ctx *ctx)
{
	struct outliner_state *s = data;
	if (!s->sprite || !ctx->mask)
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE); /* additive glow */
	fx_sprites_render(s->sys, s->sprite, FX_SHAPE_SOFT, s->glow, s->bloom,
			  NULL);
	gs_blend_state_pop();
}

static void outliner_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "otl_color",
		obs_module_text("OtlColor"));
	obs_property_t *d = obs_properties_add_list(p, "otl_dir",
		obs_module_text("OtlDir"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(d, obs_module_text("OtlDirCW"), 0);
	obs_property_list_add_int(d, obs_module_text("OtlDirCCW"), 1);
	obs_properties_add_float_slider(p, "otl_speed",
		obs_module_text("OtlSpeed"), 0.1, 3.0, 0.05);
	obs_properties_add_float_slider(p, "otl_width",
		obs_module_text("OtlWidth"), 1.0, 30.0, 0.5);
	obs_properties_add_float_slider(p, "otl_tail_pct",
		obs_module_text("OtlTail"), 1.0, 100.0, 1.0);
	obs_properties_add_float_slider(p, "otl_life",
		obs_module_text("OtlLife"), 0.2, 10.0, 0.05);
	obs_properties_add_float_slider(p, "otl_glow",
		obs_module_text("OtlGlow"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "otl_bloom",
		obs_module_text("OtlBloom"), 0.0, 3.0, 0.05);
}

static void outliner_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "otl_color", DEFAULT_OTL_COLOR);
	obs_data_set_default_int(settings, "otl_dir", 0);
	obs_data_set_default_double(settings, "otl_speed", 1.0);
	obs_data_set_default_double(settings, "otl_width", 6.0);
	obs_data_set_default_double(settings, "otl_tail_pct", 25.0);
	obs_data_set_default_double(settings, "otl_life", 2.0);
	obs_data_set_default_double(settings, "otl_glow", 1.0);
	obs_data_set_default_double(settings, "otl_bloom", 1.0);
}

const struct text_effect fx_outliner = {
	.id             = "outliner",
	.name_key       = "EffectOutliner",
	.create         = outliner_create,
	.destroy        = outliner_destroy,
	.load_graphics  = outliner_load_graphics,
	.update         = outliner_update,
	.wanted_margins = otl_margins,
	.tick           = outliner_tick,
	.render         = outliner_render,
	.reset          = outliner_reset,
	.get_properties = outliner_properties,
	.get_defaults   = outliner_defaults,
};
