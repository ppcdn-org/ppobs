#include "face-track.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 256
#define HEIGHT 256

static void fill_frame(uint8_t *frame, uint8_t r, uint8_t g, uint8_t b)
{
	for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++) {
		frame[i * 4 + 0] = r;
		frame[i * 4 + 1] = g;
		frame[i * 4 + 2] = b;
		frame[i * 4 + 3] = 255;
	}
}

static void fill_rect(uint8_t *frame, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
	for (int py = y; py < y + h; py++) {
		for (int px = x; px < x + w; px++) {
			uint8_t *pixel = frame + ((size_t)py * WIDTH + px) * 4;
			pixel[0] = r;
			pixel[1] = g;
			pixel[2] = b;
			pixel[3] = 255;
		}
	}
}

static void test_finds_synthetic_face(void)
{
	uint8_t frame[WIDTH * HEIGHT * 4];
	fill_frame(frame, 10, 10, 40);
	fill_rect(frame, 90, 70, 80, 100, 200, 150, 120);

	struct face_track track;
	face_track_init(&track);

	assert(face_track_update_rgba(&track, frame, WIDTH * 4, WIDTH, HEIGHT));
	assert(track.present);
	assert(fabsf(track.x - 90.0f / WIDTH) < 0.04f);
	assert(fabsf(track.y - 70.0f / HEIGHT) < 0.04f);
	assert(fabsf(track.width - 80.0f / WIDTH) < 0.04f);
	assert(fabsf(track.height - 100.0f / HEIGHT) < 0.04f);

	face_track_free(&track);
	printf("ok: synthetic face\n");
}

static void test_ignores_non_skin(void)
{
	uint8_t frame[WIDTH * HEIGHT * 4];
	fill_frame(frame, 10, 10, 40);

	struct face_track track;
	face_track_init(&track);

	assert(!face_track_update_rgba(&track, frame, WIDTH * 4, WIDTH, HEIGHT));
	assert(!track.present);
	assert(track.width == 0.0f);

	face_track_free(&track);
	printf("ok: non-skin frame ignored\n");
}

static void test_picks_largest_blob(void)
{
	uint8_t frame[WIDTH * HEIGHT * 4];
	fill_frame(frame, 10, 10, 40);
	fill_rect(frame, 20, 20, 30, 30, 200, 150, 120);
	fill_rect(frame, 100, 80, 90, 110, 200, 150, 120);

	struct face_track track;
	face_track_init(&track);

	assert(face_track_update_rgba(&track, frame, WIDTH * 4, WIDTH, HEIGHT));
	assert(fabsf(track.x - 100.0f / WIDTH) < 0.04f);
	assert(fabsf(track.y - 80.0f / HEIGHT) < 0.04f);
	assert(fabsf(track.width - 90.0f / WIDTH) < 0.04f);

	face_track_free(&track);
	printf("ok: largest blob chosen\n");
}

static void test_holds_then_drops(void)
{
	uint8_t face_frame[WIDTH * HEIGHT * 4];
	uint8_t blank_frame[WIDTH * HEIGHT * 4];

	fill_frame(face_frame, 10, 10, 40);
	fill_rect(face_frame, 90, 70, 80, 100, 200, 150, 120);
	fill_frame(blank_frame, 10, 10, 40);

	struct face_track track;
	face_track_init(&track);

	assert(face_track_update_rgba(&track, face_frame, WIDTH * 4, WIDTH, HEIGHT));

	/* A short dropout keeps the lock. */
	for (int i = 0; i < 5; i++)
		assert(face_track_update_rgba(&track, blank_frame, WIDTH * 4, WIDTH, HEIGHT));
	assert(track.present);

	/* Long enough without a face and the lock drops. */
	for (int i = 0; i < 20; i++)
		face_track_update_rgba(&track, blank_frame, WIDTH * 4, WIDTH, HEIGHT);
	assert(!track.present);

	/* The last box is retained so the caller can keep drawing it. */
	assert(track.width > 0.0f);

	face_track_free(&track);
	printf("ok: hold and drop\n");
}

int main(void)
{
	test_finds_synthetic_face();
	test_ignores_non_skin();
	test_picks_largest_blob();
	test_holds_then_drops();
	printf("face-track: all tests passed\n");
	return 0;
}
