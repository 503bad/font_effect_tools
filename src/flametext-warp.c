#include "flametext-warp.h"

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>

#include <plugin-support.h>

/* Grid resolution of the warp mesh. Columns dominate the arch smoothness
 * (the curve runs along x); a full 360° circle across COLS columns is ~2.8°
 * per segment. Rows carry the perspective/shear gradients (linear, so few are
 * needed) and the radial subdivision of the arch. */
#define WARP_COLS 128
#define WARP_ROWS 16

#define TWO_PI 6.28318530718f

/* Upper bound on any single-side margin (px) so a near-flat arch (huge radius)
 * can never ask for an absurd canvas. */
#define WARP_MARGIN_CAP 4000u

void fx_warp_load(struct fx_warp *g)
{
	/* Graphics lock held by the host. */
	char *path = obs_module_file("effects/warp.effect");
	if (path) {
		g->effect = gs_effect_create_from_file(path, NULL);
		if (!g->effect)
			obs_log(LOG_ERROR, "failed to load warp.effect (%s)",
				path);
	}
	bfree(path);

	/* Build the static tessellated grid once. Each vertex stores its
	 * normalized 0..1 cell coordinate in BOTH the position (the shader
	 * reads it as geometry) and the single 2-wide texcoord (sampling). */
	const uint32_t cols = WARP_COLS, rows = WARP_ROWS;
	const size_t nverts = (size_t)cols * rows * 6;

	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = nverts;
	vbd->points = bzalloc(sizeof(struct vec3) * nverts);
	vbd->num_tex = 1;
	vbd->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 2;
	vbd->tvarray[0].array = bzalloc(sizeof(float) * 2 * nverts);

	float *uvs = vbd->tvarray[0].array;
	size_t k = 0;
	for (uint32_t r = 0; r < rows; ++r) {
		for (uint32_t c = 0; c < cols; ++c) {
			float u0 = (float)c / (float)cols;
			float u1 = (float)(c + 1) / (float)cols;
			float v0 = (float)r / (float)rows;
			float v1 = (float)(r + 1) / (float)rows;

			/* Two triangles per cell: (u0,v0)(u1,v0)(u1,v1) and
			 * (u0,v0)(u1,v1)(u0,v1). */
			float quad[6][2] = {
				{u0, v0}, {u1, v0}, {u1, v1},
				{u0, v0}, {u1, v1}, {u0, v1},
			};
			for (int i = 0; i < 6; ++i, ++k) {
				vec3_set(&vbd->points[k], quad[i][0],
					 quad[i][1], 0.0f);
				uvs[k * 2 + 0] = quad[i][0];
				uvs[k * 2 + 1] = quad[i][1];
			}
		}
	}

	g->mesh = gs_vertexbuffer_create(vbd, 0); /* immutable */
	if (!g->mesh)
		obs_log(LOG_ERROR, "failed to build warp mesh");
}

void fx_warp_free(struct fx_warp *g)
{
	/* Graphics lock held by the host. */
	if (g->effect)
		gs_effect_destroy(g->effect);
	if (g->mesh)
		gs_vertexbuffer_destroy(g->mesh);
	if (g->capture)
		gs_texrender_destroy(g->capture);
	g->effect = NULL;
	g->mesh = NULL;
	g->capture = NULL;
}

void fx_warp_update(struct fx_warp *g, obs_data_t *settings)
{
	g->enabled = obs_data_get_bool(settings, "warp_on");
	/* Stretch sliders are percent (100 == neutral). */
	g->scale_x = (float)obs_data_get_int(settings, "warp_stretch_x") / 100.0f;
	g->scale_y = (float)obs_data_get_int(settings, "warp_stretch_y") / 100.0f;
	/* Sliders are -100..100; map arch/persp to -1..1 and the shears to a
	 * slope (1.0 == a 45° lean at the text-band edges). */
	g->arch = (float)obs_data_get_int(settings, "warp_arch") / 100.0f;
	g->persp = (float)obs_data_get_int(settings, "warp_persp") / 100.0f;
	g->shear_h = (float)obs_data_get_int(settings, "warp_shear_h") / 100.0f;
	g->shear_v = (float)obs_data_get_int(settings, "warp_shear_v") / 100.0f;
}

bool fx_warp_active(const struct fx_warp *g)
{
	if (!g->enabled || !g->effect || !g->mesh)
		return false;
	return fabsf(g->scale_x - 1.0f) > 1e-4f ||
	       fabsf(g->scale_y - 1.0f) > 1e-4f || fabsf(g->arch) > 1e-4f ||
	       fabsf(g->persp) > 1e-4f || fabsf(g->shear_h) > 1e-4f ||
	       fabsf(g->shear_v) > 1e-4f;
}

static uint32_t cap_margin(float v)
{
	if (v <= 0.0f)
		return 0u;
	uint32_t m = (uint32_t)ceilf(v);
	return m > WARP_MARGIN_CAP ? WARP_MARGIN_CAP : m;
}

void fx_warp_margins(const struct fx_warp *g,
		     const struct flametext_mask *mask, struct fx_margins *out)
{
	out->left = out->right = out->top = out->bottom = 0u;
	if (!fx_warp_active(g) || !mask)
		return;

	float tw = mask->text_right - mask->text_left;
	float th = mask->text_bottom - mask->text_top;
	if (tw <= 1.0f || th <= 1.0f)
		return;

	float side = 0.0f;   /* extra left/right */
	float updown = 0.0f; /* extra top/bottom */

	/* Stretch happens first, so every later term sees the scaled band, and
	 * the scale itself enlarges the box when stretching past neutral. */
	float tws = tw * g->scale_x;
	float ths = th * g->scale_y;
	if (g->scale_x > 1.0f)
		side += (g->scale_x - 1.0f) * tw * 0.5f;
	if (g->scale_y > 1.0f)
		updown += (g->scale_y - 1.0f) * th * 0.5f;

	/* Shear slides one pair of edges; the opposite pair anchors, so the
	 * bounding box grows by the full slid distance on each affected side. */
	side += fabsf(g->shear_h) * ths * 0.5f;
	updown += fabsf(g->shear_v) * tws * 0.5f;

	/* Arch: half-angle swept across the text width, clamped to a semicircle
	 * where the reach maxes out. Vertical extent is the arc sagitta plus
	 * half the text height; horizontal extent can exceed the flat half
	 * width as the curve bulges sideways. */
	float ab = fabsf(g->arch);
	if (ab > 1e-4f) {
		float span = ab * TWO_PI;
		float R0 = tws / span;
		float half = span * 0.5f;
		if (half > (float)M_PI)
			half = (float)M_PI;
		float vext = R0 * (1.0f - cosf(half)) + ths * 0.5f;
		float hext = R0 * sinf(half) - tws * 0.5f + ths * 0.5f;
		updown += vext;
		if (hext > side)
			side = hext; /* dominate, don't double-count */
	}

	out->left = out->right = cap_margin(side);
	out->top = out->bottom = cap_margin(updown);
}

void fx_warp_get_properties(obs_properties_t *props)
{
	obs_properties_t *grp = obs_properties_create();

	obs_properties_add_int_slider(grp, "warp_stretch_x",
				      obs_module_text("WarpStretchX"), 10, 400, 1);
	obs_properties_add_int_slider(grp, "warp_stretch_y",
				      obs_module_text("WarpStretchY"), 10, 400, 1);
	obs_properties_add_int_slider(grp, "warp_arch",
				      obs_module_text("WarpArch"), -100, 100, 1);
	obs_properties_add_int_slider(grp, "warp_persp",
				      obs_module_text("WarpPersp"), -100, 100, 1);
	obs_properties_add_int_slider(grp, "warp_shear_h",
				      obs_module_text("WarpShearH"), -100, 100, 1);
	obs_properties_add_int_slider(grp, "warp_shear_v",
				      obs_module_text("WarpShearV"), -100, 100, 1);

	obs_properties_add_group(props, "warp_on", obs_module_text("Warp"),
				 OBS_GROUP_CHECKABLE, grp);
}

void fx_warp_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "warp_on", false);
	obs_data_set_default_int(settings, "warp_stretch_x", 100);
	obs_data_set_default_int(settings, "warp_stretch_y", 100);
	obs_data_set_default_int(settings, "warp_arch", 0);
	obs_data_set_default_int(settings, "warp_persp", 0);
	obs_data_set_default_int(settings, "warp_shear_h", 0);
	obs_data_set_default_int(settings, "warp_shear_v", 0);
}

bool fx_warp_begin(struct fx_warp *g, uint32_t w, uint32_t h)
{
	if (!fx_warp_active(g))
		return false;

	if (!g->capture)
		g->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!g->capture)
		return false;

	gs_texrender_reset(g->capture);
	if (!gs_texrender_begin(g->capture, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

void fx_warp_end(struct fx_warp *g, uint32_t w, uint32_t h,
		 const struct flametext_mask *mask)
{
	gs_texrender_end(g->capture);

	gs_texture_t *body = gs_texrender_get_texture(g->capture);
	if (!body)
		return;

	gs_effect_t *e = g->effect;
	gs_eparam_t *p;

	if ((p = gs_effect_get_param_by_name(e, "image")))
		gs_effect_set_texture(p, body);
	if ((p = gs_effect_get_param_by_name(e, "canvas"))) {
		struct vec2 c;
		vec2_set(&c, (float)w, (float)h);
		gs_effect_set_vec2(p, &c);
	}
	if ((p = gs_effect_get_param_by_name(e, "tbox"))) {
		struct vec4 b;
		if (mask) {
			float cx = (mask->text_left + mask->text_right) * 0.5f;
			float cy = (mask->text_top + mask->text_bottom) * 0.5f;
			float tw = mask->text_right - mask->text_left;
			float th = mask->text_bottom - mask->text_top;
			vec4_set(&b, cx, cy, tw, th);
		} else {
			vec4_set(&b, (float)w * 0.5f, (float)h * 0.5f,
				 (float)w, (float)h);
		}
		gs_effect_set_vec4(p, &b);
	}
	if ((p = gs_effect_get_param_by_name(e, "stretch"))) {
		struct vec2 sc;
		vec2_set(&sc, g->scale_x, g->scale_y);
		gs_effect_set_vec2(p, &sc);
	}
	if ((p = gs_effect_get_param_by_name(e, "arch")))
		gs_effect_set_float(p, g->arch);
	if ((p = gs_effect_get_param_by_name(e, "persp")))
		gs_effect_set_float(p, g->persp);
	if ((p = gs_effect_get_param_by_name(e, "shear_h")))
		gs_effect_set_float(p, g->shear_h);
	if ((p = gs_effect_get_param_by_name(e, "shear_v")))
		gs_effect_set_float(p, g->shear_v);

	/* The capture holds premultiplied colour: composite it src-over. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "Draw")) {
		gs_load_vertexbuffer(g->mesh);
		gs_load_indexbuffer(NULL);
		gs_draw(GS_TRIS, 0, 0);
	}
	gs_blend_state_pop();
}
