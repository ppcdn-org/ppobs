#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* ONNX Runtime wrapper for portrait matting models.
 *
 * Separate from face-swap-ort.c because the two have incompatible contracts:
 * the face swap wrapper is capped at two inputs and releases every output
 * before returning, whereas recurrent matting models (RVM) take six inputs and
 * require four of their outputs to be fed back in as inputs on the next frame.
 * Carrying that state means the session has to own OrtValues across calls,
 * which the face swap wrapper deliberately does not do.
 *
 * Two model families are supported through one interface:
 *
 *   MODNet   1 input  (src), picks the matte out of its outputs by shape.
 *   RVM      6 inputs (src, r1i..r4i, downsample_ratio), 6 outputs
 *            (fgr, pha, r1o..r4o); r*o are recycled into r*i.
 *
 * Inputs and outputs are addressed by name rather than index: ONNX does not
 * guarantee ordering, and RVM's exports in particular are only documented by
 * name.
 */

#define MATTING_ORT_MAX_RANK 5
#define MATTING_ORT_MAX_STATES 4

struct matting_ort {
	void *module;
	const void *api;
	void *env;
	const char *version;
	bool sdk_available;
};

/* A recurrent state slot: the OrtValue produced as an output on the previous
 * frame, to be passed as an input on this one. */
struct matting_ort_state {
	char *input_name;
	char *output_name;
	/* Owned OrtValue carried between frames; NULL until the first
	 * inference completes. */
	void *value;
};

struct matting_ort_session {
	void *session;

	/* Primary image input. */
	char *src_name;
	int64_t src_shape[4];

	/* Optional scalar FP32 input (RVM's downsample_ratio); NULL if the
	 * model does not take one. */
	char *ratio_name;
	float ratio_value;

	/* Name of the output to read the alpha matte from.  For RVM this is
	 * "pha"; for MODNet it is resolved by shape at load time. */
	char *matte_name;

	struct matting_ort_state states[MATTING_ORT_MAX_STATES];
	size_t state_count;

	size_t input_count;
	size_t output_count;
	char **output_names;
};

/* Describes what to expect of a model so the session can be validated and
 * driven without the caller knowing which family it belongs to. */
struct matting_ort_spec {
	/* Expected NCHW image input. */
	int64_t width;
	int64_t height;

	/* Recurrent state input/output name pairs, empty for stateless
	 * models. */
	const char *const *state_inputs;
	const char *const *state_outputs;
	size_t state_count;

	/* Name of the scalar downsample-ratio input, or NULL. */
	const char *ratio_input;
	float ratio_value;

	/* Preferred matte output name; when NULL, or absent from the model,
	 * the output whose element count equals width * height is used. */
	const char *matte_output;
};

bool matting_ort_load(struct matting_ort *ort);
void matting_ort_unload(struct matting_ort *ort);

/* Opens a CPU session and resolves the spec against the model's real input and
 * output names.  Fails rather than guessing if a required name is missing. */
bool matting_ort_create_session(struct matting_ort *ort, const wchar_t *model_path,
				const struct matting_ort_spec *spec, struct matting_ort_session *out);

void matting_ort_release_session(struct matting_ort *ort, struct matting_ort_session *session);

/* Runs one frame.  `src` is planar CHW float data matching the spec's
 * dimensions; `matte` receives width * height floats.
 *
 * Recurrent state is advanced internally: the state outputs of this call are
 * retained for the next one.  Frames must therefore be submitted in order, and
 * matting_ort_reset_states() must be called whenever continuity breaks. */
bool matting_ort_run(struct matting_ort *ort, struct matting_ort_session *session, const float *src, size_t src_count,
		     float *matte, size_t matte_count);

/* Drops carried recurrent state, so the next frame starts from a clean memory.
 * Needed after a source change or a gap in submitted frames, where stale state
 * would otherwise bleed a silhouette from the previous scene into the new one.
 */
void matting_ort_reset_states(struct matting_ort *ort, struct matting_ort_session *session);
