/* ONNX Runtime wrapper for portrait matting models.  See matting-ort.h for the
 * contract and why this does not reuse face-swap-ort.c. */

#include "matting-ort.h"

#include <obs-module.h>
#include <util/platform.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(FACE_SWAP_HAS_ORT)
#include <onnxruntime_c_api.h>
#endif

/* OrtGetApiBase is ONNX Runtime's stable C ABI entry point.  Keep this small
 * prefix local so merely detecting the runtime never couples OBS to an ORT
 * import library. */
struct matting_ort_api_base {
	const void *(*get_api)(uint32_t version);
	const char *(*get_version_string)(void);
};

typedef const struct matting_ort_api_base *(*matting_ort_get_api_base_t)(void);

bool matting_ort_load(struct matting_ort *ort)
{
	if (!ort)
		return false;

	ort->module = os_dlopen("onnxruntime.dll");
	if (!ort->module)
		return false;

	matting_ort_get_api_base_t get_api_base =
		(matting_ort_get_api_base_t)os_dlsym(ort->module, "OrtGetApiBase");
	if (!get_api_base)
		goto fail;

	const struct matting_ort_api_base *base = get_api_base();
	if (!base || !base->get_api || !base->get_version_string)
		goto fail;

	ort->version = base->get_version_string();
	if (!ort->version || !*ort->version)
		goto fail;

#if defined(FACE_SWAP_HAS_ORT)
	ort->api = base->get_api(ORT_API_VERSION);
	if (!ort->api)
		goto fail;

	const OrtApi *api = ort->api;
	OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "obs-matting", (OrtEnv **)&ort->env);
	if (status) {
		blog(LOG_WARNING, "[matting] ONNX Runtime environment creation failed: %s",
		     api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		goto fail;
	}
	ort->sdk_available = true;
#else
	ort->api = NULL;
	ort->sdk_available = false;
#endif
	return true;

fail:
	matting_ort_unload(ort);
	return false;
}

void matting_ort_unload(struct matting_ort *ort)
{
	if (!ort)
		return;

#if defined(FACE_SWAP_HAS_ORT)
	if (ort->api && ort->env)
		((const OrtApi *)ort->api)->ReleaseEnv((OrtEnv *)ort->env);
#endif

	if (ort->module)
		os_dlclose(ort->module);

	ort->module = NULL;
	ort->api = NULL;
	ort->env = NULL;
	ort->version = NULL;
	ort->sdk_available = false;
}

#if defined(FACE_SWAP_HAS_ORT)

/* Collects every input/output name so the spec can be matched by name.  Caller
 * owns the returned array and its strings. */
static char **matting_ort_collect_names(const OrtApi *api, OrtSession *session, OrtAllocator *allocator, bool inputs,
					size_t count)
{
	char **names = bzalloc(count * sizeof(*names));
	if (!names)
		return NULL;

	for (size_t i = 0; i < count; i++) {
		char *name = NULL;
		OrtStatus *status = inputs ? api->SessionGetInputName(session, i, allocator, &name)
					   : api->SessionGetOutputName(session, i, allocator, &name);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		names[i] = bstrdup(name);
		api->AllocatorFree(allocator, name);
		if (!names[i])
			goto fail;
	}

	return names;

fail:
	for (size_t i = 0; i < count; i++)
		bfree(names[i]);
	bfree(names);
	return NULL;
}

/* Picks how many cores inference may use.
 *
 * Matting is the dominant cost in this filter and parallelises well: measured
 * on a 10-core CPU, going from one thread to four cuts latency by roughly 3x,
 * with little gained past six and a slight regression at eight as scheduling
 * overhead and contention take over.
 *
 * The cap matters as much as the count.  OBS is rendering, encoding and
 * streaming on the same machine, so taking every core would win a benchmark and
 * lose frames in the preview.  Half the cores, bounded at six, keeps the bulk of
 * the speedup while leaving the rest of the pipeline room to run. */
static int matting_ort_thread_count(void)
{
	const int cores = (int)os_get_physical_cores();

	if (cores <= 1)
		return 1;

	int threads = cores / 2;
	if (threads < 2)
		threads = 2;
	if (threads > 6)
		threads = 6;

	return threads;
}

static bool matting_ort_has_name(char *const *names, size_t count, const char *needle)
{
	for (size_t i = 0; i < count; i++) {
		if (names[i] && strcmp(names[i], needle) == 0)
			return true;
	}
	return false;
}

/* Finds the output that looks like a matte: rank 3 or 4 with a single channel
 * and the expected spatial size.  Used for MODNet, whose export names vary. */
static char *matting_ort_find_matte_by_shape(const OrtApi *api, OrtSession *session, char *const *output_names,
					     size_t output_count, int64_t width, int64_t height)
{
	const int64_t expected = width * height;

	for (size_t i = 0; i < output_count; i++) {
		OrtTypeInfo *type_info = NULL;
		const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
		size_t rank = 0;
		int64_t shape[MATTING_ORT_MAX_RANK] = {0};
		bool match = false;

		OrtStatus *status = api->SessionGetOutputTypeInfo(session, i, &type_info);
		if (!status)
			status = api->CastTypeInfoToTensorInfo(type_info, &tensor_info);
		if (!status && tensor_info)
			status = api->GetDimensionsCount(tensor_info, &rank);
		if (!status && tensor_info && rank && rank <= MATTING_ORT_MAX_RANK)
			status = api->GetDimensions(tensor_info, shape, rank);

		if (!status && rank >= 3 && rank <= 4) {
			/* Exports commonly leave the spatial dims symbolic
			 * ("height"/"width"), which ORT reports as -1.  Such a
			 * dimension accepts whatever we feed it, so treat it as
			 * matching rather than requiring a literal size - the
			 * reference MODNet export is fully dynamic and would
			 * otherwise never be recognised. */
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

			const int64_t channels = rank == 4 ? shape[1] : shape[0];
			match = spatial_ok && (channels == 1 || channels <= 0);
		}

		if (type_info)
			api->ReleaseTypeInfo(type_info);
		if (status) {
			api->ReleaseStatus(status);
			continue;
		}

		if (match)
			return bstrdup(output_names[i]);
	}

	return NULL;
}

#endif /* FACE_SWAP_HAS_ORT */

bool matting_ort_create_session(struct matting_ort *ort, const wchar_t *model_path,
				const struct matting_ort_spec *spec, struct matting_ort_session *out)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->sdk_available || !ort->api || !ort->env || !model_path || !*model_path || !spec || !out)
		return false;
	if (spec->state_count > MATTING_ORT_MAX_STATES)
		return false;
	if (spec->width <= 0 || spec->height <= 0)
		return false;

	memset(out, 0, sizeof(*out));

	const OrtApi *api = ort->api;
	OrtSessionOptions *options = NULL;
	OrtSession *session = NULL;
	OrtAllocator *allocator = NULL;
	char **input_names = NULL;
	char **output_names = NULL;
	size_t input_count = 0;
	size_t output_count = 0;

	OrtStatus *status = api->CreateSessionOptions(&options);
	if (!status)
		status = api->SetIntraOpNumThreads(options, matting_ort_thread_count());
	if (!status)
		status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
	if (!status)
		status = api->CreateSession((const OrtEnv *)ort->env, model_path, options, &session);

	if (options)
		api->ReleaseSessionOptions(options);
	if (status) {
		blog(LOG_WARNING, "[matting] session creation failed: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		return false;
	}

	status = api->GetAllocatorWithDefaultOptions(&allocator);
	if (!status)
		status = api->SessionGetInputCount(session, &input_count);
	if (!status)
		status = api->SessionGetOutputCount(session, &output_count);
	if (status) {
		blog(LOG_WARNING, "[matting] could not inspect model io: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		goto fail;
	}

	if (!input_count || !output_count || input_count > 16 || output_count > 16) {
		blog(LOG_WARNING, "[matting] unsupported model io counts (%zu inputs, %zu outputs)", input_count,
		     output_count);
		goto fail;
	}

	/* The model must have room for the image, every recurrent state, and
	 * the optional ratio.  Checked before name lookup so a completely
	 * unrelated model fails with a clear message. */
	const size_t required_inputs = 1 + spec->state_count + (spec->ratio_input ? 1 : 0);
	if (input_count != required_inputs) {
		blog(LOG_WARNING, "[matting] expected %zu inputs, model has %zu", required_inputs, input_count);
		goto fail;
	}

	input_names = matting_ort_collect_names(api, session, allocator, true, input_count);
	output_names = matting_ort_collect_names(api, session, allocator, false, output_count);
	if (!input_names || !output_names)
		goto fail;

	/* The image input is whichever input is not a declared state or the
	 * ratio; resolving it by elimination avoids depending on the export
	 * having named it "src". */
	for (size_t i = 0; i < input_count; i++) {
		bool reserved = spec->ratio_input && strcmp(input_names[i], spec->ratio_input) == 0;

		for (size_t s = 0; !reserved && s < spec->state_count; s++)
			reserved = strcmp(input_names[i], spec->state_inputs[s]) == 0;

		if (!reserved) {
			if (out->src_name) {
				blog(LOG_WARNING, "[matting] model has more than one image input");
				goto fail;
			}
			out->src_name = bstrdup(input_names[i]);
		}
	}

	if (!out->src_name) {
		blog(LOG_WARNING, "[matting] could not identify the image input");
		goto fail;
	}

	for (size_t s = 0; s < spec->state_count; s++) {
		if (!matting_ort_has_name(input_names, input_count, spec->state_inputs[s]) ||
		    !matting_ort_has_name(output_names, output_count, spec->state_outputs[s])) {
			blog(LOG_WARNING, "[matting] model is missing recurrent state '%s'/'%s'",
			     spec->state_inputs[s], spec->state_outputs[s]);
			goto fail;
		}

		out->states[s].input_name = bstrdup(spec->state_inputs[s]);
		out->states[s].output_name = bstrdup(spec->state_outputs[s]);
		if (!out->states[s].input_name || !out->states[s].output_name)
			goto fail;
	}
	out->state_count = spec->state_count;

	if (spec->ratio_input) {
		if (!matting_ort_has_name(input_names, input_count, spec->ratio_input)) {
			blog(LOG_WARNING, "[matting] model is missing input '%s'", spec->ratio_input);
			goto fail;
		}
		out->ratio_name = bstrdup(spec->ratio_input);
		if (!out->ratio_name)
			goto fail;
		out->ratio_value = spec->ratio_value > 0.0f ? spec->ratio_value : 1.0f;
	}

	if (spec->matte_output && matting_ort_has_name(output_names, output_count, spec->matte_output)) {
		out->matte_name = bstrdup(spec->matte_output);
	} else {
		out->matte_name = matting_ort_find_matte_by_shape(api, session, output_names, output_count,
								  spec->width, spec->height);
	}

	if (!out->matte_name) {
		blog(LOG_WARNING, "[matting] could not identify a %" PRId64 "x%" PRId64 " matte output", spec->width,
		     spec->height);
		goto fail;
	}

	out->session = session;
	out->input_count = input_count;
	out->output_count = output_count;
	out->output_names = output_names;
	out->src_shape[0] = 1;
	out->src_shape[1] = 3;
	out->src_shape[2] = spec->height;
	out->src_shape[3] = spec->width;

	for (size_t i = 0; i < input_count; i++)
		bfree(input_names[i]);
	bfree(input_names);
	return true;

fail:
	if (input_names) {
		for (size_t i = 0; i < input_count; i++)
			bfree(input_names[i]);
		bfree(input_names);
	}
	if (output_names) {
		for (size_t i = 0; i < output_count; i++)
			bfree(output_names[i]);
		bfree(output_names);
	}
	bfree(out->src_name);
	bfree(out->ratio_name);
	bfree(out->matte_name);
	for (size_t s = 0; s < MATTING_ORT_MAX_STATES; s++) {
		bfree(out->states[s].input_name);
		bfree(out->states[s].output_name);
	}
	api->ReleaseSession(session);
	memset(out, 0, sizeof(*out));
	return false;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(model_path);
	UNUSED_PARAMETER(spec);
	UNUSED_PARAMETER(out);
	return false;
#endif
}

void matting_ort_reset_states(struct matting_ort *ort, struct matting_ort_session *session)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->api || !session)
		return;

	const OrtApi *api = ort->api;
	for (size_t s = 0; s < session->state_count; s++) {
		if (session->states[s].value) {
			api->ReleaseValue((OrtValue *)session->states[s].value);
			session->states[s].value = NULL;
		}
	}
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(session);
#endif
}

void matting_ort_release_session(struct matting_ort *ort, struct matting_ort_session *session)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!session)
		return;

	matting_ort_reset_states(ort, session);

	if (ort && ort->api && session->session)
		((const OrtApi *)ort->api)->ReleaseSession((OrtSession *)session->session);

	bfree(session->src_name);
	bfree(session->ratio_name);
	bfree(session->matte_name);
	for (size_t s = 0; s < MATTING_ORT_MAX_STATES; s++) {
		bfree(session->states[s].input_name);
		bfree(session->states[s].output_name);
	}
	if (session->output_names) {
		for (size_t i = 0; i < session->output_count; i++)
			bfree(session->output_names[i]);
		bfree(session->output_names);
	}
	memset(session, 0, sizeof(*session));
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(session);
#endif
}

bool matting_ort_run(struct matting_ort *ort, struct matting_ort_session *session, const float *src, size_t src_count,
		     float *matte, size_t matte_count)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->api || !session || !session->session || !src || !matte || !matte_count)
		return false;

	const size_t expected_src = (size_t)session->src_shape[1] * (size_t)session->src_shape[2] *
				    (size_t)session->src_shape[3];
	if (src_count != expected_src)
		return false;
	if (matte_count != (size_t)session->src_shape[2] * (size_t)session->src_shape[3])
		return false;

	const OrtApi *api = ort->api;
	OrtMemoryInfo *memory_info = NULL;
	OrtValue *src_value = NULL;
	OrtValue *ratio_value = NULL;
	/* Zero-sized placeholders for the first frame, matching RVM's
	 * documented [1,1,1,1] initial recurrent states. */
	OrtValue *initial_states[MATTING_ORT_MAX_STATES] = {0};
	float initial_state_data[MATTING_ORT_MAX_STATES] = {0};

	const char *input_names[1 + MATTING_ORT_MAX_STATES + 1] = {0};
	const OrtValue *input_values[1 + MATTING_ORT_MAX_STATES + 1] = {0};
	size_t input_index = 0;

	OrtValue **outputs = bzalloc(session->output_count * sizeof(*outputs));
	if (!outputs)
		return false;

	bool success = false;
	OrtStatus *status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info);

	if (!status) {
		/* Cast away const: ORT does not modify input tensor data, but
		 * its C API takes a mutable pointer. */
		status = api->CreateTensorWithDataAsOrtValue(memory_info, (void *)src, src_count * sizeof(float),
							     session->src_shape, 4,
							     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &src_value);
	}

	if (!status) {
		input_names[input_index] = session->src_name;
		input_values[input_index] = src_value;
		input_index++;
	}

	for (size_t s = 0; !status && s < session->state_count; s++) {
		const OrtValue *value = session->states[s].value;

		if (!value) {
			const int64_t shape[4] = {1, 1, 1, 1};
			status = api->CreateTensorWithDataAsOrtValue(memory_info, &initial_state_data[s], sizeof(float),
								     shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
								     &initial_states[s]);
			value = initial_states[s];
		}

		if (!status) {
			input_names[input_index] = session->states[s].input_name;
			input_values[input_index] = value;
			input_index++;
		}
	}

	float ratio = session->ratio_value;
	if (!status && session->ratio_name) {
		const int64_t shape[1] = {1};
		status = api->CreateTensorWithDataAsOrtValue(memory_info, &ratio, sizeof(ratio), shape, 1,
							     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ratio_value);
		if (!status) {
			input_names[input_index] = session->ratio_name;
			input_values[input_index] = ratio_value;
			input_index++;
		}
	}

	if (!status) {
		status = api->Run((OrtSession *)session->session, NULL, input_names, input_values, input_index,
				  (const char *const *)session->output_names, session->output_count, outputs);
	}

	/* Copy the matte out before anything is released. */
	if (!status) {
		for (size_t i = 0; i < session->output_count; i++) {
			if (strcmp(session->output_names[i], session->matte_name) != 0)
				continue;

			OrtTensorTypeAndShapeInfo *shape_info = NULL;
			size_t element_count = 0;
			enum ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
			const float *data = NULL;

			OrtStatus *matte_status = api->GetTensorTypeAndShape(outputs[i], &shape_info);
			if (!matte_status)
				matte_status = api->GetTensorElementType(shape_info, &type);
			if (!matte_status)
				matte_status = api->GetTensorShapeElementCount(shape_info, &element_count);
			if (!matte_status)
				matte_status = api->GetTensorMutableData(outputs[i], (void **)&data);

			if (!matte_status && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && data &&
			    element_count == matte_count) {
				memcpy(matte, data, matte_count * sizeof(float));
				success = true;
			}

			if (shape_info)
				api->ReleaseTensorTypeAndShapeInfo(shape_info);
			if (matte_status)
				api->ReleaseStatus(matte_status);
			break;
		}
	}

	/* Take the new recurrent states out of the output array so they survive
	 * the cleanup below and can be fed back in on the next frame.  Only
	 * done when the matte was read successfully: keeping state from a run
	 * whose output could not be used would desynchronise the memory from
	 * the frames actually rendered. */
	if (success) {
		for (size_t s = 0; s < session->state_count; s++) {
			for (size_t i = 0; i < session->output_count; i++) {
				if (!outputs[i] || strcmp(session->output_names[i], session->states[s].output_name) != 0)
					continue;

				if (session->states[s].value)
					api->ReleaseValue((OrtValue *)session->states[s].value);
				session->states[s].value = outputs[i];
				outputs[i] = NULL;
				break;
			}
		}
	}

	if (status) {
		blog(LOG_WARNING, "[matting] inference failed: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
	}

	for (size_t i = 0; i < session->output_count; i++) {
		if (outputs[i])
			api->ReleaseValue(outputs[i]);
	}
	for (size_t s = 0; s < MATTING_ORT_MAX_STATES; s++) {
		if (initial_states[s])
			api->ReleaseValue(initial_states[s]);
	}
	if (ratio_value)
		api->ReleaseValue(ratio_value);
	if (src_value)
		api->ReleaseValue(src_value);
	if (memory_info)
		api->ReleaseMemoryInfo(memory_info);
	bfree(outputs);

	return success;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(src);
	UNUSED_PARAMETER(src_count);
	UNUSED_PARAMETER(matte);
	UNUSED_PARAMETER(matte_count);
	return false;
#endif
}
