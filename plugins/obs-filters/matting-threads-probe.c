/* Measures how matting inference scales with ONNX Runtime's intra-op thread
 * count, so the default is chosen from data rather than inherited.
 *
 * Standalone rather than part of matting-bench.c: it needs to build sessions
 * with a thread count the public API deliberately does not expose, so it talks
 * to ORT directly.
 */

#include "matting-models.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <onnxruntime_c_api.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define WARMUP_FRAMES 2
#define TIMED_FRAMES 8

static const OrtApi *g_api;

static void fill_frame(float *chw, uint32_t size, float mean, float scale)
{
	const size_t pixels = (size_t)size * size;
	const uint32_t lo = size / 4;
	const uint32_t hi = size - lo;

	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			const bool inside = x >= lo && x < hi && y >= lo && y < hi;
			const float pixel[3] = {inside ? 210.0f : 40.0f, inside ? 170.0f : 45.0f,
						inside ? 150.0f : 60.0f};
			for (size_t c = 0; c < 3; c++)
				chw[c * pixels + (size_t)y * size + x] = (pixel[c] - mean) * scale;
		}
	}
}

static char *find_model(const char *dir, const char *needle)
{
	os_dir_t *handle = os_opendir(dir);
	if (!handle)
		return NULL;

	char *found = NULL;
	struct os_dirent *entry;
	while ((entry = os_readdir(handle)) != NULL) {
		if (entry->directory)
			continue;
		const size_t len = strlen(entry->d_name);
		if (len < 6 || astrcmpi(entry->d_name + len - 5, ".onnx") != 0)
			continue;
		if (!astrstri(entry->d_name, needle))
			continue;
		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", dir, entry->d_name);
		found = path.array;
		break;
	}

	os_closedir(handle);
	return found;
}

/* Runs the model with a given thread count and returns the mean latency in ms,
 * or -1 on failure.  Builds the input/output name lists straight from the
 * session so this works for both families without knowing their layout. */
static double time_model(OrtEnv *env, const wchar_t *path, const struct matting_model_info *info, int threads)
{
	OrtSessionOptions *options = NULL;
	OrtSession *session = NULL;
	OrtStatus *status = g_api->CreateSessionOptions(&options);

	if (!status)
		status = g_api->SetIntraOpNumThreads(options, threads);
	if (!status)
		status = g_api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
	if (!status)
		status = g_api->CreateSession(env, path, options, &session);
	if (options)
		g_api->ReleaseSessionOptions(options);
	if (status) {
		g_api->ReleaseStatus(status);
		return -1.0;
	}

	OrtAllocator *allocator = NULL;
	size_t in_count = 0, out_count = 0;
	g_api->GetAllocatorWithDefaultOptions(&allocator);
	g_api->SessionGetInputCount(session, &in_count);
	g_api->SessionGetOutputCount(session, &out_count);

	char **in_names = bzalloc(in_count * sizeof(char *));
	char **out_names = bzalloc(out_count * sizeof(char *));
	for (size_t i = 0; i < in_count; i++) {
		char *n = NULL;
		g_api->SessionGetInputName(session, i, allocator, &n);
		in_names[i] = bstrdup(n);
		g_api->AllocatorFree(allocator, n);
	}
	for (size_t i = 0; i < out_count; i++) {
		char *n = NULL;
		g_api->SessionGetOutputName(session, i, allocator, &n);
		out_names[i] = bstrdup(n);
		g_api->AllocatorFree(allocator, n);
	}

	const uint32_t size = (uint32_t)info->size;
	const size_t pixels = (size_t)size * size;
	float *src = bmalloc(pixels * 3 * sizeof(float));
	fill_frame(src, size, info->mean, info->scale);

	OrtMemoryInfo *mem = NULL;
	g_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem);

	const int64_t src_shape[4] = {1, 3, size, size};
	const int64_t state_shape[4] = {1, 1, 1, 1};
	const int64_t ratio_shape[1] = {1};
	float state_data = 0.0f;
	float ratio = info->spec.ratio_value > 0.0f ? info->spec.ratio_value : 1.0f;

	double total_ms = 0.0;
	int ok = 0;

	for (uint32_t frame = 0; frame < WARMUP_FRAMES + TIMED_FRAMES; frame++) {
		OrtValue **inputs = bzalloc(in_count * sizeof(OrtValue *));
		OrtValue **outputs = bzalloc(out_count * sizeof(OrtValue *));

		for (size_t i = 0; i < in_count; i++) {
			if (strcmp(in_names[i], info->spec.ratio_input ? info->spec.ratio_input : "\x01") == 0) {
				g_api->CreateTensorWithDataAsOrtValue(mem, &ratio, sizeof(ratio), ratio_shape, 1,
								      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[i]);
			} else {
				bool is_state = false;
				for (size_t s = 0; s < info->spec.state_count; s++)
					is_state = is_state || strcmp(in_names[i], info->spec.state_inputs[s]) == 0;

				if (is_state)
					g_api->CreateTensorWithDataAsOrtValue(mem, &state_data, sizeof(float),
									      state_shape, 4,
									      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
									      &inputs[i]);
				else
					g_api->CreateTensorWithDataAsOrtValue(mem, src, pixels * 3 * sizeof(float),
									      src_shape, 4,
									      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
									      &inputs[i]);
			}
		}

		const uint64_t start = os_gettime_ns();
		OrtStatus *run = g_api->Run(session, NULL, (const char *const *)in_names,
					    (const OrtValue *const *)inputs, in_count,
					    (const char *const *)out_names, out_count, outputs);
		const uint64_t elapsed = os_gettime_ns() - start;

		if (!run && frame >= WARMUP_FRAMES) {
			total_ms += (double)elapsed / 1000000.0;
			ok++;
		}
		if (run)
			g_api->ReleaseStatus(run);

		for (size_t i = 0; i < out_count; i++)
			if (outputs[i])
				g_api->ReleaseValue(outputs[i]);
		for (size_t i = 0; i < in_count; i++)
			if (inputs[i])
				g_api->ReleaseValue(inputs[i]);
		bfree(inputs);
		bfree(outputs);
	}

	for (size_t i = 0; i < in_count; i++)
		bfree(in_names[i]);
	for (size_t i = 0; i < out_count; i++)
		bfree(out_names[i]);
	bfree(in_names);
	bfree(out_names);
	bfree(src);
	g_api->ReleaseMemoryInfo(mem);
	g_api->ReleaseSession(session);

	return ok ? total_ms / ok : -1.0;
}

static void probe(OrtEnv *env, const char *dir, enum matting_model_family family, const char *needle)
{
	const struct matting_model_info *info = matting_model_get(family);
	char *path = find_model(dir, needle);
	if (!path) {
		printf("%s: skip (no model)\n\n", info->label);
		return;
	}

	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wide);

	printf("%s (%dpx):\n", info->label, (int)info->size);

	double baseline = -1.0;
	const int counts[] = {1, 2, 4, 6, 8};

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		const double ms = time_model(env, wide, info, counts[i]);
		if (ms < 0) {
			printf("  %2d threads: failed\n", counts[i]);
			continue;
		}
		if (baseline < 0)
			baseline = ms;

		printf("  %2d threads: %6.1f ms  (%.2fx)  -> %4.1f FPS\n", counts[i], ms, baseline / ms, 1000.0 / ms);
	}
	printf("\n");

	bfree(wide);
	bfree(path);
}

int main(int argc, char **argv)
{
	const char *dir = NULL;
	char *default_dir = NULL;

	for (int i = 1; i < argc - 1; i++)
		if (strcmp(argv[i], "--models") == 0)
			dir = argv[i + 1];

	if (!dir) {
		default_dir = os_get_config_path_ptr("obs-studio/background-models");
		dir = default_dir;
	}

	if (!dir || !os_file_exists(dir)) {
		printf("skip: no model directory\n");
		bfree(default_dir);
		return 0;
	}

	g_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	if (!g_api) {
		printf("skip: ONNX Runtime unavailable\n");
		bfree(default_dir);
		return 0;
	}

	OrtEnv *env = NULL;
	OrtStatus *status = g_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "probe", &env);
	if (status) {
		g_api->ReleaseStatus(status);
		printf("skip: could not create ORT env\n");
		bfree(default_dir);
		return 0;
	}

	printf("intra-op thread scaling, %d warmup + %d timed frames\n\n", WARMUP_FRAMES, TIMED_FRAMES);

	probe(env, dir, MATTING_MODEL_MODNET, "modnet");
	probe(env, dir, MATTING_MODEL_RVM, "rvm");

	g_api->ReleaseEnv(env);
	bfree(default_dir);
	return 0;
}
