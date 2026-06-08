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

	obs_enter_graphics();
	if (s->mask) {
		flametext_mask_free(s->mask);
		s->mask = NULL;
	}
	if (s->text && s->text[0] && s->font_path[0]) {
		s->mask = flametext_mask_build(s->text, s->font_path,
					       s->font_size, s->bold, s->italic,
					       bottom_pad, mg.left, mg.right,
					       mg.top);
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
	if (fx->render) {
		struct fx_render_ctx ctx;
		fill_ctx(s, &ctx);
		fx->render(s->states[s->active], &ctx);
	}
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

	/* Shared text / font (used across every effect). */
	obs_properties_add_text(p, "text", obs_module_text("Text"),
				OBS_TEXT_MULTILINE);
	obs_properties_add_font(p, "font", obs_module_text("Font"));

	/* Effect selector. */
	obs_property_t *sel = obs_properties_add_list(p, "effect",
		obs_module_text("Effect"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);

	size_t n;
	const struct text_effect *const *fx = fx_registry(&n);
	for (size_t i = 0; i < n; ++i)
		obs_property_list_add_string(sel,
			obs_module_text(fx[i]->name_key), fx[i]->id);
	obs_property_set_modified_callback2(sel, on_effect_changed, NULL);

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
	obs_data_set_default_string(settings, "text", "test");

	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", "Impact");
	obs_data_set_default_int(font, "size", DEFAULT_FONT_SIZE);
	obs_data_set_default_int(font, "flags", 0);
	obs_data_set_default_string(font, "style", "Regular");
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);

	obs_data_set_default_string(settings, "effect", DEFAULT_EFFECT_ID);

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
