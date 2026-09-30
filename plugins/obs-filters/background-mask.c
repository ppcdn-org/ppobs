#include "background-mask.h"

#include <util/bmem.h>

#include <string.h>

/* Masks stay at model resolution (a few hundred pixels a side), so the naive
 * separable passes below are cheap enough and run on the worker thread. */
#define BACKGROUND_MASK_MAX_DIM 4096

static inline uint8_t clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (uint8_t)value;
}

bool background_mask_resize(struct background_mask *mask, uint32_t width, uint32_t height)
{
	if (!mask)
		return false;

	if (!width || !height || width > BACKGROUND_MASK_MAX_DIM || height > BACKGROUND_MASK_MAX_DIM) {
		mask->width = 0;
		mask->height = 0;
		return false;
	}

	const size_t required = (size_t)width * height;

	if (required > mask->capacity) {
		uint8_t *data = bmalloc(required);
		if (!data) {
			mask->width = 0;
			mask->height = 0;
			return false;
		}

		bfree(mask->data);
		mask->data = data;
		mask->capacity = required;
	}

	mask->width = width;
	mask->height = height;
	return true;
}

void background_mask_free(struct background_mask *mask)
{
	if (!mask)
		return;

	bfree(mask->data);
	memset(mask, 0, sizeof(*mask));
}

bool background_mask_from_probabilities(struct background_mask *mask, const float *probabilities, size_t count,
					uint32_t width, uint32_t height, float threshold, float softness)
{
	if (!mask || !probabilities)
		return false;
	if (!width || !height || (size_t)width * height != count)
		return false;
	if (!background_mask_resize(mask, width, height))
		return false;

	if (softness < 0.0f)
		softness = 0.0f;

	/* Below `low` is fully background, above `high` fully subject, and the
	 * band between them ramps linearly.  Collapsing the band to a point
	 * gives the hard threshold. */
	const float low = threshold - softness;
	const float high = threshold + softness;
	const float span = high - low;

	for (size_t i = 0; i < count; i++) {
		const float value = probabilities[i];
		float alpha;

		if (span <= 0.0f) {
			alpha = value >= threshold ? 1.0f : 0.0f;
		} else if (value <= low) {
			alpha = 0.0f;
		} else if (value >= high) {
			alpha = 1.0f;
		} else {
			alpha = (value - low) / span;
		}

		mask->data[i] = clamp_u8((int)(alpha * 255.0f + 0.5f));
	}

	return true;
}

bool background_mask_smooth(struct background_mask *current, struct background_mask *history, float factor)
{
	if (!current || !history || !current->data || !current->width || !current->height)
		return false;

	const size_t count = (size_t)current->width * current->height;

	/* A size change means the history describes a different frame geometry;
	 * blending it in would drag the previous resolution's edges into the new
	 * one, so adopt the current mask as the new baseline instead. */
	if (!history->data || history->width != current->width || history->height != current->height) {
		if (!background_mask_resize(history, current->width, current->height))
			return false;
		memcpy(history->data, current->data, count);
		return true;
	}

	if (factor >= 1.0f) {
		memcpy(history->data, current->data, count);
		return true;
	}
	if (factor < 0.0f)
		factor = 0.0f;

	for (size_t i = 0; i < count; i++) {
		const float blended = (float)current->data[i] * factor + (float)history->data[i] * (1.0f - factor);
		const uint8_t value = clamp_u8((int)(blended + 0.5f));
		current->data[i] = value;
		history->data[i] = value;
	}

	return true;
}

/* Samples at most this many background pixels when estimating the wall colour.
 * A full-frame histogram is wasted work for a colour that barely moves between
 * frames, so the frame is strided down to roughly this budget. */
#define BACKGROUND_COLOUR_SAMPLES 4096

/* Below this share of the frame the "background" pixels are more likely to be
 * matte failure than actual wall, so the estimate is refused. */
#define BACKGROUND_COLOUR_MIN_PIXELS 256

/* Only pixels the matte is confident are background feed the estimate; edge
 * pixels are exactly the contaminated ones we are trying to correct. */
#define BACKGROUND_COLOUR_MAX_ALPHA 8

static uint8_t median_of(uint8_t *values, size_t count)
{
	/* Counting sort: the values are bytes and the arrays are small, so this
	 * beats a comparison sort and avoids qsort's callback overhead. */
	uint32_t histogram[256] = {0};

	for (size_t i = 0; i < count; i++)
		histogram[values[i]]++;

	const size_t target = count / 2;
	size_t seen = 0;

	for (int value = 0; value < 256; value++) {
		seen += histogram[value];
		if (seen > target)
			return (uint8_t)value;
	}

	return 0;
}

struct background_colour background_mask_estimate_colour(const struct background_mask *mask, const uint8_t *pixels,
							 uint32_t width, uint32_t height, size_t stride)
{
	struct background_colour colour = {{0.0f, 0.0f, 0.0f}, false};

	if (!mask || !mask->data || !mask->width || !mask->height || !pixels || !width || !height || stride < 3)
		return colour;

	static uint8_t samples[3][BACKGROUND_COLOUR_SAMPLES];
	size_t count = 0;

	/* Stride so a 1080p frame and a 480p frame both cost about the same. */
	const size_t total = (size_t)width * height;
	uint32_t step = (uint32_t)(total / BACKGROUND_COLOUR_SAMPLES);
	if (step < 1)
		step = 1;

	for (size_t i = 0; i < total && count < BACKGROUND_COLOUR_SAMPLES; i += step) {
		const uint32_t x = (uint32_t)(i % width);
		const uint32_t y = (uint32_t)(i / width);

		/* Nearest-neighbour: the mask is at model resolution. */
		const uint32_t mx = x * mask->width / width;
		const uint32_t my = y * mask->height / height;

		if (mask->data[(size_t)my * mask->width + mx] > BACKGROUND_COLOUR_MAX_ALPHA)
			continue;

		const uint8_t *pixel = pixels + i * stride;
		samples[0][count] = pixel[0];
		samples[1][count] = pixel[1];
		samples[2][count] = pixel[2];
		count++;
	}

	if (count < BACKGROUND_COLOUR_MIN_PIXELS)
		return colour;

	for (int channel = 0; channel < 3; channel++)
		colour.rgb[channel] = (float)median_of(samples[channel], count);

	colour.valid = true;
	return colour;
}

/* Shared driver for erode/dilate: both are separable min/max filters, differing
 * only in which extreme they keep. */
static bool background_mask_morphology(struct background_mask *mask, struct background_mask *scratch, int radius,
				       bool dilate)
{
	if (!mask || !scratch || !mask->data || !mask->width || !mask->height)
		return false;
	if (radius <= 0)
		return true;
	if (!background_mask_resize(scratch, mask->width, mask->height))
		return false;

	const int width = (int)mask->width;
	const int height = (int)mask->height;

	for (int y = 0; y < height; y++) {
		const uint8_t *row = mask->data + (size_t)y * width;
		uint8_t *out = scratch->data + (size_t)y * width;

		for (int x = 0; x < width; x++) {
			int begin = x - radius;
			int end = x + radius;
			if (begin < 0)
				begin = 0;
			if (end >= width)
				end = width - 1;

			uint8_t value = row[begin];
			for (int sample = begin + 1; sample <= end; sample++) {
				const uint8_t candidate = row[sample];
				if (dilate ? candidate > value : candidate < value)
					value = candidate;
			}
			out[x] = value;
		}
	}

	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			int begin = y - radius;
			int end = y + radius;
			if (begin < 0)
				begin = 0;
			if (end >= height)
				end = height - 1;

			uint8_t value = scratch->data[(size_t)begin * width + x];
			for (int sample = begin + 1; sample <= end; sample++) {
				const uint8_t candidate = scratch->data[(size_t)sample * width + x];
				if (dilate ? candidate > value : candidate < value)
					value = candidate;
			}
			mask->data[(size_t)y * width + x] = value;
		}
	}

	return true;
}

bool background_mask_erode(struct background_mask *mask, struct background_mask *scratch, int radius)
{
	return background_mask_morphology(mask, scratch, radius, false);
}

bool background_mask_dilate(struct background_mask *mask, struct background_mask *scratch, int radius)
{
	return background_mask_morphology(mask, scratch, radius, true);
}

bool background_mask_feather(struct background_mask *mask, struct background_mask *scratch, int radius)
{
	if (!mask || !scratch || !mask->data || !mask->width || !mask->height)
		return false;
	if (radius <= 0)
		return true;
	if (!background_mask_resize(scratch, mask->width, mask->height))
		return false;

	const int width = (int)mask->width;
	const int height = (int)mask->height;

	for (int y = 0; y < height; y++) {
		const uint8_t *row = mask->data + (size_t)y * width;
		uint8_t *out = scratch->data + (size_t)y * width;

		for (int x = 0; x < width; x++) {
			int begin = x - radius;
			int end = x + radius;
			if (begin < 0)
				begin = 0;
			if (end >= width)
				end = width - 1;

			uint32_t total = 0;
			for (int sample = begin; sample <= end; sample++)
				total += row[sample];

			out[x] = (uint8_t)(total / (uint32_t)(end - begin + 1));
		}
	}

	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			int begin = y - radius;
			int end = y + radius;
			if (begin < 0)
				begin = 0;
			if (end >= height)
				end = height - 1;

			uint32_t total = 0;
			for (int sample = begin; sample <= end; sample++)
				total += scratch->data[(size_t)sample * width + x];

			mask->data[(size_t)y * width + x] = (uint8_t)(total / (uint32_t)(end - begin + 1));
		}
	}

	return true;
}
