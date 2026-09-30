#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Alpha mask post-processing for the portrait segmentation filter.
 *
 * The segmentation model emits a small single-channel probability map (256x256
 * or similar).  These helpers turn that into the 8-bit mask the GPU samples,
 * and keep it stable across frames: raw per-frame model output flickers badly
 * on hair and shoulder edges, which is very visible on a live stream.
 *
 * Everything here is pure computation on caller-owned buffers so it can be
 * exercised without ONNX Runtime, a GPU, or an OBS source.
 */

struct background_mask {
	uint8_t *data;
	uint32_t width;
	uint32_t height;
	size_t capacity;
};

/* Allocates or reuses the buffer.  Returns false if the size is unusable, in
 * which case the mask is left empty rather than partially sized. */
bool background_mask_resize(struct background_mask *mask, uint32_t width, uint32_t height);
void background_mask_free(struct background_mask *mask);

/* Converts model output in [0, 1] to 8-bit coverage, applying the threshold as
 * the midpoint of a soft ramp.  A hard step would alias badly once the mask is
 * upsampled to video resolution, so `softness` controls the ramp width in
 * probability units; 0 gives a hard cut. */
bool background_mask_from_probabilities(struct background_mask *mask, const float *probabilities, size_t count,
					uint32_t width, uint32_t height, float threshold, float softness);

/* Exponential moving average against the previous mask, in place on `current`.
 * `factor` is the weight of the new frame: 1.0 disables smoothing, lower values
 * keep more history.  Mismatched sizes replace the history instead of blending,
 * so a resolution change does not smear the old resolution into the new one. */
bool background_mask_smooth(struct background_mask *current, struct background_mask *history, float factor);

/* Morphological erode/dilate with a square kernel, used to pull the matte
 * inside the subject (removing background spill) or push it out (recovering
 * clipped hair).  `radius` is in mask pixels; 0 is a no-op.  `scratch` is
 * resized as needed and reused across calls. */
bool background_mask_erode(struct background_mask *mask, struct background_mask *scratch, int radius);
bool background_mask_dilate(struct background_mask *mask, struct background_mask *scratch, int radius);

/* Separable box blur, which softens the matte edge so compositing does not show
 * a cut-out outline.  `radius` is in mask pixels; 0 is a no-op. */
bool background_mask_feather(struct background_mask *mask, struct background_mask *scratch, int radius);

/* Estimated colour of the background the subject was shot against, in 0-255
 * RGB, plus whether the estimate is usable at all. */
struct background_colour {
	float rgb[3];
	bool valid;
};

/* Samples the frame where the matte says "definitely background" and returns
 * the median colour of those pixels.
 *
 * This exists because the footage is not shot on a green screen: the studio
 * wall is a near-white but slightly blue-tinted grey with a vertical gradient
 * (measured at roughly 227,233,240 top to 235,237,243 bottom on AI10-Nina).
 * A hard-coded white would leave a visible cast, so the colour is measured per
 * frame instead.  The median rather than the mean keeps a stray dark pixel
 * that slipped past the matte from dragging the estimate.
 *
 * `pixels` points at the red channel of the first pixel and `stride` is the
 * byte distance to the next one, so both packed 24-bit RGB (3) and the RGBA
 * readback the filter actually captures (4) can be measured without a copy.
 * The mask is sampled with nearest-neighbour, so it may be a different
 * (smaller) size than the image.  Returns an invalid estimate when too little
 * background is visible to be confident, which the caller must treat as "skip
 * unmixing".
 */
struct background_colour background_mask_estimate_colour(const struct background_mask *mask, const uint8_t *pixels,
							 uint32_t width, uint32_t height, size_t stride);

/* The unmixing that uses this colour runs in background_alpha.effect rather
 * than here: the filter hands the frame to OBS untouched and composites on the
 * GPU, so edits to the worker's pixel copy would never reach the output, and
 * the shader gets to correct at full video resolution for free. */
