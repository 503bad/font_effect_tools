#include "effect-imagedeco.h"
#include "flametext-sprites.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

#include <plugin-support.h>

#define DEFAULT_IMG_FONT  0xFFFFFFFFu /* white #FFFFFF       */
#define DEFAULT_IMG_COLOR 0xFF8FA0FFu /* pink-ish #FFA08F     */
#define IMG_CAPACITY 1024

struct imagedeco_state {
	gs_effect_t *sprite;
	gs_effect_t *fill;
	struct fx_sprite_system *sys;

	uint32_t font_color;
	uint32_t color;
	int      shape;  /* fx_sprite_shape value */
	float    amount; /* 0..1 spawn rate       */
	float    size;
	float    spin;
	float    glow;
	float    bloom;

	char     path[1024];   /* requested image path  */
	char     loaded[1024]; /* currently loaded path */
	gs_image_file_t imgfile;
	bool     has_image;

	float    text_h;
	uint32_t cw, chh;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *imagedeco_create(void)
{
	struct imagedeco_state *s = bzalloc(sizeof(*s));
	s->sys = fx_sprites_create(IMG_CAPACITY);
	return s;
}

static void imagedeco_destroy(void *data)
{
	struct imagedeco_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->has_image)
		gs_image_file_free(&s->imgfile);
	if (s->sprite)
		gs_effect_destroy(s->sprite);
	if (s->fill)
		gs_effect_destroy(s->fill);
	fx_sprites_destroy(s->sys);
	bfree(s);
}

static void imagedeco_load_graphics(void *data)
{
	struct imagedeco_state *s = data;
	char *path = obs_module_file("effects/sprite.effect");
	if (path) {
		s->sprite = gs_effect_create_from_file(path, NULL);
		if (!s->sprite)
			obs_log(LOG_ERROR, "failed to load sprite.effect (%s)",
				path);
	}
	bfree(path);
	s->fill = fx_textfill_load();
}

static void imagedeco_update(void *data, obs_data_t *settings)
{
	struct imagedeco_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "img_font");
	s->color = (uint32_t)obs_data_get_int(settings, "img_color");
	s->shape = (int)obs_data_get_int(settings, "img_shape");
	s->amount = (float)obs_data_get_double(settings, "img_amount");
	s->size = (float)obs_data_get_double(settings, "img_size");
	s->spin = (float)obs_data_get_double(settings, "img_spin");
	s->glow = (float)obs_data_get_double(settings, "img_glow");
	s->bloom = (float)obs_data_get_double(settings, "img_bloom");

	const char *p = obs_data_get_string(settings, "img_path");
	if (!p)
		p = "";
	snprintf(s->path, sizeof(s->path), "%s", p);
}

static void imagedeco_reset(void *data)
{
	struct imagedeco_state *s = data;
	fx_sprites_reset(s->sys);
}

static void spawn_one(struct imagedeco_state *s, const struct flametext_mask *m)
{
	fx_sprite_t *q = fx_sprites_spawn(s->sys);
	if (!q)
		return;

	float pad = s->text_h * 0.5f;
	q->x = m->text_left - pad +
	       fx_sprites_frand(s->sys) * (m->text_right - m->text_left + pad * 2.0f);
	q->y = fx_sprites_frand(s->sys) * m->text_top; /* start above text */

	float fall = s->text_h * (0.5f + 0.5f * fx_sprites_frand(s->sys));
	q->vy = fall;
	q->vx = (fx_sprites_frand(s->sys) - 0.5f) * s->text_h * 0.4f;

	float travel = (float)s->chh + s->text_h;
	q->max_life = travel / (fall > 1.0f ? fall : 1.0f);
	q->life = q->max_life;

	q->size = s->text_h * 0.16f * s->size *
		  (0.7f + 0.6f * fx_sprites_frand(s->sys));
	q->seed = fx_sprites_frand(s->sys);
	q->rot = fx_sprites_frand(s->sys) * 6.2831853f;
	q->vrot = (fx_sprites_frand(s->sys) - 0.5f) * 4.0f * s->spin;

	if (s->shape == FX_SHAPE_IMAGE) {
		q->r = q->g = q->b = 1.0f;
		q->a = 1.0f;
	} else {
		float rgba[4];
		unpack_color(s->color, rgba);
		float v = 0.75f + 0.25f * fx_sprites_frand(s->sys);
		q->r = rgba[0] * v;
		q->g = rgba[1] * v;
		q->b = rgba[2] * v;
		q->a = rgba[3];
	}
}

static void imagedeco_tick(void *data, const struct fx_render_ctx *ctx,
			   float dt)
{
	struct imagedeco_state *s = data;
	const struct flametext_mask *m = ctx->mask;
	if (!m)
		return;
	if (dt > 0.1f)
		dt = 0.1f;

	s->text_h = m->text_bottom - m->text_top;
	if (s->text_h < 1.0f)
		s->text_h = 1.0f;
	s->cw = ctx->width;
	s->chh = ctx->height;

	s->sys->emit_accum += s->amount * 30.0f * dt;
	while (s->sys->emit_accum >= 1.0f) {
		s->sys->emit_accum -= 1.0f;
		spawn_one(s, m);
	}

	for (size_t i = 0; i < s->sys->live;) {
		fx_sprite_t *q = &s->sys->items[i];
		q->life -= dt;
		if (q->life <= 0.0f) {
			s->sys->items[i] = s->sys->items[--s->sys->live];
			continue;
		}
		q->vx += sinf(q->seed * 6.2831853f + ctx->time * 2.0f) *
			 s->text_h * 0.4f * dt; /* gentle sway */
		q->x += q->vx * dt;
		q->y += q->vy * dt;
		q->rot += q->vrot * dt;

		/* Fade in at the top, fade out near the end of the fall. */
		float t01 = 1.0f - q->life / q->max_life;
		float fin = t01 < 0.1f ? t01 / 0.1f : 1.0f;
		float fout = t01 > 0.8f ? (1.0f - t01) / 0.2f : 1.0f;
		float base = (s->shape == FX_SHAPE_IMAGE) ? 1.0f
			: (float)((s->color >> 24) & 0xFF) / 255.0f;
		q->a = base * fin * fout;
		++i;
	}
}

/* Reload the user image when the path changes (graphics lock held). */
static gs_texture_t *ensure_image(struct imagedeco_state *s)
{
	if (strcmp(s->path, s->loaded) != 0) {
		if (s->has_image) {
			gs_image_file_free(&s->imgfile);
			s->has_image = false;
		}
		if (s->path[0]) {
			memset(&s->imgfile, 0, sizeof(s->imgfile));
			gs_image_file_init(&s->imgfile, s->path);
			gs_image_file_init_texture(&s->imgfile);
			s->has_image = s->imgfile.texture != NULL;
		}
		snprintf(s->loaded, sizeof(s->loaded), "%s", s->path);
	}
	return s->has_image ? s->imgfile.texture : NULL;
}

static void imagedeco_render(void *data, const struct fx_render_ctx *ctx)
{
	struct imagedeco_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex)
		return;

	if (s->fill) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		fx_textfill_render(s->fill, mask->tex, rgba);
	}

	if (!s->sprite)
		return;

	int shape = s->shape;
	gs_texture_t *img = NULL;
	if (shape == FX_SHAPE_IMAGE) {
		img = ensure_image(s);
		if (!img)
			shape = FX_SHAPE_HEART; /* fallback until a file is set */
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	fx_sprites_render(s->sys, s->sprite, shape, s->glow, s->bloom, img);
	gs_blend_state_pop();
}

static void imagedeco_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "img_font",
		obs_module_text("ImgFontColor"));

	obs_property_t *sh = obs_properties_add_list(p, "img_shape",
		obs_module_text("ImgShape"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sh, obs_module_text("ImgShapeHeart"),
				  FX_SHAPE_HEART);
	obs_property_list_add_int(sh, obs_module_text("ImgShapeStar"),
				  FX_SHAPE_STAR);
	obs_property_list_add_int(sh, obs_module_text("ImgShapePetal"),
				  FX_SHAPE_PETAL);
	obs_property_list_add_int(sh, obs_module_text("ImgShapeCircle"),
				  FX_SHAPE_CIRCLE);
	obs_property_list_add_int(sh, obs_module_text("ImgShapeSoft"),
				  FX_SHAPE_SOFT);
	obs_property_list_add_int(sh, obs_module_text("ImgShapeImage"),
				  FX_SHAPE_IMAGE);

	obs_properties_add_color_alpha(p, "img_color",
		obs_module_text("ImgColor"));
	obs_properties_add_path(p, "img_path", obs_module_text("ImgPath"),
				OBS_PATH_FILE,
				"Image files (*.png *.jpg *.jpeg *.bmp *.gif);;All files (*.*)",
				NULL);
	obs_properties_add_float_slider(p, "img_amount",
		obs_module_text("ImgAmount"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "img_size",
		obs_module_text("ImgSize"), 0.2, 4.0, 0.05);
	obs_properties_add_float_slider(p, "img_spin",
		obs_module_text("ImgSpin"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "img_glow",
		obs_module_text("ImgGlow"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "img_bloom",
		obs_module_text("ImgBloom"), 0.0, 3.0, 0.05);
}

static void imagedeco_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "img_font", DEFAULT_IMG_FONT);
	obs_data_set_default_int(settings, "img_color", DEFAULT_IMG_COLOR);
	obs_data_set_default_int(settings, "img_shape", FX_SHAPE_HEART);
	obs_data_set_default_double(settings, "img_amount", 0.4);
	obs_data_set_default_double(settings, "img_size", 1.0);
	obs_data_set_default_double(settings, "img_spin", 1.0);
	obs_data_set_default_double(settings, "img_glow", 0.3);
	obs_data_set_default_double(settings, "img_bloom", 0.5);
}

const struct text_effect fx_imagedeco = {
	.id             = "imagedeco",
	.name_key       = "EffectImageDeco",
	.create         = imagedeco_create,
	.destroy        = imagedeco_destroy,
	.load_graphics  = imagedeco_load_graphics,
	.update         = imagedeco_update,
	.tick           = imagedeco_tick,
	.render         = imagedeco_render,
	.reset          = imagedeco_reset,
	.get_properties = imagedeco_properties,
	.get_defaults   = imagedeco_defaults,
};
