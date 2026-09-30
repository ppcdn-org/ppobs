#pragma once

#include "matting-ort.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Known portrait matting model families.
 *
 * The two supported families differ in more than their tensor shapes, so the
 * differences are captured as data rather than branched on at each call site:
 *
 *   MODNet  Apache-2.0.  Stateless, one input, (x - 127.5) / 127.5.
 *   RVM     GPL-3.0.     Recurrent, six inputs, x / 255, and it must see
 *                        frames in order to keep its temporal memory useful.
 *
 * Licences are recorded here because they are a property of the model the
 * operator supplies, and RVM being GPL-3.0 constrains redistribution.  Neither
 * model is bundled; both are loaded from the user's config directory.
 */

enum matting_model_family {
	MATTING_MODEL_AUTO,
	MATTING_MODEL_MODNET,
	MATTING_MODEL_RVM,
};

struct matting_model_info {
	enum matting_model_family family;
	const char *label;
	const char *licence;

	/* Square input resolution the ONNX export is pinned to. */
	int64_t size;

	/* Per-channel normalisation applied to 0-255 pixels:
	 * value = (pixel - mean) * scale */
	float mean;
	float scale;

	/* True when the model carries recurrent state, and therefore needs
	 * frames submitted in order and a reset when continuity breaks. */
	bool recurrent;

	struct matting_ort_spec spec;
};

/* Returns the descriptor for a family, or NULL for MATTING_MODEL_AUTO. */
const struct matting_model_info *matting_model_get(enum matting_model_family family);

/* Guesses the family from a model filename, so the default "Automatic" setting
 * works for the conventional RVM and MODNet export names without the user
 * having to pick.  Returns MATTING_MODEL_AUTO when the name is unrecognised.
 */
enum matting_model_family matting_model_detect(const char *filename);
