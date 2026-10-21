#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Sizing for the blurred-background mode of the segmentation filter.
 *
 * The blur runs on a reduced copy of the frame: a wide Gaussian on the full
 * picture would need hundreds of taps per pixel, while on a copy shrunk by a
 * power of two the same visual radius needs only a dozen.  The shader is
 * unrolled to BACKGROUND_BLUR_TAPS taps per side, so the reduction factor is
 * picked to keep the radius (in reduced pixels) inside that.
 *
 * Everything here is pure arithmetic so it can be tested without a GPU. */

/* Taps on each side of the centre in background_alpha.effect (PSBlurPass).
 * Changing this means editing the unrolled shader too. */
#define BACKGROUND_BLUR_TAPS 12

/* Strength 100 blurs with a radius of this fraction of the frame height.
 * A fraction rather than pixels so the look matches across 720p and 4K. */
#define BACKGROUND_BLUR_MAX_RADIUS 0.06f

#define BACKGROUND_BLUR_MIN_STRENGTH 1
#define BACKGROUND_BLUR_MAX_STRENGTH 100

#define BACKGROUND_BLUR_MIN_SCALE 2
#define BACKGROUND_BLUR_MAX_SCALE 32

/* A Gaussian is negligible beyond three sigma, so the kernel's outer tap sits
 * at 3 * sigma. */
#define BACKGROUND_BLUR_SIGMA_SPAN 3.0f

/* Below this the kernel is narrower than a pixel and degenerates to a plain
 * downscale/upscale, which is still a (small) blur and avoids dividing by 0. */
#define BACKGROUND_BLUR_MIN_SIGMA 0.6f

struct background_blur_plan {
	bool valid;
	uint32_t scale;  /* full-size pixels per reduced pixel, a power of two */
	uint32_t width;  /* reduced size */
	uint32_t height;
	float radius;    /* blur radius in full-size pixels */
	float sigma;     /* Gaussian sigma in reduced pixels */
};

static inline struct background_blur_plan background_blur_plan_for(uint32_t cx, uint32_t cy, int strength)
{
	struct background_blur_plan plan = {0};

	if (!cx || !cy)
		return plan;

	if (strength < BACKGROUND_BLUR_MIN_STRENGTH)
		strength = BACKGROUND_BLUR_MIN_STRENGTH;
	if (strength > BACKGROUND_BLUR_MAX_STRENGTH)
		strength = BACKGROUND_BLUR_MAX_STRENGTH;

	plan.radius = (float)cy * BACKGROUND_BLUR_MAX_RADIUS * ((float)strength / BACKGROUND_BLUR_MAX_STRENGTH);

	plan.scale = BACKGROUND_BLUR_MIN_SCALE;
	while (plan.radius / (float)plan.scale > (float)BACKGROUND_BLUR_TAPS && plan.scale < BACKGROUND_BLUR_MAX_SCALE)
		plan.scale *= 2;

	plan.width = (cx + plan.scale - 1) / plan.scale;
	plan.height = (cy + plan.scale - 1) / plan.scale;

	plan.sigma = plan.radius / (float)plan.scale / BACKGROUND_BLUR_SIGMA_SPAN;
	if (plan.sigma < BACKGROUND_BLUR_MIN_SIGMA)
		plan.sigma = BACKGROUND_BLUR_MIN_SIGMA;

	plan.valid = true;
	return plan;
}
