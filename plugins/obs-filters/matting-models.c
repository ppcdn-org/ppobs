#include "matting-models.h"

#include <util/dstr.h>

#include <string.h>

/* RVM's ONNX export names, from its inference documentation.  The recurrent
 * outputs r1o..r4o feed back into r1i..r4i on the next frame. */
static const char *const RVM_STATE_INPUTS[] = {"r1i", "r2i", "r3i", "r4i"};
static const char *const RVM_STATE_OUTPUTS[] = {"r1o", "r2o", "r3o", "r4o"};

/* Both families export with symbolic spatial dimensions, so the size below is
 * our choice of operating point rather than something the model dictates.
 *
 * 256 is that operating point for both: the matte is upscaled to video
 * resolution on the GPU regardless, and 512 would cost four times the input
 * tensor for detail that the feathering and upscale largely discard.  Raising
 * this is the first thing to try if hair edges prove too coarse in practice,
 * and is safe to change - the session validates against the model rather than
 * against this constant. */
#define MODNET_SIZE 256
#define RVM_SIZE 256

/* RVM downsamples internally before its refinement stage; the documented
 * guidance is to keep the downsampled edge between 256 and 512.  The input is
 * already 256, so no further reduction is wanted. */
#define RVM_DOWNSAMPLE_RATIO 1.0f

static const struct matting_model_info MATTING_MODELS[] = {
	{
		.family = MATTING_MODEL_MODNET,
		.label = "MODNet",
		.licence = "Apache-2.0",
		.size = MODNET_SIZE,
		/* MODNet's reference inference normalises to [-1, 1]. */
		.mean = 127.5f,
		.scale = 1.0f / 127.5f,
		.recurrent = false,
		.spec = {
			.width = MODNET_SIZE,
			.height = MODNET_SIZE,
			.state_inputs = NULL,
			.state_outputs = NULL,
			.state_count = 0,
			.ratio_input = NULL,
			.ratio_value = 0.0f,
			/* Resolved by shape: MODNet exports do not agree on a
			 * name for the matte output. */
			.matte_output = NULL,
		},
	},
	{
		.family = MATTING_MODEL_RVM,
		.label = "RobustVideoMatting",
		.licence = "GPL-3.0",
		.size = RVM_SIZE,
		/* RVM takes RGB normalised to [0, 1]. */
		.mean = 0.0f,
		.scale = 1.0f / 255.0f,
		.recurrent = true,
		.spec = {
			.width = RVM_SIZE,
			.height = RVM_SIZE,
			.state_inputs = RVM_STATE_INPUTS,
			.state_outputs = RVM_STATE_OUTPUTS,
			.state_count = 4,
			.ratio_input = "downsample_ratio",
			.ratio_value = RVM_DOWNSAMPLE_RATIO,
			/* RVM's alpha output is named; fgr is the foreground
			 * estimate and is not used here. */
			.matte_output = "pha",
		},
	},
};

#define MATTING_MODEL_COUNT (sizeof(MATTING_MODELS) / sizeof(MATTING_MODELS[0]))

const struct matting_model_info *matting_model_get(enum matting_model_family family)
{
	for (size_t i = 0; i < MATTING_MODEL_COUNT; i++) {
		if (MATTING_MODELS[i].family == family)
			return &MATTING_MODELS[i];
	}

	return NULL;
}

enum matting_model_family matting_model_detect(const char *filename)
{
	if (!filename || !*filename)
		return MATTING_MODEL_AUTO;

	/* Substring match on the conventional release names ("rvm_mobilenetv3
	 * _fp32.onnx", "modnet_photographic_portrait_matting.onnx").  A wrong
	 * guess is recoverable: session creation validates the spec against
	 * the model's real inputs and fails loudly rather than producing a
	 * broken matte. */
	if (astrstri(filename, "rvm") != NULL)
		return MATTING_MODEL_RVM;
	if (astrstri(filename, "modnet") != NULL)
		return MATTING_MODEL_MODNET;

	return MATTING_MODEL_AUTO;
}
