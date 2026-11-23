#include "face-ort.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <math.h>
#include <string.h>

#if defined(FACE_SWAP_HAS_ORT)
#include <onnxruntime_c_api.h>
#endif

struct face_ort_api_base {
	const void *(*get_api)(uint32_t version);
	const char *(*get_version_string)(void);
};

typedef const struct face_ort_api_base *(*face_ort_get_api_base_t)(void);

#if defined(FACE_SWAP_HAS_ORT)
static const char *const yunet_output_names[12] = {
	"cls_8",  "cls_16",  "cls_32",  "obj_8", "obj_16", "obj_32",
	"bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32",
};

static int face_ort_thread_count(int cap)
{
	const int cores = (int)os_get_physical_cores();

	if (cores <= 1)
		return 1;

	int threads = cores / 2;
	if (threads < 1)
		threads = 1;
	if (threads > cap)
		threads = cap;

	return threads;
}

static bool face_ort_create_session(const OrtApi *api, OrtEnv *env, const wchar_t *path, int threads, OrtSession **out)
{
	OrtSessionOptions *options = NULL;
	OrtStatus *status = api->CreateSessionOptions(&options);

	if (!status)
		status = api->SetIntraOpNumThreads(options, threads);
	if (!status)
		status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
	if (!status)
		status = api->CreateSession(env, path, options, out);

	if (options)
		api->ReleaseSessionOptions(options);

	if (status) {
		blog(LOG_WARNING, "[big head] session creation failed: %s", api->GetErrorMessage(status));
		api->ReleaseStatus(status);
		return false;
	}

	return true;
}

static bool face_ort_output_names_match(const OrtApi *api, OrtSession *session, OrtAllocator *allocator)
{
	size_t output_count = 0;
	OrtStatus *status = api->SessionGetOutputCount(session, &output_count);

	if (status) {
		api->ReleaseStatus(status);
		return false;
	}

	if (output_count != 12)
		return false;

	for (size_t i = 0; i < output_count; i++) {
		char *name = NULL;
		status = api->SessionGetOutputName(session, i, allocator, &name);
		if (status) {
			api->ReleaseStatus(status);
			return false;
		}

		bool found = false;
		for (size_t j = 0; j < 12; j++) {
			if (strcmp(name, yunet_output_names[j]) == 0) {
				found = true;
				break;
			}
		}

		api->AllocatorFree(allocator, name);
		if (!found)
			return false;
	}

	return true;
}
#endif

bool face_ort_load(struct face_ort *ort, const wchar_t *detect_model, const wchar_t *embed_model)
{
	if (!ort)
		return false;

	memset(ort, 0, sizeof(*ort));

	ort->module = os_dlopen("onnxruntime.dll");
	if (!ort->module)
		return false;

	face_ort_get_api_base_t get_api_base = (face_ort_get_api_base_t)os_dlsym(ort->module, "OrtGetApiBase");
	if (!get_api_base)
		goto fail;

	const struct face_ort_api_base *base = get_api_base();
	if (!base || !base->get_api || !base->get_version_string)
		goto fail;

	ort->version = base->get_version_string();
	if (!ort->version || !*ort->version)
		goto fail;

#if defined(FACE_SWAP_HAS_ORT)
	{
		const OrtApi *api = base->get_api(ORT_API_VERSION);
		if (!api)
			goto fail;

		ort->api = api;

		/* The bundled SFace export carries its weights as graph inputs as
		 * well as initializers, which makes ORT emit a warning per weight
		 * at WARNING level.  Errors are still reported through our own
		 * messages, so keep the OBS log free of that noise. */
		OrtStatus *status = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "obs-big-head", (OrtEnv **)&ort->env);
		if (status) {
			blog(LOG_WARNING, "[big head] ONNX Runtime environment creation failed: %s",
			     api->GetErrorMessage(status));
			api->ReleaseStatus(status);
			goto fail;
		}

		status = api->GetAllocatorWithDefaultOptions((OrtAllocator **)&ort->allocator);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		status = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
						  (OrtMemoryInfo **)&ort->memory_info);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		OrtSession *detect = NULL;
		OrtSession *embed = NULL;

		if (!face_ort_create_session(api, (OrtEnv *)ort->env, detect_model, face_ort_thread_count(4), &detect))
			goto fail;
		ort->detect_session = detect;

		if (!face_ort_create_session(api, (OrtEnv *)ort->env, embed_model, face_ort_thread_count(2), &embed))
			goto fail;
		ort->embed_session = embed;

		OrtAllocator *allocator = (OrtAllocator *)ort->allocator;

		status = api->SessionGetInputName(detect, 0, allocator, &ort->detect_input_name);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		status = api->SessionGetInputName(embed, 0, allocator, &ort->embed_input_name);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		status = api->SessionGetOutputName(embed, 0, allocator, &ort->embed_output_name);
		if (status) {
			api->ReleaseStatus(status);
			goto fail;
		}

		if (!face_ort_output_names_match(api, detect, allocator)) {
			blog(LOG_WARNING, "[big head] detector model does not expose the expected YuNet outputs");
			goto fail;
		}

		ort->detect_input_width = FACE_ORT_CANVAS;
		ort->detect_input_height = FACE_ORT_CANVAS;
		ort->detect_input_count = (size_t)FACE_ORT_CANVAS * FACE_ORT_CANVAS * 3;
		ort->detect_input = bmalloc(ort->detect_input_count * sizeof(float));

		const size_t embed_count = (size_t)FACE_ALIGN_SIZE * FACE_ALIGN_SIZE * 3;
		ort->embed_input = bmalloc(embed_count * sizeof(float));
		ort->embed_input_count = ort->embed_input ? embed_count : 0;

		if (!ort->detect_input || !ort->embed_input)
			goto fail;

		ort->sdk_available = true;
	}
#else
	ort->api = NULL;
	ort->sdk_available = false;
#endif

	return true;

fail:
	face_ort_unload(ort);
	return false;
}

void face_ort_unload(struct face_ort *ort)
{
	if (!ort)
		return;

#if defined(FACE_SWAP_HAS_ORT)
	if (ort->api) {
		const OrtApi *api = ort->api;
		OrtAllocator *allocator = (OrtAllocator *)ort->allocator;

		if (allocator) {
			if (ort->detect_input_name)
				api->AllocatorFree(allocator, ort->detect_input_name);
			if (ort->embed_input_name)
				api->AllocatorFree(allocator, ort->embed_input_name);
			if (ort->embed_output_name)
				api->AllocatorFree(allocator, ort->embed_output_name);
		}

		if (ort->detect_session)
			api->ReleaseSession((OrtSession *)ort->detect_session);
		if (ort->embed_session)
			api->ReleaseSession((OrtSession *)ort->embed_session);
		if (ort->memory_info)
			api->ReleaseMemoryInfo((OrtMemoryInfo *)ort->memory_info);
		if (ort->env)
			api->ReleaseEnv((OrtEnv *)ort->env);
	}
#endif

	if (ort->module)
		os_dlclose(ort->module);

	bfree(ort->detect_input);
	bfree(ort->embed_input);
	memset(ort, 0, sizeof(*ort));
}

size_t face_ort_detect(struct face_ort *ort, const uint8_t *rgba, size_t stride, uint32_t image_width,
		       uint32_t image_height, float score_threshold, float nms_threshold, struct face_detection *faces,
		       size_t max_faces)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->sdk_available || !ort->detect_session || !ort->detect_input || !rgba || !faces)
		return 0;

	const OrtApi *api = ort->api;
	const int padded_width = (int)((image_width + 31) & ~31u);
	const int padded_height = (int)((image_height + 31) & ~31u);
	const size_t plane = (size_t)padded_width * padded_height;
	float *tensor = ort->detect_input;

	if (plane * 3 > ort->detect_input_count) {
		blog(LOG_WARNING, "[big head] capture %ux%u exceeds detector buffer", image_width, image_height);
		return 0;
	}

	memset(tensor, 0, plane * 3 * sizeof(float));

	for (uint32_t y = 0; y < image_height; y++) {
		const uint8_t *row = rgba + (size_t)y * stride;

		for (uint32_t x = 0; x < image_width; x++) {
			const uint8_t *pixel = row + (size_t)x * 4;
			const size_t index = (size_t)y * padded_width + x;

			tensor[index] = (float)pixel[2];
			tensor[plane + index] = (float)pixel[1];
			tensor[plane * 2 + index] = (float)pixel[0];
		}
	}

	const int64_t dims[4] = {1, 3, padded_height, padded_width};
	OrtValue *input = NULL;
	OrtStatus *status = api->CreateTensorWithDataAsOrtValue((OrtMemoryInfo *)ort->memory_info, tensor,
								plane * 3 * sizeof(float), dims, 4,
								ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
	if (status) {
		api->ReleaseStatus(status);
		return 0;
	}

	const char *input_names[1] = {ort->detect_input_name};
	OrtValue *outputs[12] = {0};
	status = api->Run((OrtSession *)ort->detect_session, NULL, input_names, &input, 1, yunet_output_names, 12,
			  outputs);
	api->ReleaseValue(input);

	if (status) {
		api->ReleaseStatus(status);
		for (size_t i = 0; i < 12; i++) {
			if (outputs[i])
				api->ReleaseValue(outputs[i]);
		}
		return 0;
	}

	struct face_detection candidates[256];
	size_t count = 0;

	for (int s = 0; s < 3; s++) {
		const int detector_stride = 8 << s;
		const int cols = padded_width / detector_stride;
		const int rows = padded_height / detector_stride;

		float *cls = NULL;
		float *obj = NULL;
		float *bbox = NULL;
		float *kps = NULL;
		OrtStatus *ss[4] = {
			api->GetTensorMutableData(outputs[s], (void **)&cls),
			api->GetTensorMutableData(outputs[s + 3], (void **)&obj),
			api->GetTensorMutableData(outputs[s + 6], (void **)&bbox),
			api->GetTensorMutableData(outputs[s + 9], (void **)&kps),
		};

		bool data_ok = true;
		for (size_t n = 0; n < 4; n++) {
			if (ss[n]) {
				data_ok = false;
				api->ReleaseStatus(ss[n]);
			}
		}

		if (!data_ok)
			continue;

		count = face_detect_decode_stride(candidates, count, 256, cls, obj, bbox, kps, detector_stride, cols,
						  rows, score_threshold);
	}

	for (size_t i = 0; i < 12; i++) {
		if (outputs[i])
			api->ReleaseValue(outputs[i]);
	}

	size_t kept = face_detect_nms(candidates, count, nms_threshold, max_faces);
	if (!kept)
		return 0;

	if (!image_width || !image_height)
		return 0;

	for (size_t i = 0; i < kept; i++) {
		candidates[i].x /= (float)image_width;
		candidates[i].y /= (float)image_height;
		candidates[i].width /= (float)image_width;
		candidates[i].height /= (float)image_height;

		for (int n = 0; n < FACE_DETECT_LANDMARK_COUNT; n++) {
			candidates[i].landmarks[n][0] /= (float)image_width;
			candidates[i].landmarks[n][1] /= (float)image_height;
		}
	}

	memcpy(faces, candidates, kept * sizeof(*faces));
	return kept;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(rgba);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(image_width);
	UNUSED_PARAMETER(image_height);
	UNUSED_PARAMETER(score_threshold);
	UNUSED_PARAMETER(nms_threshold);
	UNUSED_PARAMETER(max_faces);
	if (faces)
		memset(faces, 0, sizeof(*faces));
	return 0;
#endif
}

bool face_ort_embed(struct face_ort *ort, const uint8_t *rgba, size_t stride, uint32_t image_width,
		    uint32_t image_height, const struct face_detection *face, float *embedding)
{
#if defined(FACE_SWAP_HAS_ORT)
	if (!ort || !ort->sdk_available || !ort->embed_session || !ort->embed_input || !rgba || !face || !embedding)
		return false;

	const OrtApi *api = ort->api;

	float src[FACE_DETECT_LANDMARK_COUNT][2];
	for (int i = 0; i < FACE_DETECT_LANDMARK_COUNT; i++) {
		src[i][0] = face->landmarks[i][0] * (float)image_width;
		src[i][1] = face->landmarks[i][1] * (float)image_height;
	}

	float matrix[6];
	if (!face_align_similarity(src, matrix))
		return false;

	if (!face_align_rgba_to_tensor(rgba, stride, image_width, image_height, matrix, ort->embed_input))
		return false;

	const int64_t dims[4] = {1, 3, FACE_ALIGN_SIZE, FACE_ALIGN_SIZE};
	OrtValue *input = NULL;
	OrtStatus *status =
		api->CreateTensorWithDataAsOrtValue((OrtMemoryInfo *)ort->memory_info, ort->embed_input,
						    (size_t)FACE_ALIGN_SIZE * FACE_ALIGN_SIZE * 3 * sizeof(float), dims,
						    4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
	if (status) {
		api->ReleaseStatus(status);
		return false;
	}

	const char *input_names[1] = {ort->embed_input_name};
	const char *output_names[1] = {ort->embed_output_name};
	OrtValue *output = NULL;
	status = api->Run((OrtSession *)ort->embed_session, NULL, input_names, &input, 1, output_names, 1, &output);
	api->ReleaseValue(input);

	if (status) {
		api->ReleaseStatus(status);
		return false;
	}

	float *data = NULL;
	if (api->GetTensorMutableData(output, (void **)&data) || !data) {
		api->ReleaseValue(output);
		return false;
	}

	memcpy(embedding, data, FACE_EMBED_DIM * sizeof(float));
	api->ReleaseValue(output);
	face_embedding_normalize(embedding, FACE_EMBED_DIM);
	return true;
#else
	UNUSED_PARAMETER(ort);
	UNUSED_PARAMETER(rgba);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(image_width);
	UNUSED_PARAMETER(image_height);
	UNUSED_PARAMETER(face);
	if (embedding)
		memset(embedding, 0, FACE_EMBED_DIM * sizeof(float));
	return false;
#endif
}
