#include "effect-water.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DROP_CAPACITY       1024
#define DEFAULT_WATER_COLOR 0xFFFFE6BFu /* pale blue #BFE6FF in OBS 0xAABBGGRR */
#define DEFAULT_FONT_COLOR  0xFFFFFFFFu /* opaque white                       */

/* One drop. It forms at a glyph tip (origin_y), swells, then runs down to
 * target_y where it stops and fades over the lifetime. y grows downward; sizes
 * are canvas pixels. fade01 is < 0 while the drop is still running and 1->0
 * while it fades after arriving. */
struct waterdrop {
	float x;        /* column center                        */
	float origin_y; /* tip where the drop formed (trail top) */
	float head_y;   /* current head (bottom) y               */
	float target_y; /* where this drop stops dripping        */
	float vy;       /* downward velocity (px/s)              */
	float width;    /* drop half-width (px)                  */
	float swell;    /* 0..1 surface-tension build-up         */
	float age;      /* seconds since birth (for fade-in)     */
	float fade01;   /* <0 running; 1..0 fading after arrival */
	float seed;     /* 0..1 per-drop variation               */
};

/* A glyph lower tip: a place a drop is allowed to form. Extracted from the
 * mask's bottom contour so drops never originate from blank gaps. */
struct tip {
	float x, y;
};

struct water_state {
	gs_effect_t *effect;

	/* Settings. */
	float    amount;       /* max simultaneous drops             */
	float    frequency;    /* drops spawned per second           */
	float    distance;     /* base drip distance (px)            */
	float    distance_var; /* 0..1 random shortening of distance */
	float    size;         /* drop size scale (multiplier)       */
	float    size_var;     /* 0..1 random size spread            */
	float    lifetime;     /* fade-out time after arrival (s)    */
	uint32_t color;        /* water tint, OBS 0xAABBGGRR         */
	uint32_t font_color;   /* glyph fill color, OBS 0xAABBGGRR   */

	/* Drop pool: live drops occupy [0, live), swap-removed on death. */
	struct waterdrop *drops;
	size_t            capacity;
	size_t            live;

	/* Emission sites derived from the current mask. */
	struct tip *tips;
	size_t      tip_count;

	/* Geometry-derived scales (refreshed in set_mask). */
	float text_h;  /* text band height (px)            */
	float drop_w;  /* base drop half-width (px)        */
	float gravity; /* fall acceleration (px/s^2)       */

	float    emit_accum; /* fractional spawn carry      */
	uint32_t rng;        /* xorshift state              */
};

/* --- small local helpers (mirrors the spark system's deterministic PRNG) --- */
static inline uint32_t xs32(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x ? x : 0x9e3779b9u;
	return x;
}

static inline float frand(uint32_t *s)
{
	return (float)(xs32(s) & 0xFFFFFF) / (float)0x1000000; /* [0,1) */
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static void *water_create(void)
{
	struct water_state *s = bzalloc(sizeof(*s));
	s->drops = bzalloc(sizeof(struct waterdrop) * DROP_CAPACITY);
	s->capacity = DROP_CAPACITY;
	s->rng = 0x51ed270bu;
	s->amount = 16.0f;
	s->frequency = 1.5f;
	s->distance = 300.0f;
	s->distance_var = 0.4f;
	s->size = 1.0f;
	s->size_var = 0.4f;
	s->lifetime = 2.0f;
	s->color = DEFAULT_WATER_COLOR;
	s->font_color = DEFAULT_FONT_COLOR;
	return s;
}

static void water_destroy(void *data)
{
	struct water_state *s = data;
	if (!s)
		return;
	/* Graphics lock held by the host. */
	if (s->effect)
		gs_effect_destroy(s->effect);
	bfree(s->drops);
	bfree(s->tips);
	bfree(s);
}

static void water_load_graphics(void *data)
{
	struct water_state *s = data;
	char *path = obs_module_file("effects/waterdrop.effect");
	/* Graphics lock held by the host. */
	if (path) {
		s->effect = gs_effect_create_from_file(path, NULL);
		if (!s->effect)
			obs_log(LOG_ERROR, "failed to load waterdrop.effect (%s)",
				path);
	}
	bfree(path);
}

static void water_update(void *data, obs_data_t *settings)
{
	struct water_state *s = data;
	s->amount = (float)obs_data_get_double(settings, "water_amount");
	s->frequency = (float)obs_data_get_double(settings, "water_frequency");
	s->distance = (float)obs_data_get_double(settings, "water_distance");
	s->distance_var = (float)obs_data_get_double(settings, "water_distance_var");
	s->size = (float)obs_data_get_double(settings, "water_size");
	s->size_var = (float)obs_data_get_double(settings, "water_size_var");
	s->lifetime = (float)obs_data_get_double(settings, "water_lifetime");
	s->color = (uint32_t)obs_data_get_int(settings, "water_color");
	s->font_color = (uint32_t)obs_data_get_int(settings, "water_font_color");
}

/* Reserve enough room below the text for a drop to fall the full requested
 * distance (variance only ever shortens it), plus margin for the drop head. */
static uint32_t water_wanted_bottom_pad(void *data, uint32_t font_size)
{
	struct water_state *s = data;
	float margin = font_size * 0.6f * (s->size > 1.0f ? s->size : 1.0f);
	return (uint32_t)(s->distance + margin + 4.0f);
}

/* Find the glyph lower tips from the mask's per-column bottom contour: the
 * local low points (largest y) of each inked run, spaced apart so a flat edge
 * yields a few evenly-distributed drip sites rather than one per pixel. */
static void rebuild_tips(struct water_state *s, const struct flametext_mask *mask)
{
	bfree(s->tips);
	s->tips = NULL;
	s->tip_count = 0;
	if (!mask || !mask->bottom_y || mask->width == 0)
		return;

	const int *by = mask->bottom_y;
	const int w = (int)mask->width;

	s->text_h = mask->text_bottom - mask->text_top;
	if (s->text_h < 1.0f)
		s->text_h = 1.0f;
	s->drop_w = clampf(s->text_h * 0.035f * s->size, 1.0f, 80.0f);
	s->gravity = s->text_h * 3.0f;

	int win = (int)(s->text_h * 0.12f);
	if (win < 2)
		win = 2;
	float min_spacing = s->text_h * 0.18f; /* keep drip sites from clustering */
	if (min_spacing < 6.0f)
		min_spacing = 6.0f;

	/* Worst case: one tip per column. */
	s->tips = bzalloc(sizeof(struct tip) * (size_t)w);

	float last_x = -1e9f;
	for (int x = 0; x < w; ++x) {
		int y = by[x];
		if (y < 0)
			continue; /* blank column — never a drip site */

		/* Is this a local low point? No nearby inked column hangs
		 * lower (larger y) within the window. */
		bool is_low = true;
		for (int k = x - win; k <= x + win && is_low; ++k) {
			if (k < 0 || k >= w || k == x)
				continue;
			if (by[k] >= 0 && by[k] > y)
				is_low = false;
		}
		if (!is_low)
			continue;

		if ((float)x - last_x < min_spacing)
			continue; /* keep tips from clustering */

		s->tips[s->tip_count].x = (float)x + 0.5f;
		s->tips[s->tip_count].y = (float)y;
		++s->tip_count;
		last_x = (float)x;
	}
}

static void water_set_mask(void *data, const struct flametext_mask *mask)
{
	rebuild_tips(data, mask);
}

static void spawn_drop(struct water_state *s)
{
	if (s->live >= s->capacity || s->tip_count == 0)
		return;

	const struct tip *t = &s->tips[xs32(&s->rng) % s->tip_count];

	struct waterdrop *d = &s->drops[s->live++];
	d->x = t->x;
	d->origin_y = t->y;
	d->head_y = t->y;

	/* Each drop falls a randomly shortened fraction of the base distance. */
	float dist = s->distance * (1.0f - s->distance_var * frand(&s->rng));
	d->target_y = t->y + dist;

	/* Size spreads symmetrically around the base by +/- size_var. */
	float sf = 1.0f + s->size_var * (frand(&s->rng) * 2.0f - 1.0f);
	d->width = s->drop_w * clampf(sf, 0.1f, 2.0f);

	d->vy = 0.0f;
	d->swell = 0.0f;
	d->age = 0.0f;
	d->fade01 = -1.0f; /* still running */
	d->seed = frand(&s->rng);
}

static void water_tick(void *data, const struct fx_render_ctx *ctx, float dt)
{
	UNUSED_PARAMETER(ctx);
	struct water_state *s = data;
	if (dt <= 0.0f)
		return;
	if (dt > 0.1f)
		dt = 0.1f; /* clamp hitches so the sim stays stable */

	/* --- emission (respect the simultaneous-drop cap) --- */
	int cap = (int)(s->amount + 0.5f);
	if (s->tip_count > 0 && cap >= 1 && s->frequency > 0.0f) {
		s->emit_accum += s->frequency * dt;
		while (s->emit_accum >= 1.0f) {
			s->emit_accum -= 1.0f;
			if ((int)s->live >= cap) {
				s->emit_accum = 0.0f; /* don't bank up at the cap */
				break;
			}
			spawn_drop(s);
		}
	}

	float fade_life = s->lifetime > 0.01f ? s->lifetime : 0.01f;

	/* --- integrate + retire --- */
	for (size_t i = 0; i < s->live;) {
		struct waterdrop *d = &s->drops[i];
		d->age += dt;

		if (d->fade01 < 0.0f) {
			/* Surface tension: the drop hangs and swells at the tip,
			 * then releases and accelerates downward under gravity
			 * until it reaches its target depth. */
			float swell_time = 0.45f + 0.6f * d->seed;
			if (d->swell < 1.0f) {
				d->swell += dt / swell_time;
				if (d->swell > 1.0f)
					d->swell = 1.0f;
				d->head_y = d->origin_y +
					    d->width * (0.4f + 1.6f * d->swell);
			} else {
				d->vy += s->gravity * dt;
				d->head_y += d->vy * dt;
			}
			if (d->head_y >= d->target_y) {
				d->head_y = d->target_y;
				d->fade01 = 1.0f; /* arrived → start fading */
			}
		} else {
			/* Finished dripping: fade out over the lifetime. */
			d->fade01 -= dt / fade_life;
			if (d->fade01 <= 0.0f) {
				s->drops[i] = s->drops[--s->live];
				continue;
			}
		}
		++i;
	}
}

static void water_reset(void *data)
{
	struct water_state *s = data;
	s->live = 0;
	s->emit_accum = 0.0f;
}

static void draw_text_fill(struct water_state *s, const struct flametext_mask *mask)
{
	gs_eparam_t *p_image = gs_effect_get_param_by_name(s->effect, "image");
	gs_eparam_t *p_font = gs_effect_get_param_by_name(s->effect, "font_color");
	if (p_image)
		gs_effect_set_texture(p_image, mask->tex);
	if (p_font) {
		float rgba[4];
		unpack_color(s->font_color, rgba);
		struct vec4 col;
		vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
		gs_effect_set_vec4(p_font, &col);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(s->effect, "DrawText"))
		gs_draw_sprite(mask->tex, 0, 0, 0);
	gs_blend_state_pop();
}

static void draw_drops(struct water_state *s)
{
	float rgba[4];
	unpack_color(s->color, rgba);
	struct vec4 col;
	vec4_set(&col, rgba[0], rgba[1], rgba[2], rgba[3]);
	gs_eparam_t *p_color = gs_effect_get_param_by_name(s->effect,
							   "water_color");
	if (p_color)
		gs_effect_set_vec4(p_color, &col);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(s->effect, "Draw")) {
		gs_render_start(true);
		for (size_t i = 0; i < s->live; ++i) {
			const struct waterdrop *d = &s->drops[i];

			/* Fade in at birth; full while running; fade01 drives the
			 * fade-out once the drop has arrived. */
			float a = clampf(d->age / 0.25f, 0.0f, 1.0f);
			if (d->fade01 >= 0.0f)
				a *= d->fade01;
			if (a <= 0.001f)
				continue;

			/* TEXCOORD0 = corner uv, TEXCOORD1 = (alpha, kind). */
			#define WV(cx, cy, u, vv, al, kind)         \
				gs_texcoord(u, vv, 0);              \
				gs_texcoord(al, kind, 1);           \
				gs_vertex2f(cx, cy)

			/* --- trail (kind 0): origin_y down to head_y --- */
			float top = d->origin_y;
			float bot = d->head_y;
			if (bot - top > 1.0f) {
				float tw = d->width * 0.55f;
				float lx = d->x - tw, rx = d->x + tw;
				WV(lx, top, 0.0f, 0.0f, a, 0.0f);
				WV(rx, top, 1.0f, 0.0f, a, 0.0f);
				WV(lx, bot, 0.0f, 1.0f, a, 0.0f);

				WV(rx, top, 1.0f, 0.0f, a, 0.0f);
				WV(rx, bot, 1.0f, 1.0f, a, 0.0f);
				WV(lx, bot, 0.0f, 1.0f, a, 0.0f);
			}

			/* --- head (kind 1): rounded drop, stretched by speed --- */
			float stretch = 1.0f +
				clampf(d->vy / (s->text_h * 6.0f), 0.0f, 1.3f);
			float hx = d->width;
			float hy = d->width * stretch;
			float x0 = d->x - hx, x1 = d->x + hx;
			float y0 = d->head_y - hy, y1 = d->head_y + hy;
			WV(x0, y0, 0.0f, 0.0f, a, 1.0f);
			WV(x1, y0, 1.0f, 0.0f, a, 1.0f);
			WV(x0, y1, 0.0f, 1.0f, a, 1.0f);

			WV(x1, y0, 1.0f, 0.0f, a, 1.0f);
			WV(x1, y1, 1.0f, 1.0f, a, 1.0f);
			WV(x0, y1, 0.0f, 1.0f, a, 1.0f);
			#undef WV
		}
		gs_render_stop(GS_TRIS);
	}

	gs_blend_state_pop();
}

static void water_render(void *data, const struct fx_render_ctx *ctx)
{
	struct water_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!s->effect || !mask || !mask->tex)
		return;

	/* Letters first, then drops on top. */
	draw_text_fill(s, mask);
	if (s->live > 0)
		draw_drops(s);
}

static void water_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "water_font_color",
		obs_module_text("WaterFontColor"));
	obs_properties_add_color_alpha(p, "water_color",
		obs_module_text("WaterColor"));
	obs_properties_add_float_slider(p, "water_amount",
		obs_module_text("WaterAmount"), 1.0, 60.0, 1.0);
	obs_properties_add_float_slider(p, "water_frequency",
		obs_module_text("WaterFrequency"), 0.1, 10.0, 0.1);
	obs_properties_add_float_slider(p, "water_distance",
		obs_module_text("WaterDistance"), 0.0, 2000.0, 10.0);
	obs_properties_add_float_slider(p, "water_distance_var",
		obs_module_text("WaterDistanceVar"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "water_size",
		obs_module_text("WaterSize"), 0.2, 4.0, 0.05);
	obs_properties_add_float_slider(p, "water_size_var",
		obs_module_text("WaterSizeVar"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "water_lifetime",
		obs_module_text("WaterLifetime"), 0.1, 10.0, 0.1);
}

static void water_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "water_font_color", DEFAULT_FONT_COLOR);
	obs_data_set_default_int(settings, "water_color", DEFAULT_WATER_COLOR);
	obs_data_set_default_double(settings, "water_amount", 16.0);
	obs_data_set_default_double(settings, "water_frequency", 1.5);
	obs_data_set_default_double(settings, "water_distance", 300.0);
	obs_data_set_default_double(settings, "water_distance_var", 0.4);
	obs_data_set_default_double(settings, "water_size", 1.0);
	obs_data_set_default_double(settings, "water_size_var", 0.4);
	obs_data_set_default_double(settings, "water_lifetime", 2.0);
}

const struct text_effect fx_water = {
	.id                = "water",
	.name_key          = "EffectWater",
	.create            = water_create,
	.destroy           = water_destroy,
	.load_graphics     = water_load_graphics,
	.update            = water_update,
	.wanted_bottom_pad = water_wanted_bottom_pad,
	.set_mask          = water_set_mask,
	.tick              = water_tick,
	.render            = water_render,
	.reset             = water_reset,
	.get_properties    = water_properties,
	.get_defaults      = water_defaults,
};
