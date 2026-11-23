#include "face-detect.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                                                        \
	do {                                                                                               \
		if (!(expr)) {                                                                             \
			fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #expr);           \
			exit(1);                                                                           \
		}                                                                                          \
	} while (0)

static void test_decode_stride(void)
{
	float cls[1] = {0.81f};
	float obj[1] = {1.0f};
	float bbox[4] = {0.5f, 0.5f, 0.0f, 0.0f};
	float kps[10] = {0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f, 0.25f, 0.75f, 0.75f, 0.75f};
	struct face_detection faces[FACE_DETECT_MAX_FACES];

	size_t count = face_detect_decode_stride(faces, 0, FACE_DETECT_MAX_FACES, cls, obj, bbox, kps, 8, 1, 1, 0.8f);
	CHECK(count == 1);
	CHECK(fabsf(faces[0].x - 0.0f) < 1e-4f);
	CHECK(fabsf(faces[0].y - 0.0f) < 1e-4f);
	CHECK(fabsf(faces[0].width - 8.0f) < 1e-4f);
	CHECK(fabsf(faces[0].height - 8.0f) < 1e-4f);
	CHECK(fabsf(faces[0].score - 0.9f) < 1e-4f);
	CHECK(fabsf(faces[0].landmarks[1][0] - 4.0f) < 1e-4f);
	CHECK(fabsf(faces[0].landmarks[4][1] - 6.0f) < 1e-4f);

	count = face_detect_decode_stride(faces, 0, FACE_DETECT_MAX_FACES, cls, obj, bbox, kps, 8, 1, 1, 0.95f);
	CHECK(count == 0);

	printf("ok: decode stride\n");
}

static void test_nms(void)
{
	struct face_detection faces[3] = {
		{.x = 0.0f, .y = 0.0f, .width = 10.0f, .height = 10.0f, .score = 0.9f},
		{.x = 1.0f, .y = 1.0f, .width = 10.0f, .height = 10.0f, .score = 0.8f},
		{.x = 20.0f, .y = 20.0f, .width = 10.0f, .height = 10.0f, .score = 0.7f},
	};

	const size_t kept = face_detect_nms(faces, 3, 0.3f, FACE_DETECT_MAX_FACES);
	CHECK(kept == 2);
	CHECK(fabsf(faces[0].score - 0.9f) < 1e-4f);
	CHECK(fabsf(faces[1].score - 0.7f) < 1e-4f);

	printf("ok: nms\n");
}

static void test_similarity_identity(void)
{
	static const float landmarks[FACE_DETECT_LANDMARK_COUNT][2] = {
		{38.2946f, 51.6963f}, {73.5318f, 51.5014f}, {56.0252f, 71.7366f},
		{41.5493f, 92.3655f}, {70.7299f, 92.2041f},
	};
	float matrix[6];

	CHECK(face_align_similarity(landmarks, matrix));
	CHECK(fabsf(matrix[0] - 1.0f) < 1e-5f);
	CHECK(fabsf(matrix[1]) < 1e-5f);
	CHECK(fabsf(matrix[2]) < 1e-5f);
	CHECK(fabsf(matrix[3]) < 1e-5f);
	CHECK(fabsf(matrix[4] - 1.0f) < 1e-5f);
	CHECK(fabsf(matrix[5]) < 1e-5f);

	printf("ok: similarity identity\n");
}

static void test_similarity_scaled(void)
{
	static const float landmarks[FACE_DETECT_LANDMARK_COUNT][2] = {
		{38.2946f, 51.6963f}, {73.5318f, 51.5014f}, {56.0252f, 71.7366f},
		{41.5493f, 92.3655f}, {70.7299f, 92.2041f},
	};
	float src[FACE_DETECT_LANDMARK_COUNT][2];
	float matrix[6];

	for (int i = 0; i < FACE_DETECT_LANDMARK_COUNT; i++) {
		src[i][0] = (landmarks[i][0] - 56.0f) * 2.0f + 200.0f;
		src[i][1] = (landmarks[i][1] - 72.0f) * 2.0f + 150.0f;
	}

	CHECK(face_align_similarity(src, matrix));

	for (int i = 0; i < FACE_DETECT_LANDMARK_COUNT; i++) {
		const float x = matrix[0] * src[i][0] + matrix[1] * src[i][1] + matrix[2];
		const float y = matrix[3] * src[i][0] + matrix[4] * src[i][1] + matrix[5];
		CHECK(fabsf(x - landmarks[i][0]) < 1e-3f);
		CHECK(fabsf(y - landmarks[i][1]) < 1e-3f);
	}

	printf("ok: similarity scaled\n");
}

static void test_align_tensor(void)
{
	uint8_t image[FACE_ALIGN_SIZE * FACE_ALIGN_SIZE * 4];
	float tensor[FACE_ALIGN_SIZE * FACE_ALIGN_SIZE * 3];
	const float identity[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
	const size_t plane = (size_t)FACE_ALIGN_SIZE * FACE_ALIGN_SIZE;

	for (int y = 0; y < FACE_ALIGN_SIZE; y++) {
		for (int x = 0; x < FACE_ALIGN_SIZE; x++) {
			uint8_t *pixel = image + ((size_t)y * FACE_ALIGN_SIZE + x) * 4;
			pixel[0] = (uint8_t)x;
			pixel[1] = (uint8_t)y;
			pixel[2] = 77;
			pixel[3] = 255;
		}
	}

	CHECK(face_align_rgba_to_tensor(image, FACE_ALIGN_SIZE * 4, FACE_ALIGN_SIZE, FACE_ALIGN_SIZE, identity,
					tensor));

	const size_t index = (size_t)20 * FACE_ALIGN_SIZE + 10;
	CHECK(fabsf(tensor[index] - 10.0f) < 1e-3f);
	CHECK(fabsf(tensor[plane + index] - 20.0f) < 1e-3f);
	CHECK(fabsf(tensor[plane * 2 + index] - 77.0f) < 1e-3f);

	printf("ok: align tensor\n");
}

static void test_embedding(void)
{
	float a[FACE_EMBED_DIM] = {0};
	float b[FACE_EMBED_DIM] = {0};

	a[0] = 3.0f;
	a[1] = 4.0f;
	b[0] = 1.0f;
	b[1] = 0.0f;

	face_embedding_normalize(a, FACE_EMBED_DIM);
	CHECK(fabsf(a[0] - 0.6f) < 1e-5f);
	CHECK(fabsf(a[1] - 0.8f) < 1e-5f);
	CHECK(fabsf(face_embedding_similarity(a, b, FACE_EMBED_DIM) - 0.6f) < 1e-5f);

	printf("ok: embedding\n");
}

int main(void)
{
	test_decode_stride();
	test_nms();
	test_similarity_identity();
	test_similarity_scaled();
	test_align_tensor();
	test_embedding();
	printf("face-detect: all tests passed\n");
	return 0;
}
