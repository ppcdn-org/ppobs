/* Covers the sizing of the blurred-background mode.
 *
 * The shader's kernel is unrolled to a fixed number of taps, so the plan has
 * to keep the radius inside that for every frame size and strength a user can
 * reach, and the unrolled shader has to actually contain that many taps.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "background-blur.h"

static bool is_power_of_two(uint32_t v)
{
	return v && !(v & (v - 1));
}

static void test_empty_frame_is_invalid(void)
{
	assert(!background_blur_plan_for(0, 1080, 50).valid);
	assert(!background_blur_plan_for(1920, 0, 50).valid);
}

static void test_kernel_always_fits_the_unrolled_taps(void)
{
	static const uint32_t heights[] = {1, 2, 120, 480, 720, 1080, 1440, 2160, 4320};

	for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
		for (int strength = -5; strength <= 120; strength++) {
			const uint32_t cy = heights[h];
			const struct background_blur_plan plan = background_blur_plan_for(cy * 16 / 9 + 1, cy, strength);

			assert(plan.valid);
			assert(is_power_of_two(plan.scale));
			assert(plan.scale >= BACKGROUND_BLUR_MIN_SCALE && plan.scale <= BACKGROUND_BLUR_MAX_SCALE);
			assert(plan.sigma >= BACKGROUND_BLUR_MIN_SIGMA);

			/* The outermost tap must not be asked to reach past the
			 * kernel the shader has.  Only checkable while the scale cap
			 * has not been hit; tiny frames never reach it. */
			if (plan.scale < BACKGROUND_BLUR_MAX_SCALE && plan.sigma > BACKGROUND_BLUR_MIN_SIGMA)
				assert(plan.sigma * BACKGROUND_BLUR_SIGMA_SPAN <= (float)BACKGROUND_BLUR_TAPS + 0.001f);
		}
	}
}

static void test_reduced_size_covers_the_frame(void)
{
	const struct background_blur_plan plan = background_blur_plan_for(1921, 1081, 100);

	assert(plan.valid);
	assert(plan.width * plan.scale >= 1921);
	assert(plan.height * plan.scale >= 1081);
	assert((plan.width - 1) * plan.scale < 1921);
	assert((plan.height - 1) * plan.scale < 1081);
}

static void test_stronger_blur_is_never_weaker(void)
{
	float previous = 0.0f;

	for (int strength = 1; strength <= 100; strength++) {
		const struct background_blur_plan plan = background_blur_plan_for(1920, 1080, strength);

		assert(plan.radius >= previous);
		previous = plan.radius;
	}
}

static void test_radius_follows_frame_height(void)
{
	/* The same strength should look the same at any resolution, so the
	 * radius is proportional to height. */
	const struct background_blur_plan hd = background_blur_plan_for(1920, 1080, 50);
	const struct background_blur_plan uhd = background_blur_plan_for(3840, 2160, 50);

	assert(uhd.radius > hd.radius * 1.99f && uhd.radius < hd.radius * 2.01f);
}

static void test_out_of_range_strength_is_clamped(void)
{
	const struct background_blur_plan low = background_blur_plan_for(1920, 1080, -10);
	const struct background_blur_plan one = background_blur_plan_for(1920, 1080, 1);
	const struct background_blur_plan high = background_blur_plan_for(1920, 1080, 1000);
	const struct background_blur_plan max = background_blur_plan_for(1920, 1080, 100);

	assert(low.radius == one.radius);
	assert(high.radius == max.radius);
}

#ifdef BLUR_EFFECT_PATH
/* The shader cannot read BACKGROUND_BLUR_TAPS, so the two are tied together
 * here: one blur_pair() call per tap, numbered 1..TAPS. */
static void test_shader_has_the_taps_the_header_promises(void)
{
	FILE *file = fopen(BLUR_EFFECT_PATH, "rb");
	assert(file);

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *text = malloc((size_t)size + 1);
	assert(text);
	assert(fread(text, 1, (size_t)size, file) == (size_t)size);
	text[size] = '\0';
	fclose(file);

	for (int k = 1; k <= BACKGROUND_BLUR_TAPS; k++) {
		char needle[64];
		snprintf(needle, sizeof(needle), "blur_pair(vert_in.uv, %d.0)", k);
		assert(strstr(text, needle) != NULL);
	}

	char beyond[64];
	snprintf(beyond, sizeof(beyond), "blur_pair(vert_in.uv, %d.0)", BACKGROUND_BLUR_TAPS + 1);
	assert(strstr(text, beyond) == NULL);

	free(text);
}
#endif

int main(void)
{
	test_empty_frame_is_invalid();
	test_kernel_always_fits_the_unrolled_taps();
	test_reduced_size_covers_the_frame();
	test_stronger_blur_is_never_weaker();
	test_radius_follows_frame_height();
	test_out_of_range_strength_is_clamped();
#ifdef BLUR_EFFECT_PATH
	test_shader_has_the_taps_the_header_promises();
#endif
	return 0;
}
