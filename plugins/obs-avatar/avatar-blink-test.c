/* Draws the blink on a real GPU, headless, and writes the pixels out.
 *
 * The blink is a shader, and a shader is the one part of this that nothing else
 * checks: it either compiles on the machine's D3D11 or it does not, and its
 * parameters are wired by name. This renders the artwork and then the lids at a
 * few blink amounts into an offscreen texture and writes each as a BMP, so they
 * can be compared with the picture the shader was designed against.
 *
 * It needs a GPU and the OBS runtime beside it, so it is run by hand from the
 * directory holding obs.dll and libobs-d3d11.dll:
 *
 *   avatar-blink-test <image> <eyes.txt> <output dir> [amount ...]
 *
 * eyes.txt is "<ref width> <ref height>" and then ten numbers per eye, as the
 * worker sends them. With no GPU it SKIPs.
 */

#include "avatar-blink.h"
#include "avatar-test-check.h"

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void skip(const char *why)
{
	printf("SKIP: %s\n", why);
	fflush(stdout);
	exit(0);
}

static bool read_eyes(const char *path, struct avatar_eyes *eyes)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return false;

	memset(eyes, 0, sizeof(*eyes));
	bool ok = fscanf(file, "%u %u", &eyes->ref_width, &eyes->ref_height) == 2;

	while (ok && eyes->count < AVATAR_EYES_CAPACITY) {
		struct avatar_eye *e = &eyes->eye[eyes->count];
		const int got = fscanf(file, "%f %f %f %f %f %f %f %f %f %f", &e->cx, &e->left, &e->right, &e->eh, &e->up[0],
				       &e->up[1], &e->up[2], &e->lo[0], &e->lo[1], &e->lo[2]);
		if (got != 10)
			break;
		eyes->count++;
	}

	fclose(file);
	eyes->valid = eyes->count > 0;
	return eyes->valid;
}

/* 24-bit BMP, top row first, so no image library is needed to look at it. */
static void write_bmp(const char *path, const uint8_t *rgba, uint32_t linesize, uint32_t w, uint32_t h)
{
	FILE *file = fopen(path, "wb");
	CHECK(file != NULL);

	const uint32_t row = (w * 3 + 3) & ~3u;
	const uint32_t size = 54 + row * h;
	uint8_t header[54] = {'B', 'M'};
	memcpy(header + 2, &size, 4);
	const uint32_t offset = 54, dib = 40, planes_bits = (24u << 16) | 1u, pixels = row * h;
	const int32_t sw = (int32_t)w, sh = -(int32_t)h; /* negative height: top-down */
	memcpy(header + 10, &offset, 4);
	memcpy(header + 14, &dib, 4);
	memcpy(header + 18, &sw, 4);
	memcpy(header + 22, &sh, 4);
	memcpy(header + 26, &planes_bits, 4);
	memcpy(header + 34, &pixels, 4);
	fwrite(header, 1, sizeof(header), file);

	uint8_t *line = calloc(1, row);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
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

int main(int argc, char **argv)
{
	if (argc < 4) {
		printf("usage: avatar-blink-test <image> <eyes.txt> <output dir> [amount ...]\n");
		return 2;
	}

	struct avatar_eyes eyes;
	CHECK(read_eyes(argv[2], &eyes));
	CHECK(eyes.count == 2);

	if (!obs_startup("en-US", NULL, NULL))
		skip("could not start libobs");

	/* Deprecated in favour of the frontend's own lookup, which a headless test
	 * does not have; it is still the way to say where the base effects live. */
#pragma warning(push)
#pragma warning(disable : 4996)
	obs_add_data_path("../../data/libobs/");
#pragma warning(pop)

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = 640;
	ovi.base_height = 480;
	ovi.output_width = 640;
	ovi.output_height = 480;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	ovi.gpu_conversion = true;

	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		skip("no usable Direct3D 11 device");

	obs_enter_graphics();

	gs_image_file4_t image;
	gs_image_file4_init(&image, argv[1], GS_IMAGE_ALPHA_PREMULTIPLY);
	gs_image_file4_init_texture(&image);
	struct gs_image_file *file = &image.image3.image2.image;
	CHECK(file->loaded);
	CHECK(file->texture != NULL);

	const uint32_t w = file->cx, h = file->cy;
	printf("artwork %ux%u, reference %ux%u\n", w, h, eyes.ref_width, eyes.ref_height);

	gs_texrender_t *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	CHECK(target != NULL);

	struct avatar_blink blink;
	avatar_blink_init(&blink, 1);

	static const float defaults[] = {0.0f, 0.35f, 0.7f, 1.0f};
	const float *amounts = defaults;
	float given[16];
	size_t count = sizeof(defaults) / sizeof(defaults[0]);

	if (argc > 4) {
		count = (size_t)(argc - 4) < 16 ? (size_t)(argc - 4) : 16;
		for (size_t i = 0; i < count; i++)
			given[i] = (float)atof(argv[4 + i]);
		amounts = given;
	}

	for (size_t i = 0; i < count; i++) {
		gs_texrender_reset(target);
		CHECK(gs_texrender_begin(target, w, h));

		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

		/* The artwork the way the source draws it, then the lids over it. */
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), file->texture);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(file->texture, 0, w, h);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);

		avatar_blink_draw(&blink, file->texture, w, h, &eyes, amounts[i]);

		gs_texrender_end(target);

		gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
		CHECK(stage != NULL);
		gs_stage_texture(stage, gs_texrender_get_texture(target));

		uint8_t *data = NULL;
		uint32_t linesize = 0;
		CHECK(gs_stagesurface_map(stage, &data, &linesize));

		char path[1024];
		snprintf(path, sizeof(path), "%s/blink_gpu_%03d.bmp", argv[3], (int)(amounts[i] * 100.0f + 0.5f));
		write_bmp(path, data, linesize, w, h);
		printf("amount %.2f -> %s\n", (double)amounts[i], path);

		gs_stagesurface_unmap(stage);
		gs_stagesurface_destroy(stage);
	}

	CHECK(!blink.failed);
	CHECK(blink.effect != NULL);

	avatar_blink_free(&blink);
	gs_texrender_destroy(target);
	gs_image_file4_free(&image);
	obs_leave_graphics();

	obs_shutdown();
	printf("PASS\n");
	return 0;
}
