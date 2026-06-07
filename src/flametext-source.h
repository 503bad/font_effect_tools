#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flametext_mask;
struct spark_system;
struct gs_effect;
typedef struct gs_effect gs_effect_t;

/* Per-instance state for a Flame Text source. */
struct flametext_source {
	obs_source_t *source;

	/* Text / font settings */
	char    *text;
	char    *font_face;
	uint32_t font_size;
	bool     bold;
	bool     italic;
	char     font_path[1024];

	/* Flame shader parameters */
	float flame_height;  /* upward reach, fraction of canvas (0..1) */
	float sway_speed;    /* turbulence scroll speed                 */
	float color_temp;    /* heat multiplier                         */
	float intensity;     /* overall strength                        */

	/* Spark parameters */
	float    emit_rate;
	float    init_speed;
	float    lifetime;
	float    spark_size;
	float    spread;
	uint32_t spark_color; /* OBS 0xAABBGGRR */
	int      spark_origin; /* 0 = above the text, 1 = below the text */
	float    bloom;        /* additive glow strength for particles */

	/* Runtime resources */
	struct flametext_mask *mask;
	struct spark_system   *sparks;
	gs_effect_t           *flame_effect;
	gs_effect_t           *spark_effect;

	float clock; /* seconds since show() */
};

void flametext_register_source(void);

#ifdef __cplusplus
}
#endif
