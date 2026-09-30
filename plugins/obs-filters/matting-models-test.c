#include "matting-models.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void test_modnet_spec(void)
{
	const struct matting_model_info *info = matting_model_get(MATTING_MODEL_MODNET);
	assert(info != NULL);

	/* Stateless: one image input, no recurrent state, no ratio. */
	assert(!info->recurrent);
	assert(info->spec.state_count == 0);
	assert(info->spec.ratio_input == NULL);

	/* The matte output is resolved by shape because MODNet exports do not
	 * agree on a name for it. */
	assert(info->spec.matte_output == NULL);

	/* Normalisation to [-1, 1]: a mid-grey pixel must land near zero, and
	 * the endpoints at -1 and +1. */
	assert(((0.0f - info->mean) * info->scale) < -0.99f);
	assert(((255.0f - info->mean) * info->scale) > 0.99f);

	assert(info->spec.width == info->size && info->spec.height == info->size);
	assert(strcmp(info->licence, "Apache-2.0") == 0);
}

static void test_rvm_spec(void)
{
	const struct matting_model_info *info = matting_model_get(MATTING_MODEL_RVM);
	assert(info != NULL);

	/* Recurrent: four state pairs plus the scalar ratio input. */
	assert(info->recurrent);
	assert(info->spec.state_count == 4);
	assert(info->spec.state_count <= MATTING_ORT_MAX_STATES);
	assert(info->spec.ratio_input != NULL);
	assert(strcmp(info->spec.ratio_input, "downsample_ratio") == 0);
	assert(info->spec.ratio_value > 0.0f);

	/* State names must pair up in order, matching RVM's documented
	 * rXi -> rXo recycling. */
	for (size_t i = 0; i < info->spec.state_count; i++) {
		assert(info->spec.state_inputs[i] != NULL);
		assert(info->spec.state_outputs[i] != NULL);
		/* r1i/r1o differ only in the final character. */
		assert(strlen(info->spec.state_inputs[i]) == 3);
		assert(strncmp(info->spec.state_inputs[i], info->spec.state_outputs[i], 2) == 0);
		assert(info->spec.state_inputs[i][2] == 'i');
		assert(info->spec.state_outputs[i][2] == 'o');
	}

	/* RVM names its alpha output, so it is selected explicitly rather than
	 * by shape - "fgr" has the same spatial size and would otherwise be a
	 * candidate. */
	assert(info->spec.matte_output != NULL);
	assert(strcmp(info->spec.matte_output, "pha") == 0);

	/* Normalisation to [0, 1]. */
	assert(((0.0f - info->mean) * info->scale) == 0.0f);
	assert(((255.0f - info->mean) * info->scale) > 0.99f);
	assert(((255.0f - info->mean) * info->scale) <= 1.0f);

	/* GPL-3.0 is recorded because it constrains redistribution. */
	assert(strcmp(info->licence, "GPL-3.0") == 0);
}

static void test_auto_has_no_spec(void)
{
	/* AUTO is a UI choice, not a real family, so it must not resolve to a
	 * descriptor that could be used to build a session. */
	assert(matting_model_get(MATTING_MODEL_AUTO) == NULL);
}

static void test_detection(void)
{
	/* Conventional release filenames. */
	assert(matting_model_detect("rvm_mobilenetv3_fp32.onnx") == MATTING_MODEL_RVM);
	assert(matting_model_detect("rvm_resnet50_fp16.onnx") == MATTING_MODEL_RVM);
	assert(matting_model_detect("modnet_photographic_portrait_matting.onnx") == MATTING_MODEL_MODNET);

	/* Case-insensitive, since users rename files. */
	assert(matting_model_detect("RVM_MobileNetV3.onnx") == MATTING_MODEL_RVM);
	assert(matting_model_detect("MODNet.onnx") == MATTING_MODEL_MODNET);

	/* Unrecognised names fall back to AUTO so the filter can warn and ask
	 * for an explicit choice instead of guessing a tensor contract. */
	assert(matting_model_detect("my_matting_model.onnx") == MATTING_MODEL_AUTO);
	assert(matting_model_detect("") == MATTING_MODEL_AUTO);
	assert(matting_model_detect(NULL) == MATTING_MODEL_AUTO);
}

/* Mirrors the spatial-dimension check in matting_ort_find_matte_by_shape.
 * Kept in sync by hand: the real function needs a live ORT session, but the
 * arithmetic is where the bug was, and it is worth pinning.
 *
 * The reference MODNet export declares its output as
 * [batch_size, 1, height, width] - every dimension symbolic, reported by ORT
 * as -1.  An earlier version multiplied those into zero and so rejected the
 * model outright. */
static bool spatial_matches(const int64_t *shape, size_t rank, int64_t expected)
{
	bool spatial_ok = true;
	int64_t spatial = 1;

	for (size_t d = rank - 2; d < rank; d++) {
		if (shape[d] <= 0)
			spatial = -1;
		else if (spatial > 0)
			spatial *= shape[d];
	}
	if (spatial > 0)
		spatial_ok = spatial == expected;

	return spatial_ok;
}

static void test_dynamic_shape_matte_is_accepted(void)
{
	const int64_t expected = 256 * 256;

	/* Fully dynamic, as the reference MODNet export emits. */
	const int64_t dynamic[4] = {-1, 1, -1, -1};
	assert(spatial_matches(dynamic, 4, expected));

	/* Fixed and correct. */
	const int64_t fixed_ok[4] = {1, 1, 256, 256};
	assert(spatial_matches(fixed_ok, 4, expected));

	/* Fixed but a different resolution: must not match, otherwise a
	 * mismatched export would be accepted and produce a garbled matte. */
	const int64_t fixed_bad[4] = {1, 1, 512, 512};
	assert(!spatial_matches(fixed_bad, 4, expected));

	/* Partially dynamic still matches, since the fed dimension decides. */
	const int64_t partial[4] = {1, 1, 256, -1};
	assert(spatial_matches(partial, 4, expected));
}

static void test_sizes_are_usable(void)
{
	/* Both models must declare a positive square size; the filter uses it
	 * to size the GPU readback and the inference buffers. */
	const enum matting_model_family families[] = {MATTING_MODEL_MODNET, MATTING_MODEL_RVM};

	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
		const struct matting_model_info *info = matting_model_get(families[i]);
		assert(info != NULL);
		assert(info->size > 0);
		assert(info->size <= 1024);
		assert(info->scale > 0.0f);
	}
}

int main(void)
{
	test_modnet_spec();
	test_rvm_spec();
	test_auto_has_no_spec();
	test_detection();
	test_dynamic_shape_matte_is_accepted();
	test_sizes_are_usable();
	return 0;
}
