#include "effect-fontswitch.h"
#include "flametext-sprites.h" /* fx_textfill_* */
#include "flametext-text.h"
#include "flametext-font-resolve.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_FSW_FONT 0xFFFFFFFFu /* white #FFFFFF */
#define FSW_MAX_FONTS 8
#define DEFAULT_FONT_SIZE 120

struct fontswitch_state {
	gs_effect_t *fill;

	uint32_t font_color;
	float    interval; /* seconds per font           */
	int      order;    /* 0 = sequential, 1 = random */

	/* Requested vs. built snapshots (counter-style lock-free hand-off):
	 * update() (no graphics lock) only writes the request fields; render()
	 * compares them against what the masks were built from and rebuilds the
	 * masks under the graphics lock when they differ. */
	char     text[2048];
	char     face[FSW_MAX_FONTS][256];
	bool     fbold[FSW_MAX_FONTS];
	bool     fitalic[FSW_MAX_FONTS];
	int      nfaces;
	uint32_t size;
	int      line_spacing, letter_spacing, align;
	bool     vertical;

	char     built_text[2048];
	char     built_face[FSW_MAX_FONTS][256];
	bool     built_fbold[FSW_MAX_FONTS];
	bool     built_fitalic[FSW_MAX_FONTS];
	int      built_nfaces;
	uint32_t built_size;
	int      built_line_spacing, built_letter_spacing, built_align;
	bool     built_vertical;

	struct flametext_mask *masks[FSW_MAX_FONTS];
	int      mask_count;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static uint32_t hash_u32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static void slot_key(int i, char *buf, size_t n)
{
	snprintf(buf, n, "fsw_font%d", i + 1);
}

static void *fontswitch_create(void)
{
	return bzalloc(sizeof(struct fontswitch_state));
}

static void fsw_free_masks(struct fontswitch_state *s)
{
	for (int i = 0; i < s->mask_count; ++i) {
		if (s->masks[i])
			flametext_mask_free(s->masks[i]);
		s->masks[i] = NULL;
	}
	s->mask_count = 0;
}

static void fontswitch_destroy(void *data)
{
	struct fontswitch_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	fsw_free_masks(s);
	if (s->fill)
		gs_effect_destroy(s->fill);
	bfree(s);
}

static void fontswitch_load_graphics(void *data)
{
	struct fontswitch_state *s = data;
	s->fill = fx_textfill_load();
	if (!s->fill)
		obs_log(LOG_ERROR, "fontswitch: failed to load textfill.effect");
}

static void fontswitch_update(void *data, obs_data_t *settings)
{
	struct fontswitch_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "fsw_font");
	s->interval = (float)obs_data_get_double(settings, "fsw_interval");
	s->order = (int)obs_data_get_int(settings, "fsw_order");

	const char *text = obs_data_get_string(settings, "text");
	snprintf(s->text, sizeof(s->text), "%s", text ? text : "");

	/* Shared font size (the canvas is sized for it, so every switched font
	 * is rasterized at the same size for consistent alignment). */
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	s->size = font_obj ? (uint32_t)obs_data_get_int(font_obj, "size") : 0;
	if (font_obj)
		obs_data_release(font_obj);

	/* Collect the non-empty font slots from their font pickers. */
	s->nfaces = 0;
	for (int i = 0; i < FSW_MAX_FONTS; ++i) {
		char key[32];
		slot_key(i, key, sizeof(key));
		obs_data_t *fo = obs_data_get_obj(settings, key);
		const char *face = fo ? obs_data_get_string(fo, "face") : "";
		long long flags = fo ? obs_data_get_int(fo, "flags") : 0;
		if (face && face[0]) {
			snprintf(s->face[s->nfaces], sizeof(s->face[0]), "%s",
				 face);
			s->fbold[s->nfaces] = (flags & OBS_FONT_BOLD) != 0;
			s->fitalic[s->nfaces] = (flags & OBS_FONT_ITALIC) != 0;
			s->nfaces++;
		}
		if (fo)
			obs_data_release(fo);
	}

	/* Mirror the host's shared layout settings so each font is laid out the
	 * same way as the rest of the plugin. */
	bool line_manual = obs_data_get_int(settings, "line_mode") == 1;
	int line_px = (int)obs_data_get_int(settings, "line_px");
	s->line_spacing = (line_manual && line_px > 0) ? line_px : 0;
	bool letter_manual = obs_data_get_int(settings, "letter_mode") == 1;
	s->letter_spacing =
		letter_manual ? (int)obs_data_get_int(settings, "letter_px") : 0;
	s->align = (int)obs_data_get_int(settings, "align");
	s->vertical = obs_data_get_int(settings, "writing_dir") == 1;
}

/* Build one mask per listed font (graphics lock held; called from render). */
static void fsw_build_masks(struct fontswitch_state *s)
{
	fsw_free_masks(s);
	if (!s->built_text[0] || !s->built_size)
		return;

	for (int i = 0; i < s->built_nfaces && s->mask_count < FSW_MAX_FONTS;
	     ++i) {
		char path[1024];
		if (!flametext_resolve_font(s->built_face[i], s->built_fbold[i],
					    s->built_fitalic[i], path,
					    sizeof(path))) {
			obs_log(LOG_WARNING,
				"fontswitch: could not resolve font '%s'",
				s->built_face[i]);
			continue;
		}
		struct flametext_mask *m = flametext_mask_build(
			s->built_text, path, s->built_size, s->built_fbold[i],
			s->built_fitalic[i], s->built_line_spacing,
			s->built_letter_spacing, s->built_align, 0u, 0u, 0u, 0u,
			s->built_vertical);
		if (m)
			s->masks[s->mask_count++] = m;
		else
			obs_log(LOG_WARNING,
				"fontswitch: mask build failed for '%s'",
				s->built_face[i]);
	}
}

/* True when the requested text/fonts/layout differ from what was built. */
static bool fsw_request_changed(const struct fontswitch_state *s)
{
	if (strcmp(s->text, s->built_text) != 0 || s->size != s->built_size ||
	    s->nfaces != s->built_nfaces ||
	    s->line_spacing != s->built_line_spacing ||
	    s->letter_spacing != s->built_letter_spacing ||
	    s->align != s->built_align || s->vertical != s->built_vertical)
		return true;
	for (int i = 0; i < s->nfaces; ++i) {
		if (strcmp(s->face[i], s->built_face[i]) != 0 ||
		    s->fbold[i] != s->built_fbold[i] ||
		    s->fitalic[i] != s->built_fitalic[i])
			return true;
	}
	return false;
}

static void fsw_snapshot(struct fontswitch_state *s)
{
	snprintf(s->built_text, sizeof(s->built_text), "%s", s->text);
	s->built_size = s->size;
	s->built_nfaces = s->nfaces;
	s->built_line_spacing = s->line_spacing;
	s->built_letter_spacing = s->letter_spacing;
	s->built_align = s->align;
	s->built_vertical = s->vertical;
	for (int i = 0; i < s->nfaces; ++i) {
		snprintf(s->built_face[i], sizeof(s->built_face[0]), "%s",
			 s->face[i]);
		s->built_fbold[i] = s->fbold[i];
		s->built_fitalic[i] = s->fitalic[i];
	}
}

/* Pick which font is shown at the given time. */
static int fsw_pick(const struct fontswitch_state *s, float time)
{
	int n = s->mask_count;
	if (n <= 1)
		return 0;
	float iv = s->interval < 0.05f ? 0.05f : s->interval;
	uint32_t step = (uint32_t)floorf(time / iv);
	if (s->order == 0)
		return (int)(step % (uint32_t)n);
	int idx = (int)(hash_u32(step + 1u) % (uint32_t)n);
	int prev = (int)(hash_u32(step) % (uint32_t)n);
	if (idx == prev) /* avoid an invisible "no change" tick */
		idx = (idx + 1) % n;
	return idx;
}

static void fsw_draw_aligned(struct fontswitch_state *s,
			     const struct flametext_mask *m,
			     const struct flametext_mask *host,
			     const float rgba[4])
{
	/* Align the chosen font's text band centre with the host band centre so
	 * letters stay put as the typeface changes. */
	float mcx = (m->text_left + m->text_right) * 0.5f;
	float mcy = (m->text_top + m->text_bottom) * 0.5f;
	float hcx = (host->text_left + host->text_right) * 0.5f;
	float hcy = (host->text_top + host->text_bottom) * 0.5f;

	gs_matrix_push();
	gs_matrix_translate3f(hcx - mcx, hcy - mcy, 0.0f);
	fx_textfill_render(s->fill, m->tex, rgba);
	gs_matrix_pop();
}

static void fontswitch_render(void *data, const struct fx_render_ctx *ctx)
{
	struct fontswitch_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill)
		return;

	/* Rebuild the per-font masks (graphics lock held) on any change. */
	if (fsw_request_changed(s)) {
		fsw_snapshot(s);
		fsw_build_masks(s);
	}

	float rgba[4];
	unpack_color(s->font_color, rgba);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (s->mask_count == 0) {
		/* No usable fonts listed: fall back to the host's own text. */
		fx_textfill_render(s->fill, mask->tex, rgba);
	} else {
		int idx = fsw_pick(s, ctx->time);
		if (idx < 0 || idx >= s->mask_count)
			idx = 0;
		const struct flametext_mask *m = s->masks[idx];
		if (m && m->tex)
			fsw_draw_aligned(s, m, mask, rgba);
	}

	gs_blend_state_pop();
}

static void fontswitch_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "fsw_font",
		obs_module_text("FswFontColor"));
	for (int i = 0; i < FSW_MAX_FONTS; ++i) {
		char key[32];
		char label[64];
		slot_key(i, key, sizeof(key));
		snprintf(label, sizeof(label), "%s %d",
			 obs_module_text("FswFontSlot"), i + 1);
		obs_properties_add_font(p, key, label);
	}
	obs_property_t *o = obs_properties_add_list(p, "fsw_order",
		obs_module_text("FswOrder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(o, obs_module_text("FswOrderSeq"), 0);
	obs_property_list_add_int(o, obs_module_text("FswOrderRandom"), 1);
	obs_properties_add_float_slider(p, "fsw_interval",
		obs_module_text("FswInterval"), 0.1, 10.0, 0.1);
}

static void fsw_default_slot(obs_data_t *settings, int i, const char *face)
{
	char key[32];
	slot_key(i, key, sizeof(key));
	obs_data_t *f = obs_data_create();
	obs_data_set_default_string(f, "face", face);
	obs_data_set_default_int(f, "size", DEFAULT_FONT_SIZE);
	obs_data_set_default_int(f, "flags", 0);
	obs_data_set_default_string(f, "style", "Regular");
	obs_data_set_default_obj(settings, key, f);
	obs_data_release(f);
}

static void fontswitch_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "fsw_font", DEFAULT_FSW_FONT);
	obs_data_set_default_int(settings, "fsw_order", 0);
	obs_data_set_default_double(settings, "fsw_interval", 1.0);
	/* A few starter fonts; the rest of the slots are left empty. */
	fsw_default_slot(settings, 0, "Impact");
	fsw_default_slot(settings, 1, "Arial");
	fsw_default_slot(settings, 2, "Georgia");
}

const struct text_effect fx_fontswitch = {
	.id             = "fontswitch",
	.name_key       = "EffectFontSwitch",
	.create         = fontswitch_create,
	.destroy        = fontswitch_destroy,
	.load_graphics  = fontswitch_load_graphics,
	.update         = fontswitch_update,
	.render         = fontswitch_render,
	.get_properties = fontswitch_properties,
	.get_defaults   = fontswitch_defaults,
};
