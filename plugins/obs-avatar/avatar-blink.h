/* A blink, drawn over the artwork on the GPU.
 *
 * A generated frame is only the lower face, so nothing about the eyes comes from
 * the worker except where they are. The blink is the upper lid coming down: a
 * patch of the skin just above each eye is laid over it, its edge following the
 * lid's curve from the open lid to the lower lid, with a lash line along the
 * edge. Done this way and not by stretching the eye's own pixels, which leaves
 * vertical streaks as soon as the lid has to cover the whole eye.
 *
 * The timing is the other half of it. People do not blink on a clock, and a
 * regular blink looks mechanical faster than a missing one does. Intervals are
 * random between two and six seconds, now and then a second blink follows the
 * first, and the lid closes faster than it opens.
 */

#pragma once

#include "avatar-eyes.h"

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

struct avatar_blink {
	gs_effect_t *effect;
	bool failed;

	uint64_t start_ns; /* when the blink now in progress began, 0 for none */
	uint64_t next_ns;  /* when the next one begins */
	uint64_t rng;
};

void avatar_blink_init(struct avatar_blink *blink, uint64_t now_ns);

/* How shut the eyes are now: 0 open, 1 closed. Starts the next blink when its
 * time has come. */
float avatar_blink_amount(struct avatar_blink *blink, uint64_t now_ns);

/* Draws the lids over what has been drawn of `base` so far, with the sprite
 * `cx` by `cy`, which has to be the size and place `base` was drawn at. The
 * graphics context must be current. */
void avatar_blink_draw(struct avatar_blink *blink, gs_texture_t *base, uint32_t cx, uint32_t cy,
		       const struct avatar_eyes *eyes, float amount);

/* Graphics context current. */
void avatar_blink_free(struct avatar_blink *blink);

#ifdef __cplusplus
}
#endif
