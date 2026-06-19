#include "flametext-source.h"
#include "flametext-text.h"
#include "flametext-font-resolve.h"
#include "effect-base.h"
#include "effect-registry.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <stdio.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_FONT_SIZE 120
#define DEFAULT_EFFECT_ID "flame"

static const char *flametext_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FlameTextSource");
}

static void replace_string(char **dst, const char *src)
{
	if (*dst)
		bfree(*dst);
	*dst = src ? bstrdup(src) : NULL;
}

/* Per-effect settings group key, e.g. "flame_group". Used both as the group's
 * settings key and to toggle its visibility from the effect selector. */
static void group_key(const struct text_effect *e, char *buf, size_t n)
{
	snprintf(buf, n, "%s_group", e->id);
}

static void rebuild_mask(struct flametext_source *s)
{
	/* Reserve extra room below the text when the active effect wants it
	 * (e.g. water drips need somewhere to fall). */
	const struct text_effect *active = fx_registry(NULL)[s->active];
	uint32_t bottom_pad = active->wanted_bottom_pad
		? active->wanted_bottom_pad(s->states[s->active], s->font_size)
		: 0u;

	/* Extra room on each side for effects that stream past the glyphs. */
	struct fx_margins mg = {0u, 0u, 0u, 0u};
	if (active->wanted_margins)
		active->wanted_margins(s->states[s->active], s->font_size, &mg);
	if (mg.bottom > bottom_pad)
		bottom_pad = mg.bottom;

	/* The outer glow reaches past whatever the effect draws, on every
	 * side, so its margin stacks on top of the effect's own. */
	uint32_t gm = fx_oglow_margin(&s->oglow);
	mg.left += gm;
	mg.right += gm;
	mg.top += gm;
	bottom_pad += gm;

	obs_enter_graphics();
	if (s->mask) {
		flametext_mask_free(s->mask);
		s->mask = NULL;
	}
	if (s->text && s->text[0] && s->font_path[0]) {
		s->mask = flametext_mask_build(s->text, s->font_path,
					       s->font_size, s->bold, s->italic,
					       s->line_spacing, s->letter_spacing,
					       s->align, bottom_pad,
					       mg.left, mg.right, mg.top,
					       s->writing_dir == 1);

		/* The warp deforms the whole composited frame and can push the
		 * text past the canvas edge. Its reach depends on the text-band
		 * geometry, which only the built mask reveals, so reserve the
		 * room in a second pass once the first build is measured. */
		struct fx_margins wm;
		fx_warp_margins(&s->warp, s->mask, &wm);
		if (s->mask && (wm.left || wm.right || wm.top || wm.bottom)) {
			uint32_t wbottom = bottom_pad + wm.bottom;
			flametext_mask_free(s->mask);
			s->mask = flametext_mask_build(
				s->text, s->font_path, s->font_size, s->bold,
				s->italic, s->line_spacing, s->letter_spacing,
				s->align, wbottom, mg.left + wm.left,
				mg.right + wm.right, mg.top + wm.top,
				s->writing_dir == 1);
		}
	}
	obs_leave_graphics();

	/* Let every effect refresh anything derived from the mask geometry. */
	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->set_mask)
			fx[i]->set_mask(s->states[i], s->mask);
	}

	if (s->text && s->text[0] && s->font_path[0] && !s->mask)
		obs_log(LOG_WARNING,
			"text mask build failed (text='%s' font='%s')", s->text,
			s->font_path);
}

static void flametext_update(void *data, obs_data_t *settings)
{
	struct flametext_source *s = data;

	/* --- shared text / font --- */
	replace_string(&s->text, obs_data_get_string(settings, "text"));

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	const char *face = font_obj ? obs_data_get_string(font_obj, "face") : "";
	uint32_t size = font_obj ? (uint32_t)obs_data_get_int(font_obj, "size")
				 : DEFAULT_FONT_SIZE;
	long long flags = font_obj ? obs_data_get_int(font_obj, "flags") : 0;
	bool bold = (flags & OBS_FONT_BOLD) != 0;
	bool italic = (flags & OBS_FONT_ITALIC) != 0;

	bool face_changed = (s->font_face == NULL && face[0]) ||
			    (s->font_face && strcmp(s->font_face, face) != 0);
	bool style_changed = (s->bold != bold) || (s->italic != italic);

	replace_string(&s->font_face, face);
	s->font_size = size ? size : DEFAULT_FONT_SIZE;
	s->bold = bold;
	s->italic = italic;

	if (face_changed || style_changed) {
		s->font_path[0] = 0;
		if (s->font_face && s->font_face[0])
			flametext_resolve_font(s->font_face, s->bold, s->italic,
					       s->font_path, sizeof(s->font_path));
	}

	if (font_obj)
		obs_data_release(font_obj);

	/* --- shared line spacing (auto vs explicit pixel pitch) --- */
	bool line_manual = obs_data_get_int(settings, "line_mode") == 1;
	int line_px = (int)obs_data_get_int(settings, "line_px");
	s->line_spacing = (line_manual && line_px > 0) ? line_px : 0;

	/* --- shared letter spacing (auto vs extra pixels per advance) --- */
	s->letter_manual = obs_data_get_int(settings, "letter_mode") == 1;
	s->letter_spacing = s->letter_manual
		? (int)obs_data_get_int(settings, "letter_px") : 0;

	/* --- shared writing direction and alignment --- */
	s->writing_dir = (int)obs_data_get_int(settings, "writing_dir");
	s->align = (int)obs_data_get_int(settings, "align");

	/* --- active effect selection --- */
	int idx = fx_registry_index(obs_data_get_string(settings, "effect"));
	s->active = idx >= 0 ? idx : 0;

	/* --- per-effect parameters (update all so inactive ones stay in sync) */
	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->update)
			fx[i]->update(s->states[i], settings);
	}

	/* --- shared outer glow / warp (before rebuild_mask: both add margins) */
	fx_oglow_update(&s->oglow, settings);
	fx_warp_update(&s->warp, settings);

	rebuild_mask(s);
}

static void *flametext_create(obs_data_t *settings, obs_source_t *source)
{
	struct flametext_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->clock = 0.0f;

	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	s->effect_count = n;
	s->states = bzalloc(sizeof(void *) * (n ? n : 1));
	for (size_t i = 0; i < n; ++i)
		s->states[i] = fx[i]->create ? fx[i]->create() : NULL;

	obs_enter_graphics();
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->load_graphics)
			fx[i]->load_graphics(s->states[i]);
	}
	fx_oglow_load(&s->oglow);
	fx_warp_load(&s->warp);
	obs_leave_graphics();

	flametext_update(s, settings);
	return s;
}

static void flametext_destroy(void *data)
{
	struct flametext_source *s = data;
	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);

	obs_enter_graphics();
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->destroy)
			fx[i]->destroy(s->states[i]);
	}
	if (s->mask)
		flametext_mask_free(s->mask);
	fx_oglow_free(&s->oglow);
	fx_warp_free(&s->warp);
	obs_leave_graphics();

	bfree(s->states);
	bfree(s->text);
	bfree(s->font_face);
	bfree(s);
}

static void fill_ctx(const struct flametext_source *s, struct fx_render_ctx *ctx)
{
	ctx->mask = s->mask;
	ctx->time = s->clock;
	ctx->width = (s->mask && s->mask->width) ? s->mask->width : 1u;
	ctx->height = (s->mask && s->mask->height) ? s->mask->height : 1u;
}

static void flametext_video_tick(void *data, float seconds)
{
	struct flametext_source *s = data;
	s->clock += seconds;

	const struct text_effect *fx = fx_registry(NULL)[s->active];
	if (fx->tick) {
		struct fx_render_ctx ctx;
		fill_ctx(s, &ctx);
		fx->tick(s->states[s->active], &ctx, seconds);
	}
}

static void flametext_show(void *data)
{
	struct flametext_source *s = data;
	s->clock = 0.0f;
	/* Reset every effect's transient state so a re-show starts clean
	 * regardless of which effect is active. */
	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		if (fx[i]->reset)
			fx[i]->reset(s->states[i]);
	}
}

static void flametext_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct flametext_source *s = data;
	if (!s->mask || !s->mask->tex)
		return;

	const struct text_effect *fx = fx_registry(NULL)[s->active];
	if (!fx->render)
		return;

	struct fx_render_ctx ctx;
	fill_ctx(s, &ctx);

	/* Geometric warp wraps the whole frame: capture the effect (and its
	 * outer glow) offscreen, then redraw it deformed. When neutral this
	 * is a no-op and rendering goes straight to the scene. */
	bool warping = fx_warp_begin(&s->warp, ctx.width, ctx.height);

	/* Outer glow: capture the effect offscreen, lay the halo on the scene
	 * first (behind everything it drew), then composite the frame on top. */
	if (fx_oglow_begin(&s->oglow, ctx.width, ctx.height)) {
		fx->render(s->states[s->active], &ctx);
		fx_oglow_end(&s->oglow, ctx.width, ctx.height);
	} else {
		fx->render(s->states[s->active], &ctx);
	}

	if (warping)
		fx_warp_end(&s->warp, ctx.width, ctx.height, s->mask);
}

static uint32_t flametext_get_width(void *data)
{
	const struct flametext_source *s = data;
	return (s->mask && s->mask->width) ? s->mask->width : 1u;
}

static uint32_t flametext_get_height(void *data)
{
	const struct flametext_source *s = data;
	return (s->mask && s->mask->height) ? s->mask->height : 1u;
}

/* Show the explicit pixel slider only when the line spacing mode is manual. */
static bool on_line_mode_changed(void *priv, obs_properties_t *props,
				 obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	obs_property_t *px = obs_properties_get(props, "line_px");
	if (px)
		obs_property_set_visible(px,
			obs_data_get_int(settings, "line_mode") == 1);
	return true;
}

/* Show the explicit pixel slider only when the letter spacing mode is manual. */
static bool on_letter_mode_changed(void *priv, obs_properties_t *props,
				   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	obs_property_t *px = obs_properties_get(props, "letter_px");
	if (px)
		obs_property_set_visible(px,
			obs_data_get_int(settings, "letter_mode") == 1);
	return true;
}

/* Alignment labels follow the writing direction: lines align left/center/
 * right when horizontal, columns align top/center/bottom when vertical. The
 * stored values (0/1/2) are shared between both modes. */
static void populate_align_list(obs_property_t *al, bool vertical)
{
	obs_property_list_clear(al);
	obs_property_list_add_int(al,
		obs_module_text(vertical ? "AlignTop" : "AlignLeft"),
		FLAMETEXT_ALIGN_LEFT);
	obs_property_list_add_int(al, obs_module_text("AlignCenter"),
				  FLAMETEXT_ALIGN_CENTER);
	obs_property_list_add_int(al,
		obs_module_text(vertical ? "AlignBottom" : "AlignRight"),
		FLAMETEXT_ALIGN_RIGHT);
}

/* Swap the alignment labels when the writing direction changes. */
static bool on_writing_dir_changed(void *priv, obs_properties_t *props,
				   obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	obs_property_t *al = obs_properties_get(props, "align");
	if (al)
		populate_align_list(al,
			obs_data_get_int(settings, "writing_dir") == 1);
	return true;
}

/* Show only the active effect's property group. */
static bool on_effect_changed(void *priv, obs_properties_t *props,
			      obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(prop);
	const char *id = obs_data_get_string(settings, "effect");

	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_property_t *g = obs_properties_get(props, key);
		if (g)
			obs_property_set_visible(g, strcmp(id, fx[i]->id) == 0);
	}
	return true; /* property visibility changed → refresh the UI */
}

static obs_properties_t *flametext_properties(void *data)
{
	struct flametext_source *s = data;
	obs_properties_t *p = obs_properties_create();

	/* Effect selector first so the chosen look leads the panel. */
	obs_property_t *sel = obs_properties_add_list(p, "effect",
		obs_module_text("Effect"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);

	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i)
		obs_property_list_add_string(sel,
			obs_module_text(fx[i]->name_key), fx[i]->id);
	obs_property_set_modified_callback2(sel, on_effect_changed, NULL);

	/* Shared text / font (used across every effect). */
	obs_properties_add_text(p, "text", obs_module_text("Text"),
				OBS_TEXT_MULTILINE);
	obs_properties_add_font(p, "font", obs_module_text("Font"));

	/* Shared line spacing: auto (font's natural height) or explicit pixels. */
	obs_property_t *lm = obs_properties_add_list(p, "line_mode",
		obs_module_text("LineSpacing"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lm, obs_module_text("LineSpacingAuto"), 0);
	obs_property_list_add_int(lm, obs_module_text("LineSpacingManual"), 1);
	obs_property_set_modified_callback2(lm, on_line_mode_changed, NULL);
	obs_property_t *lpx = obs_properties_add_int_slider(p, "line_px",
		obs_module_text("LineSpacingPx"), 1, 2000, 1);
	obs_property_set_visible(lpx,
		s ? (s->line_spacing > 0) : false);

	/* Shared letter spacing: auto (font's natural advances) or extra px. */
	obs_property_t *km = obs_properties_add_list(p, "letter_mode",
		obs_module_text("LetterSpacing"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(km, obs_module_text("LetterSpacingAuto"), 0);
	obs_property_list_add_int(km, obs_module_text("LetterSpacingManual"), 1);
	obs_property_set_modified_callback2(km, on_letter_mode_changed, NULL);
	obs_property_t *kpx = obs_properties_add_int_slider(p, "letter_px",
		obs_module_text("LetterSpacingPx"), -500, 1000, 1);
	obs_property_set_visible(kpx, s ? s->letter_manual : false);

	/* Shared writing direction (exclusive radio: horizontal/vertical). */
	obs_property_t *wd = obs_properties_add_list(p, "writing_dir",
		obs_module_text("WritingDir"), OBS_COMBO_TYPE_RADIO,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(wd, obs_module_text("WritingHorizontal"), 0);
	obs_property_list_add_int(wd, obs_module_text("WritingVertical"), 1);
	obs_property_set_modified_callback2(wd, on_writing_dir_changed, NULL);

	/* Shared alignment (exclusive radio; labels per writing direction). */
	obs_property_t *al = obs_properties_add_list(p, "align",
		obs_module_text("TextAlign"), OBS_COMBO_TYPE_RADIO,
		OBS_COMBO_FORMAT_INT);
	populate_align_list(al, s ? s->writing_dir == 1 : false);

	/* One group per effect; only the active one is shown. */
	for (size_t i = 0; i < n; ++i) {
		obs_properties_t *grp = obs_properties_create();
		if (fx[i]->get_properties)
			fx[i]->get_properties(grp);
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_properties_add_group(p, key,
			obs_module_text(fx[i]->name_key), OBS_GROUP_NORMAL, grp);
	}

	/* Shared geometric warp + outer glow, below the effect groups
	 * (both apply to whichever effect is active). */
	fx_warp_get_properties(p);
	fx_oglow_get_properties(p);

	/* Initial visibility based on the currently active effect. */
	int active = s ? s->active : 0;
	for (size_t i = 0; i < n; ++i) {
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_property_t *g = obs_properties_get(p, key);
		if (g)
			obs_property_set_visible(g, (int)i == active);
	}

	return p;
}

static void flametext_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", "Font Effect Tools");

	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", "Impact");
	obs_data_set_default_int(font, "size", DEFAULT_FONT_SIZE);
	obs_data_set_default_int(font, "flags", 0);
	obs_data_set_default_string(font, "style", "Regular");
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);

	obs_data_set_default_string(settings, "effect", DEFAULT_EFFECT_ID);

	obs_data_set_default_int(settings, "line_mode", 0); /* auto */
	obs_data_set_default_int(settings, "line_px", 150);
	obs_data_set_default_int(settings, "letter_mode", 0); /* auto */
	obs_data_set_default_int(settings, "letter_px", 0);
	obs_data_set_default_int(settings, "writing_dir", 0); /* horizontal */
	obs_data_set_default_int(settings, "align", FLAMETEXT_ALIGN_CENTER);

	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i) {
		/* Keep each effect's property group enabled by default. */
		char key[96];
		group_key(fx[i], key, sizeof(key));
		obs_data_set_default_bool(settings, key, true);
		if (fx[i]->get_defaults)
			fx[i]->get_defaults(settings);
	}

	fx_warp_get_defaults(settings);
	fx_oglow_get_defaults(settings);
}

static struct obs_source_info s_flametext_source_info = {
	.id             = "flame_text_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.icon_type      = OBS_ICON_TYPE_TEXT,
	.get_name       = flametext_get_name,
	.create         = flametext_create,
	.destroy        = flametext_destroy,
	.update         = flametext_update,
	.video_tick     = flametext_video_tick,
	.video_render   = flametext_video_render,
	.get_width      = flametext_get_width,
	.get_height     = flametext_get_height,
	.get_properties = flametext_properties,
	.get_defaults   = flametext_defaults,
	.show           = flametext_show,
};

void flametext_register_source(void)
{
	obs_register_source(&s_flametext_source_info);
}
