/* Measures CPU inference latency for the matting models.
 *
 * The design targets under 80ms per inference so that a 10-15 FPS inference
 * rate is sustainable while OBS keeps rendering at full frame rate.  Whether
 * the models actually meet that had been an open question; this reports it
 * rather than leaving it to guesswork.
 *
 * Not a pass/fail test - hardware varies too much for a threshold to mean
 * anything in CI.  It prints timings and exits zero.
 */

#include "matting-models.h"
#include "matting-ort.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define WARMUP_FRAMES 2
#define TIMED_FRAMES 10

static void fill_frame(float *chw, uint32_t size, float mean, float scale, uint32_t phase)
{
	const size_t pixels = (size_t)size * size;

	/* Shift the rectangle between frames so a recurrent model sees motion
	 * rather than a still image, which is what it would face live. */
	const uint32_t lo = size / 4 + phase % 8;
	const uint32_t hi = size - size / 4 + phase % 8;

	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			const bool inside = x >= lo && x < hi && y >= lo && y < hi;
			const float pixel[3] = {
				inside ? 210.0f : 40.0f,
				inside ? 170.0f : 45.0f,
				inside ? 150.0f : 60.0f,
			};

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

static void bench(struct matting_ort *ort, const char *dir, enum matting_model_family family, const char *needle)
{
	const struct matting_model_info *info = matting_model_get(family);
	char *path = find_model(dir, needle);

	if (!path) {
		printf("%-20s skip (no model)\n", info->label);
		return;
	}

	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wide);

	struct matting_ort_session session = {0};
	if (!wide || !matting_ort_create_session(ort, wide, &info->spec, &session)) {
		printf("%-20s skip (session failed)\n", info->label);
		bfree(wide);
		bfree(path);
		return;
	}

	const uint32_t size = (uint32_t)info->size;
	const size_t pixels = (size_t)size * size;
	float *src = bmalloc(pixels * 3 * sizeof(float));
	float *matte = bmalloc(pixels * sizeof(float));

	for (uint32_t i = 0; i < WARMUP_FRAMES; i++) {
		fill_frame(src, size, info->mean, info->scale, i);
		matting_ort_run(ort, &session, src, pixels * 3, matte, pixels);
	}

	uint64_t total = 0;
	uint64_t worst = 0;
	uint64_t best = UINT64_MAX;
	uint32_t ok = 0;

	for (uint32_t i = 0; i < TIMED_FRAMES; i++) {
		fill_frame(src, size, info->mean, info->scale, i + WARMUP_FRAMES);

		const uint64_t start = os_gettime_ns();
		const bool success = matting_ort_run(ort, &session, src, pixels * 3, matte, pixels);
		const uint64_t elapsed = os_gettime_ns() - start;

		if (!success)
			continue;

		total += elapsed;
		if (elapsed > worst)
			worst = elapsed;
		if (elapsed < best)
			best = elapsed;
		ok++;
	}

	if (ok) {
		const double mean_ms = (double)total / ok / 1000000.0;
		printf("%-20s %3upx  mean %6.1f ms  min %6.1f  max %6.1f  -> max sustainable %4.1f FPS\n", info->label,
		       size, mean_ms, (double)best / 1000000.0, (double)worst / 1000000.0, 1000.0 / mean_ms);
	} else {
		printf("%-20s no successful runs\n", info->label);
	}

	bfree(src);
	bfree(matte);
	matting_ort_release_session(ort, &session);
	bfree(wide);
	bfree(path);
}

int main(int argc, char **argv)
{
	const char *dir = NULL;
	char *default_dir = NULL;

	for (int i = 1; i < argc - 1; i++) {
		if (strcmp(argv[i], "--models") == 0)
			dir = argv[i + 1];
	}

	if (!dir) {
		default_dir = os_get_config_path_ptr("obs-studio/background-models");
		dir = default_dir;
	}

	if (!dir || !os_file_exists(dir)) {
		printf("skip: model directory not present\n");
		bfree(default_dir);
		return 0;
	}

	struct matting_ort ort = {0};
	if (!matting_ort_load(&ort) || !ort.sdk_available) {
		printf("skip: ONNX Runtime unavailable\n");
		matting_ort_unload(&ort);
		bfree(default_dir);
		return 0;
	}

	/* Thread count is chosen inside matting-ort.c from the core count, so
	 * report the machine rather than claiming a fixed value. */
	printf("ONNX Runtime %s, %u warmup + %u timed frames, %d physical cores\n\n", ort.version, WARMUP_FRAMES,
	       TIMED_FRAMES, os_get_physical_cores());

	bench(&ort, dir, MATTING_MODEL_MODNET, "modnet");
	bench(&ort, dir, MATTING_MODEL_RVM, "rvm");

	matting_ort_unload(&ort);
	bfree(default_dir);
	return 0;
}
