/* Runs the blurred-background shader on a real GPU, headless, and checks pixels.
 *
 * background_alpha.effect is only compiled when OBS first creates the filter,
 * so a typo in it, a wrong parameter name, an unsupported render-target format
 * or a parameter set at the wrong moment would not show up until someone enabled
 * the filter on a live source.  This runs the filter's own blur code
 * (background-blur-render.c) and the composite technique over a synthetic frame
 * whose answer is known:
 *
 *   - the background is one-pixel grey stripes, which a blur must flatten;
 *   - the subject is a solid green block with a matching matte, which must come
 *     through untouched, and whose green must not leak into the background.
 *
 * The last point is why the blur is weighted by the matte.  A plain Gaussian
 * smears the subject's colour outward, leaving a coloured halo around the
 * silhouette; the grey stripes have no green in them, so any green found beside
 * the block is that halo.
 *
 * It needs a GPU and the OBS runtime beside it, so it is run by hand from the
 * directory holding obs.dll and libobs-d3d11.dll:
 *
 *   background-blur-gpu-test <background_alpha.effect> [output.bmp]
 *
 * With no usable device it prints SKIP and exits 0.
 */

#include "background-blur-render.h"

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 360

/* The subject block, in pixels. */
#define SUBJECT_LEFT 240
#define SUBJECT_RIGHT 400
#define SUBJECT_TOP 120
#define SUBJECT_BOTTOM 240

static int failures;

#define CHECK(cond, ...)                                          \
	do {                                                      \
		if (!(cond)) {                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                      \
			printf("\n");                             \
			failures++;                               \
		}                                                 \
	} while (0)

static void skip(const char *why)
{
	printf("SKIP: %s\n", why);
	fflush(stdout);
	exit(0);
}

static bool in_subject(int x, int y)
{
	return x >= SUBJECT_LEFT && x < SUBJECT_RIGHT && y >= SUBJECT_TOP && y < SUBJECT_BOTTOM;
}

static void write_bmp(const char *path, const uint8_t *rgba, uint32_t linesize)
{
	FILE *file = fopen(path, "wb");
	if (!file)
		return;

	const uint32_t row = (W * 3 + 3) & ~3u;
	const uint32_t pixels = row * H;
	const uint32_t size = 54 + pixels;
	uint8_t header[54] = {'B', 'M'};
	const uint32_t offset = 54, dib = 40, planes_bits = (24u << 16) | 1u;
	const int32_t sw = W, sh = -H;

	memcpy(header + 2, &size, 4);
	memcpy(header + 10, &offset, 4);
	memcpy(header + 14, &dib, 4);
	memcpy(header + 18, &sw, 4);
	memcpy(header + 22, &sh, 4);
	memcpy(header + 26, &planes_bits, 4);
	memcpy(header + 34, &pixels, 4);
	fwrite(header, 1, sizeof(header), file);

	uint8_t *line = calloc(1, row);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const uint8_t *px = rgba + (size_t)y * linesize + (size_t)x * 4;
			line[x * 3 + 0] = px[2];
			line[x * 3 + 1] = px[1];
			line[x * 3 + 2] = px[0];
		}
		fwrite(line, 1, row, file);
	}
	free(line);
	fclose(file);
}

/* Draws the frame into the reduced target, the way the filter draws its source. */
static void draw_frame(void *data, uint32_t width, uint32_t height)
{
	gs_texture_t *source = data;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), source);
	while (gs_effect_loop(base, "Draw"))
		gs_draw_sprite(source, 0, width, height);
	gs_blend_state_pop();
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: background-blur-gpu-test <background_alpha.effect> [output.bmp]\n");
		return 2;
	}

	if (!obs_startup("en-US", NULL, NULL))
		skip("could not start libobs");

#pragma warning(push)
#pragma warning(disable : 4996)
	obs_add_data_path("../../data/libobs/");
#pragma warning(pop)

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = W;
	ovi.base_height = H;
	ovi.output_width = W;
	ovi.output_height = H;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	ovi.gpu_conversion = true;

	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		skip("no usable Direct3D 11 device");

	obs_enter_graphics();

	char *error = NULL;
	gs_effect_t *effect = gs_effect_create_from_file(argv[1], &error);
	if (!effect) {
		printf("FAIL: effect did not compile: %s\n", error ? error : "(no message)");
		return 1;
	}
	bfree(error);

	/* Frame: one-pixel vertical grey stripes, with a green block on top. */
	uint8_t *frame = bmalloc((size_t)W * H * 4);
	uint8_t *matte_pixels = bmalloc((size_t)W * H);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint8_t *px = frame + ((size_t)y * W + x) * 4;
			const bool subject = in_subject(x, y);
			const uint8_t stripe = (x & 1) ? 255 : 0;

			px[0] = subject ? 0 : stripe;
			px[1] = subject ? 255 : stripe;
			px[2] = subject ? 0 : stripe;
			px[3] = 255;
			matte_pixels[(size_t)y * W + x] = subject ? 255 : 0;
		}
	}

	const uint8_t *frame_data = frame;
	const uint8_t *matte_data = matte_pixels;
	gs_texture_t *source = gs_texture_create(W, H, GS_RGBA, 1, &frame_data, 0);
	gs_texture_t *mask = gs_texture_create(W, H, GS_R8, 1, &matte_data, 0);
	if (!source || !mask) {
		printf("FAIL: could not create input textures\n");
		return 1;
	}

	const struct background_blur_plan plan = background_blur_plan_for(W, H, BACKGROUND_BLUR_MAX_STRENGTH);
	CHECK(plan.valid, "plan for %dx%d is invalid", W, H);
	printf("plan: scale %u, reduced %ux%u, radius %.1f, sigma %.2f\n", plan.scale, plan.width, plan.height,
	       plan.radius, plan.sigma);

	const struct background_blur_matte matte = {.texture = mask, .scale = {1.0f, 1.0f}, .offset = {0.0f, 0.0f}};
	struct background_blur_targets targets = {{NULL}};

	gs_texrender_t *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(W, H, GS_RGBA);
	if (!output || !stage) {
		printf("FAIL: could not create render targets\n");
		return 1;
	}

	gs_texture_t *blurred = background_blur_render(effect, &targets, &plan, &matte, draw_frame, source);
	if (!blurred) {
		printf("FAIL: background_blur_render returned no texture\n");
		return 1;
	}

	/* The composite, with the matte fully applied.  Parameters are set here,
	 * after the blur has run, because an effect forgets them all each time a
	 * technique ends. */
	gs_texrender_reset(output);
	CHECK(gs_texrender_begin(output, W, H), "output target");
	{
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)W, 0.0f, (float)H, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), source);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "blurred_tex"), blurred);
		background_blur_set_matte(effect, &matte);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "mask_strength"), 1.0f);
		while (gs_effect_loop(effect, "DrawBlurred"))
			gs_draw_sprite(source, 0, W, H);
		gs_blend_state_pop();
	}
	gs_texrender_end(output);

	gs_stage_texture(stage, gs_texrender_get_texture(output));

	uint8_t *pixels = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(stage, &pixels, &linesize) || !pixels) {
		printf("FAIL: could not read the result back\n");
		return 1;
	}

#define PX(x, y, c) pixels[(size_t)(y) * linesize + (size_t)(x) * 4 + (c)]

	/* The subject comes through as it went in, edge included. */
	CHECK(PX(320, 180, 0) <= 3 && PX(320, 180, 1) >= 252 && PX(320, 180, 2) <= 3, "subject centre is %d,%d,%d",
	      PX(320, 180, 0), PX(320, 180, 1), PX(320, 180, 2));
	CHECK(PX(SUBJECT_LEFT, 180, 1) >= 252 && PX(SUBJECT_LEFT, 180, 0) <= 3, "subject left edge is %d,%d,%d",
	      PX(SUBJECT_LEFT, 180, 0), PX(SUBJECT_LEFT, 180, 1), PX(SUBJECT_LEFT, 180, 2));

	/* The background is flattened: a stripe pattern that swings 0..255 per
	 * pixel must end up nearly constant, around the mid-grey it averages to. */
	int lo = 255, hi = 0;
	for (int y = 20; y < 100; y++) {
		for (int x = 20; x < 200; x++) {
			const int r = PX(x, y, 0);
			if (r < lo)
				lo = r;
			if (r > hi)
				hi = r;
		}
	}
	printf("background red range %d..%d\n", lo, hi);
	CHECK(hi - lo <= 24, "background still striped: red spans %d..%d", lo, hi);
	CHECK(lo >= 100 && hi <= 156, "background is not mid-grey: red spans %d..%d", lo, hi);

	/* No green leaking out of the subject.  The band is just outside the
	 * block, where a plain Gaussian would be strongest. */
	int worst_green = 0;
	for (int y = SUBJECT_TOP + 10; y < SUBJECT_BOTTOM - 10; y++) {
		for (int x = SUBJECT_LEFT - 24; x < SUBJECT_LEFT - 2; x++) {
			const int g = PX(x, y, 1) - PX(x, y, 0);
			if (g > worst_green)
				worst_green = g;
		}
		for (int x = SUBJECT_RIGHT + 2; x < SUBJECT_RIGHT + 24; x++) {
			const int g = PX(x, y, 1) - PX(x, y, 0);
			if (g > worst_green)
				worst_green = g;
		}
	}
	printf("worst green excess beside the subject: %d\n", worst_green);
	CHECK(worst_green <= 12, "subject colour leaks into the background (green excess %d)", worst_green);

	/* Alpha passes through from the source. */
	CHECK(PX(10, 10, 3) == 255, "alpha is %d", PX(10, 10, 3));

	if (argc > 2)
		write_bmp(argv[2], pixels, linesize);

	gs_stagesurface_unmap(stage);

	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(output);
	background_blur_targets_destroy(&targets);
	gs_texture_destroy(mask);
	gs_texture_destroy(source);
	gs_effect_destroy(effect);
	obs_leave_graphics();
	bfree(frame);
	bfree(matte_pixels);
	obs_shutdown();

	printf(failures ? "%d FAILED\n" : "PASS\n", failures);
	return failures ? 1 : 0;
}
