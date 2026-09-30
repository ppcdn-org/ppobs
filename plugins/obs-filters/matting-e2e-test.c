/* End-to-end check against real model files.
 *
 * The other tests cover logic in isolation; this one loads an actual .onnx
 * through ONNX Runtime and runs frames through it, which is the only way to
 * confirm the tensor contract in matting-models.c matches what the exports
 * really want.  Everything up to this point had been verified by reading the
 * models' declared io, not by feeding them data.
 *
 * Skips with success when the runtime or the models are absent, so it stays
 * usable on machines that have neither.  Point it at a directory with
 * --models <dir>, or let it default to the OBS config location.
 */

#include "matting-models.h"
#include "matting-ort.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(bool condition, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	if (condition) {
		printf("  ok   ");
	} else {
		printf("  FAIL ");
		failures++;
	}
	vprintf(fmt, args);
	printf("\n");

	va_end(args);
}

/* Draws a filled rectangle on a mid-grey field, as a crude stand-in for a
 * subject against a background.  The models are not expected to find a person
 * in this, so nothing is asserted about the matte's content - only that
 * inference completes and produces values in range. */
static void fill_synthetic_frame(float *chw, uint32_t size, float mean, float scale)
{
	const size_t pixels = (size_t)size * size;
	const uint32_t lo = size / 4;
	const uint32_t hi = size - lo;

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

static bool matte_is_sane(const float *matte, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		/* NaN fails every comparison, so test it explicitly rather than
		 * letting it slip through a range check. */
		if (isnan(matte[i]) || matte[i] < -0.01f || matte[i] > 1.01f)
			return false;
	}
	return true;
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

static void exercise_model(struct matting_ort *ort, const char *dir, enum matting_model_family family,
			   const char *needle)
{
	const struct matting_model_info *info = matting_model_get(family);
	char *path = find_model(dir, needle);

	printf("\n%s:\n", info->label);

	if (!path) {
		printf("  skip no model matching '*%s*.onnx'\n", needle);
		return;
	}

	printf("  using %s\n", path);

	/* The filename convention has to resolve to the family being tested,
	 * otherwise the filter's Automatic setting would pick the wrong tensor
	 * contract for this very file. */
	const char *base = strrchr(path, '/');
	check(matting_model_detect(base ? base + 1 : path) == family, "filename resolves to %s", info->label);

	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wide);

	struct matting_ort_session session = {0};
	const bool opened = wide && matting_ort_create_session(ort, wide, &info->spec, &session);
	check(opened, "session opens");

	if (opened) {
		check(session.src_name != NULL, "image input resolved (%s)", session.src_name ? session.src_name : "-");
		check(session.matte_name != NULL, "matte output resolved (%s)",
		      session.matte_name ? session.matte_name : "-");
		check(session.state_count == info->spec.state_count, "%zu recurrent states", session.state_count);

		const uint32_t size = (uint32_t)info->size;
		const size_t pixels = (size_t)size * size;
		float *src = bmalloc(pixels * 3 * sizeof(float));
		float *matte = bmalloc(pixels * sizeof(float));

		fill_synthetic_frame(src, size, info->mean, info->scale);

		const bool first = matting_ort_run(ort, &session, src, pixels * 3, matte, pixels);
		check(first, "inference runs");
		if (first)
			check(matte_is_sane(matte, pixels), "matte values within [0, 1]");

		if (first && info->recurrent) {
			/* State should now be carried; a second frame must
			 * still succeed rather than choke on the retained
			 * OrtValues. */
			check(session.states[0].value != NULL, "recurrent state retained after first frame");

			const bool second = matting_ort_run(ort, &session, src, pixels * 3, matte, pixels);
			check(second, "second frame runs with carried state");
			if (second)
				check(matte_is_sane(matte, pixels), "matte still sane on second frame");

			/* Resetting must drop the state without disturbing the
			 * session, so the next frame starts from a clean
			 * memory - this is the path taken when a source is
			 * hidden or the scene changes. */
			matting_ort_reset_states(ort, &session);
			check(session.states[0].value == NULL, "reset clears carried state");

			const bool third = matting_ort_run(ort, &session, src, pixels * 3, matte, pixels);
			check(third, "runs again after reset");
		}

		/* A wrong element count must be refused rather than read out of
		 * bounds. */
		check(!matting_ort_run(ort, &session, src, pixels * 3 - 1, matte, pixels), "rejects short input");
		check(!matting_ort_run(ort, &session, src, pixels * 3, matte, pixels - 1), "rejects short matte");

		bfree(src);
		bfree(matte);
		matting_ort_release_session(ort, &session);
	}

	bfree(wide);
	bfree(path);
}

/* The MODNet spec resolves its matte by shape, and RVM emits a same-sized
 * "fgr" alongside "pha".  Applying MODNet's spec to RVM would therefore be a
 * plausible way to silently select the foreground instead of the alpha, so
 * confirm the mismatch is rejected outright. */
static void test_cross_family_rejected(struct matting_ort *ort, const char *dir)
{
	char *path = find_model(dir, "rvm");

	printf("\ncross-family:\n");
	if (!path) {
		printf("  skip no RVM model present\n");
		return;
	}

	wchar_t *wide = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wide);

	const struct matting_model_info *modnet = matting_model_get(MATTING_MODEL_MODNET);
	struct matting_ort_session session = {0};
	const bool opened = wide && matting_ort_create_session(ort, wide, &modnet->spec, &session);

	check(!opened, "RVM rejected when opened with the MODNet spec");
	if (opened)
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

	printf("model directory: %s\n", dir ? dir : "(none)");

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

	printf("ONNX Runtime %s\n", ort.version);

	exercise_model(&ort, dir, MATTING_MODEL_MODNET, "modnet");
	exercise_model(&ort, dir, MATTING_MODEL_RVM, "rvm");
	test_cross_family_rejected(&ort, dir);

	matting_ort_unload(&ort);
	bfree(default_dir);

	printf("\n%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
