#include "effect-puzzle.h"
#include "flametext-sprites.h" /* fx_textfill_* (glow fallback colours) */
#include "flametext-glow.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

#define DEFAULT_PZ_FONT 0xFFFFFFFFu /* white #FFFFFF */

struct puzzle_state {
	gs_effect_t *fill; /* puzzle.effect: per-vertex coloured mask fill */
	gs_effect_t *glow;

	uint32_t font_color;
	int      grain;  /* number of columns the text is cut into */
	float    speed;
	int      order;  /* 0 = from left, 1 = from right, 2 = all  */
	float    glow_amt;
	float    bloom;
	bool     loop;
	float    wait;   /* seconds held assembled before disassembly */
};

struct pz_cellinfo {
	int   x0, y0, cw, ch;
	float dx, dy, a;
};

static void unpack_color(uint32_t c, float rgba[4])
{
	rgba[0] = (float)((c >> 0) & 0xFF) / 255.0f;
	rgba[1] = (float)((c >> 8) & 0xFF) / 255.0f;
	rgba[2] = (float)((c >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((c >> 24) & 0xFF) / 255.0f;
}

static uint32_t pack_rgba(const float c[4])
{
	uint32_t R = (uint32_t)(c[0] < 0 ? 0 : (c[0] > 1 ? 255 : c[0] * 255.0f + 0.5f));
	uint32_t G = (uint32_t)(c[1] < 0 ? 0 : (c[1] > 1 ? 255 : c[1] * 255.0f + 0.5f));
	uint32_t B = (uint32_t)(c[2] < 0 ? 0 : (c[2] > 1 ? 255 : c[2] * 255.0f + 0.5f));
	uint32_t A = (uint32_t)(c[3] < 0 ? 0 : (c[3] > 1 ? 255 : c[3] * 255.0f + 0.5f));
	return (A << 24) | (B << 16) | (G << 8) | R; /* OBS 0xAABBGGRR */
}

/* Cheap deterministic hash → [0,1). */
static float hash01(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return (float)(x & 0xFFFFFF) / (float)0x1000000;
}

static void *puzzle_create(void)
{
	return bzalloc(sizeof(struct puzzle_state));
}

static void puzzle_destroy(void *data)
{
	struct puzzle_state *s = data;
	if (!s)
		return;
	if (s->fill)
		gs_effect_destroy(s->fill);
	if (s->glow)
		gs_effect_destroy(s->glow);
	bfree(s);
}

static void puzzle_load_graphics(void *data)
{
	struct puzzle_state *s = data;
	char *path = obs_module_file("effects/puzzle.effect");
	if (path) {
		s->fill = gs_effect_create_from_file(path, NULL);
		if (!s->fill)
			obs_log(LOG_ERROR, "failed to load puzzle.effect (%s)",
				path);
	}
	bfree(path);
	s->glow = fx_glow_load();
}

static void puzzle_update(void *data, obs_data_t *settings)
{
	struct puzzle_state *s = data;
	s->font_color = (uint32_t)obs_data_get_int(settings, "pz_font");
	s->grain = (int)obs_data_get_int(settings, "pz_grain");
	s->speed = (float)obs_data_get_double(settings, "pz_speed");
	s->order = (int)obs_data_get_int(settings, "pz_order");
	s->glow_amt = (float)obs_data_get_double(settings, "pz_glow");
	s->bloom = (float)obs_data_get_double(settings, "pz_bloom");
	s->loop = obs_data_get_bool(settings, "pz_loop");
	s->wait = (float)obs_data_get_double(settings, "pz_wait");
}

/* Resolve one grid cell's rect, slide offset and fade at loop-local time `lt`.
 * Pieces slide in (enter), hold assembled, then slide back out (exit) when
 * looping. Returns false when the cell is degenerate or currently invisible. */
static bool pz_cell(const struct puzzle_state *s, float lt, float tl, float tt,
		    float cellW, float cellH, int cols, int r, int c,
		    float enter_dur, float stag, float hold_end, float slide,
		    struct pz_cellinfo *out)
{
	int y0 = (int)(tt + (float)r * cellH + 0.5f);
	int y1 = (int)(tt + (float)(r + 1) * cellH + 0.5f);
	int ch = y1 - y0;
	if (ch <= 0)
		return false;
	int x0 = (int)(tl + (float)c * cellW + 0.5f);
	int x1 = (int)(tl + (float)(c + 1) * cellW + 0.5f);
	int cw = x1 - x0;
	if (cw <= 0)
		return false;

	uint32_t cellid = (uint32_t)(r * cols + c);
	float jit = hash01(cellid * 2654435761u);

	float ts;
	if (s->order == 0)
		ts = (float)c * stag + jit * stag * 0.5f;
	else if (s->order == 1)
		ts = (float)(cols - 1 - c) * stag + jit * stag * 0.5f;
	else
		ts = jit * (float)cols * stag;

	float es = ts, ee = ts + enter_dur;
	float xs = hold_end + ts, xe = xs + enter_dur;

	float frac, a; /* frac: 0 = home, 1 = fully off toward its edge */
	if (lt < es) {
		frac = 1.0f;
		a = 0.0f; /* not entered yet */
	} else if (lt < ee) {
		float p = (lt - es) / enter_dur;
		float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
		frac = 1.0f - e;
		a = p;
	} else if (lt < xs) {
		frac = 0.0f;
		a = 1.0f; /* assembled / held */
	} else if (lt < xe) {
		float p = (lt - xs) / enter_dur;
		float e = p * p * p; /* ease in */
		frac = e;
		a = 1.0f - p;
	} else {
		frac = 1.0f;
		a = 0.0f; /* gone (between loops) */
	}
	if (a <= 0.001f)
		return false;

	int edge = (int)(hash01(cellid * 40503u + 7u) * 4.0f);
	if (edge > 3)
		edge = 3;
	float ox = 0.0f, oy = 0.0f;
	float mag = slide * (0.5f + 0.5f * jit);
	if (edge == 0)
		ox = -mag;
	else if (edge == 1)
		ox = mag;
	else if (edge == 2)
		oy = -mag;
	else
		oy = mag;

	out->x0 = x0;
	out->y0 = y0;
	out->cw = cw;
	out->ch = ch;
	out->dx = ox * frac;
	out->dy = oy * frac;
	out->a = a;
	return true;
}

static void puzzle_render(void *data, const struct fx_render_ctx *ctx)
{
	struct puzzle_state *s = data;
	const struct flametext_mask *mask = ctx->mask;
	if (!mask || !mask->tex || !s->fill)
		return;

	float tl = mask->text_left, tr = mask->text_right;
	float tt = mask->text_top, tb = mask->text_bottom;
	float bandW = tr - tl, bandH = tb - tt;
	if (bandW < 1.0f || bandH < 1.0f)
		return;

	int cols = s->grain < 1 ? 1 : s->grain;
	float cellW = bandW / (float)cols;
	int rows = (int)(bandH / cellW + 0.5f);
	if (rows < 1)
		rows = 1;
	float cellH = bandH / (float)rows;

	float sp = s->speed < 0.1f ? 0.1f : s->speed;
	float enter_dur = 0.7f / sp;
	float stag = 0.10f / sp; /* per-column reveal step */
	float slide = 0.6f * (float)(ctx->width > ctx->height ? ctx->width
							      : ctx->height);

	/* Loop timeline: assemble -> hold -> disassemble -> repeat. One-shot
	 * stops once assembled. */
	float maxts = (float)cols * stag;
	float all_in = maxts + enter_dur;
	float wait = s->wait < 0.0f ? 0.0f : s->wait;
	float hold_end = all_in + wait;
	float cycle = hold_end + (maxts + enter_dur) + 0.4f;
	float lt = s->loop ? fmodf(ctx->time, cycle)
			   : fminf(ctx->time, all_in);

	float rgba[4];
	unpack_color(s->font_color, rgba);
	float texW = (float)ctx->width, texH = (float)ctx->height;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	/* Glow / bloom haloes (each its own quad, positioned by the matrix). */
	bool draw_glow = s->glow_amt > 0.001f && s->glow;
	bool draw_bloom = s->bloom > 0.001f && s->glow;
	if (draw_glow || draw_bloom) {
		for (int r = 0; r < rows; ++r) {
			for (int c = 0; c < cols; ++c) {
				struct pz_cellinfo ci;
				if (!pz_cell(s, lt, tl, tt, cellW, cellH, cols, r,
					     c, enter_dur, stag, hold_end, slide,
					     &ci))
					continue;
				float fc[4] = {rgba[0], rgba[1], rgba[2],
					       rgba[3] * ci.a};
				gs_matrix_push();
				gs_matrix_translate3f((float)ci.x0 + ci.dx,
						      (float)ci.y0 + ci.dy, 0.0f);
				if (draw_glow)
					fx_glow_render_sub(s->glow, mask->tex,
							   ctx->width, ctx->height,
							   fc, s->glow_amt,
							   (uint32_t)ci.x0,
							   (uint32_t)ci.y0,
							   (uint32_t)ci.cw,
							   (uint32_t)ci.ch);
				if (draw_bloom)
					fx_glow_render_sub(s->glow, mask->tex,
							   ctx->width, ctx->height,
							   fc, s->bloom,
							   (uint32_t)ci.x0,
							   (uint32_t)ci.y0,
							   (uint32_t)ci.cw,
							   (uint32_t)ci.ch);
				gs_matrix_pop();
			}
		}
	}

	/* Fill: every visible piece batched into one draw, each quad carrying
	 * its own colour/alpha (same vertex path as the sprite system). */
	int visible = 0;
	for (int r = 0; r < rows && !visible; ++r)
		for (int c = 0; c < cols && !visible; ++c) {
			struct pz_cellinfo ci;
			if (pz_cell(s, lt, tl, tt, cellW, cellH, cols, r, c,
				    enter_dur, stag, hold_end, slide, &ci))
				visible = 1;
		}

	if (visible) {
		gs_eparam_t *pimg = gs_effect_get_param_by_name(s->fill, "image");
		if (pimg)
			gs_effect_set_texture(pimg, mask->tex);

		while (gs_effect_loop(s->fill, "Draw")) {
			gs_render_start(true);
			for (int r = 0; r < rows; ++r) {
				for (int c = 0; c < cols; ++c) {
					struct pz_cellinfo ci;
					if (!pz_cell(s, lt, tl, tt, cellW, cellH,
						     cols, r, c, enter_dur, stag,
						     hold_end, slide, &ci))
						continue;
					float fc[4] = {rgba[0], rgba[1], rgba[2],
						       rgba[3] * ci.a};
					uint32_t col = pack_rgba(fc);
					float X0 = (float)ci.x0 + ci.dx;
					float Y0 = (float)ci.y0 + ci.dy;
					float X1 = X0 + (float)ci.cw;
					float Y1 = Y0 + (float)ci.ch;
					float u0 = (float)ci.x0 / texW;
					float v0 = (float)ci.y0 / texH;
					float u1 = (float)(ci.x0 + ci.cw) / texW;
					float v1 = (float)(ci.y0 + ci.ch) / texH;
					float vx[4] = {X0, X1, X1, X0};
					float vy[4] = {Y0, Y0, Y1, Y1};
					float ux[4] = {u0, u1, u1, u0};
					float uy[4] = {v0, v0, v1, v1};
					int idx[6] = {0, 1, 2, 0, 2, 3};
					for (int t = 0; t < 6; ++t) {
						int k = idx[t];
						gs_texcoord(ux[k], uy[k], 0);
						gs_color(col);
						gs_vertex2f(vx[k], vy[k]);
					}
				}
			}
			gs_render_stop(GS_TRIS);
		}
	}

	gs_blend_state_pop();
}

static void puzzle_properties(obs_properties_t *p)
{
	obs_properties_add_color_alpha(p, "pz_font",
		obs_module_text("PzFontColor"));
	obs_properties_add_int_slider(p, "pz_grain",
		obs_module_text("PzGrain"), 2, 20, 1);
	obs_properties_add_float_slider(p, "pz_speed",
		obs_module_text("PzSpeed"), 0.2, 3.0, 0.05);
	obs_property_t *o = obs_properties_add_list(p, "pz_order",
		obs_module_text("PzOrder"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(o, obs_module_text("PzOrderLeft"), 0);
	obs_property_list_add_int(o, obs_module_text("PzOrderRight"), 1);
	obs_property_list_add_int(o, obs_module_text("PzOrderAll"), 2);
	obs_properties_add_float_slider(p, "pz_glow",
		obs_module_text("GlowAmount"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(p, "pz_bloom",
		obs_module_text("PzBloom"), 0.0, 3.0, 0.05);
	obs_properties_add_bool(p, "pz_loop", obs_module_text("LoopOn"));
	obs_properties_add_float_slider(p, "pz_wait",
		obs_module_text("LoopWait"), 0.0, 10.0, 0.1);
}

static void puzzle_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "pz_font", DEFAULT_PZ_FONT);
	obs_data_set_default_int(settings, "pz_grain", 6);
	obs_data_set_default_double(settings, "pz_speed", 1.0);
	obs_data_set_default_int(settings, "pz_order", 0);
	obs_data_set_default_double(settings, "pz_glow", 0.0);
	obs_data_set_default_double(settings, "pz_bloom", 0.0);
	obs_data_set_default_bool(settings, "pz_loop", true);
	obs_data_set_default_double(settings, "pz_wait", 1.5);
}

const struct text_effect fx_puzzle = {
	.id             = "puzzle",
	.name_key       = "EffectPuzzle",
	.create         = puzzle_create,
	.destroy        = puzzle_destroy,
	.load_graphics  = puzzle_load_graphics,
	.update         = puzzle_update,
	.render         = puzzle_render,
	.get_properties = puzzle_properties,
	.get_defaults   = puzzle_defaults,
};
