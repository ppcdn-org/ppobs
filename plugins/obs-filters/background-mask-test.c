#include "background-mask.h"

#include <util/bmem.h>

#include <assert.h>
#include <string.h>

static void test_resize(void)
{
	struct background_mask mask = {0};

	assert(background_mask_resize(&mask, 4, 4));
	assert(mask.width == 4 && mask.height == 4);
	assert(mask.data != NULL);

	/* Shrinking keeps the larger allocation so steady-state operation does
	 * not reallocate every frame. */
	const size_t capacity = mask.capacity;
	assert(background_mask_resize(&mask, 2, 2));
	assert(mask.width == 2 && mask.height == 2 && mask.capacity == capacity);

	/* Degenerate sizes must not leave a half-configured mask behind. */
	assert(!background_mask_resize(&mask, 0, 4));
	assert(mask.width == 0 && mask.height == 0);

	background_mask_free(&mask);
	assert(mask.data == NULL && mask.capacity == 0);
}

static void test_threshold(void)
{
	struct background_mask mask = {0};
	const float probabilities[4] = {0.0f, 0.4f, 0.6f, 1.0f};

	/* Zero softness is a hard cut at the threshold. */
	assert(background_mask_from_probabilities(&mask, probabilities, 4, 4, 1, 0.5f, 0.0f));
	assert(mask.data[0] == 0);
	assert(mask.data[1] == 0);
	assert(mask.data[2] == 255);
	assert(mask.data[3] == 255);

	/* A ramp puts intermediate probabilities at intermediate coverage, and
	 * still saturates outside the band. */
	assert(background_mask_from_probabilities(&mask, probabilities, 4, 4, 1, 0.5f, 0.25f));
	assert(mask.data[0] == 0);
	assert(mask.data[1] > 0 && mask.data[1] < 255);
	assert(mask.data[2] > mask.data[1]);
	assert(mask.data[3] == 255);

	/* Count must agree with the geometry or the mask is rejected. */
	assert(!background_mask_from_probabilities(&mask, probabilities, 4, 3, 1, 0.5f, 0.0f));

	background_mask_free(&mask);
}

static void test_smooth(void)
{
	struct background_mask current = {0};
	struct background_mask history = {0};

	assert(background_mask_resize(&current, 2, 1));
	current.data[0] = 0;
	current.data[1] = 255;

	/* First call seeds the history rather than blending against nothing. */
	assert(background_mask_smooth(&current, &history, 0.5f));
	assert(current.data[0] == 0 && current.data[1] == 255);
	assert(history.width == 2 && history.height == 1);

	/* Half weight lands halfway between the new and previous frames. */
	current.data[0] = 255;
	current.data[1] = 255;
	assert(background_mask_smooth(&current, &history, 0.5f));
	assert(current.data[0] > 120 && current.data[0] < 136);
	assert(current.data[1] == 255);

	/* Full weight disables smoothing entirely. */
	current.data[0] = 10;
	assert(background_mask_smooth(&current, &history, 1.0f));
	assert(current.data[0] == 10);
	assert(history.data[0] == 10);

	/* A geometry change must not blend the old resolution into the new. */
	assert(background_mask_resize(&current, 3, 1));
	current.data[0] = 200;
	current.data[1] = 200;
	current.data[2] = 200;
	assert(background_mask_smooth(&current, &history, 0.5f));
	assert(current.data[0] == 200);
	assert(history.width == 3);

	background_mask_free(&current);
	background_mask_free(&history);
}

static void test_morphology(void)
{
	struct background_mask mask = {0};
	struct background_mask scratch = {0};

	/* A single lit pixel in a 5x5 field: erode clears it, dilate spreads it
	 * over the kernel footprint. */
	assert(background_mask_resize(&mask, 5, 5));
	memset(mask.data, 0, 25);
	mask.data[12] = 255;

	assert(background_mask_dilate(&mask, &scratch, 1));
	assert(mask.data[12] == 255);
	assert(mask.data[11] == 255 && mask.data[13] == 255);
	assert(mask.data[7] == 255 && mask.data[17] == 255);
	assert(mask.data[0] == 0);

	assert(background_mask_erode(&mask, &scratch, 1));
	assert(mask.data[12] == 255);
	assert(mask.data[11] == 0 && mask.data[13] == 0);

	/* Radius zero is a no-op rather than an error. */
	assert(background_mask_erode(&mask, &scratch, 0));
	assert(mask.data[12] == 255);

	background_mask_free(&mask);
	background_mask_free(&scratch);
}

static void test_feather(void)
{
	struct background_mask mask = {0};
	struct background_mask scratch = {0};

	/* A hard vertical edge should become a gradient, and the extremes
	 * should move toward each other rather than stay saturated. */
	assert(background_mask_resize(&mask, 4, 1));
	mask.data[0] = 0;
	mask.data[1] = 0;
	mask.data[2] = 255;
	mask.data[3] = 255;

	assert(background_mask_feather(&mask, &scratch, 1));
	assert(mask.data[0] == 0);
	assert(mask.data[1] > 0 && mask.data[1] < 255);
	assert(mask.data[2] > mask.data[1]);
	assert(mask.data[3] == 255);

	background_mask_free(&mask);
	background_mask_free(&scratch);
}

static void test_estimate_colour(void)
{
	struct background_mask mask = {0};
	const uint32_t width = 64;
	const uint32_t height = 64;
	uint8_t *rgb = bmalloc((size_t)width * height * 3);

	/* Left half is subject, right half is the light grey wall the demo
	 * footage was shot against. */
	assert(background_mask_resize(&mask, width, height));
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const size_t i = (size_t)y * width + x;
			const bool subject = x < width / 2;

			mask.data[i] = subject ? 255 : 0;
			rgb[i * 3 + 0] = subject ? 40 : 230;
			rgb[i * 3 + 1] = subject ? 50 : 235;
			rgb[i * 3 + 2] = subject ? 60 : 242;
		}
	}

	struct background_colour colour = background_mask_estimate_colour(&mask, rgb, width, height, 3);
	assert(colour.valid);
	/* The wall colour must come back, not an average with the subject. */
	assert(colour.rgb[0] == 230.0f && colour.rgb[1] == 235.0f && colour.rgb[2] == 242.0f);

	/* The median must ignore a few dark pixels the matte let through,
	 * which a mean would drag the estimate toward. */
	for (uint32_t x = width / 2; x < width / 2 + 3; x++) {
		const size_t i = x;
		rgb[i * 3 + 0] = 0;
		rgb[i * 3 + 1] = 0;
		rgb[i * 3 + 2] = 0;
	}
	colour = background_mask_estimate_colour(&mask, rgb, width, height, 3);
	assert(colour.valid);
	assert(colour.rgb[0] == 230.0f);

	/* An all-subject frame has no wall to measure, and must say so rather
	 * than return a colour built from a handful of pixels. */
	memset(mask.data, 255, (size_t)width * height);
	colour = background_mask_estimate_colour(&mask, rgb, width, height, 3);
	assert(!colour.valid);

	/* A mask smaller than the frame is the normal case: the matte is at
	 * model resolution and the frame is not. */
	assert(background_mask_resize(&mask, 8, 8));
	memset(mask.data, 0, 64);
	colour = background_mask_estimate_colour(&mask, rgb, width, height, 3);
	assert(colour.valid);

	bfree(rgb);
	background_mask_free(&mask);
}

/* The filter measures the RGBA readback in place, so a four-byte stride has to
 * land on the same colour a packed RGB buffer would. */
static void test_estimate_colour_rgba_stride(void)
{
	struct background_mask mask = {0};
	const uint32_t width = 32;
	const uint32_t height = 32;
	uint8_t *rgba = bmalloc((size_t)width * height * 4);

	assert(background_mask_resize(&mask, width, height));
	for (uint32_t i = 0; i < width * height; i++) {
		const bool subject = (i % width) < width / 2;

		mask.data[i] = subject ? 255 : 0;
		rgba[i * 4 + 0] = subject ? 40 : 228;
		rgba[i * 4 + 1] = subject ? 50 : 233;
		rgba[i * 4 + 2] = subject ? 60 : 241;
		/* Opaque alpha, which must not be mistaken for a colour. */
		rgba[i * 4 + 3] = 255;
	}

	struct background_colour colour = background_mask_estimate_colour(&mask, rgba, width, height, 4);
	assert(colour.valid);
	assert(colour.rgb[0] == 228.0f && colour.rgb[1] == 233.0f && colour.rgb[2] == 241.0f);

	bfree(rgba);
	background_mask_free(&mask);
}

static void test_estimate_colour_rejects_bad_input(void)
{
	struct background_mask mask = {0};
	uint8_t rgb[3 * 4] = {0};

	/* Before the first inference there is no mask, which must not be read
	 * as "the whole frame is background". */
	struct background_colour colour = background_mask_estimate_colour(&mask, rgb, 2, 2, 3);
	assert(!colour.valid);

	assert(background_mask_resize(&mask, 2, 2));
	colour = background_mask_estimate_colour(&mask, NULL, 2, 2, 3);
	assert(!colour.valid);
	colour = background_mask_estimate_colour(&mask, rgb, 0, 2, 3);
	assert(!colour.valid);
	/* A stride narrower than one pixel would read past every sample. */
	colour = background_mask_estimate_colour(&mask, rgb, 2, 2, 2);
	assert(!colour.valid);

	background_mask_free(&mask);
}

static void test_rejects_empty(void)
{
	struct background_mask empty = {0};
	struct background_mask scratch = {0};

	/* Every operation must tolerate a mask that was never sized, since that
	 * is the state before the first successful inference. */
	assert(!background_mask_erode(&empty, &scratch, 1));
	assert(!background_mask_dilate(&empty, &scratch, 1));
	assert(!background_mask_feather(&empty, &scratch, 1));
	assert(!background_mask_smooth(&empty, &scratch, 0.5f));
}

int main(void)
{
	test_resize();
	test_threshold();
	test_smooth();
	test_morphology();
	test_feather();
	test_estimate_colour();
	test_estimate_colour_rgba_stride();
	test_estimate_colour_rejects_bad_input();
	test_rejects_empty();
	return 0;
}
