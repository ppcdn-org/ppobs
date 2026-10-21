#pragma once

#include <graphics/graphics.h>
#include <graphics/vec2.h>

#include "background-blur.h"

/* The GPU side of the blurred-background mode: the reduced copy and the two
 * Gaussian passes over it.  Split from the filter so that the same code runs in
 * background-blur-gpu-test, which checks the pixels it produces.
 *
 * Everything here is graphics-thread only. */

enum background_blur_target {
	BLUR_TARGET_REDUCED, /* the frame shrunk to the working size */
	BLUR_TARGET_MIDDLE,  /* after the horizontal pass: weighted colour + weight, so float */
	BLUR_TARGET_RESULT,  /* after the vertical pass: plain colour */
	BLUR_TARGET_COUNT,
};

/* One render target per stage, so no pass ever samples the texture it is
 * drawing into.  Created on first use. */
struct background_blur_targets {
	gs_texrender_t *render[BLUR_TARGET_COUNT];
};

/* The matte the blur is weighted by, with the same placement parameters the
 * composite uses so the two always agree on where the subject is. */
struct background_blur_matte {
	gs_texture_t *texture;
	struct vec2 scale;
	struct vec2 offset;
};

/* Draws the source into the render target that is current, stretched to fill
 * the given reduced size.  Called between gs_texrender_begin and _end. */
typedef void (*background_blur_draw_fn)(void *data, uint32_t width, uint32_t height);

/* Returns the finished blur, or NULL if a target could not be created or drawn,
 * in which case the caller should fall back to the untouched source.  The
 * texture belongs to `targets` and is valid until the next call. */
gs_texture_t *background_blur_render(gs_effect_t *effect, struct background_blur_targets *targets,
				     const struct background_blur_plan *plan, const struct background_blur_matte *matte,
				     background_blur_draw_fn draw, void *draw_data);

void background_blur_targets_destroy(struct background_blur_targets *targets);

/* Sets the effect's matte placement parameters.  An effect forgets every
 * parameter when a technique ends (gs_technique_end), so this has to be repeated
 * before each draw that reads the matte - the blur passes and the composite. */
void background_blur_set_matte(gs_effect_t *effect, const struct background_blur_matte *matte);
