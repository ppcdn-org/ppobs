#include "face-detect.h"
#include "face-ort.h"

#include <util/bmem.h>
#include <util/platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FACE_MODEL_DIR
#define FACE_MODEL_DIR "."
#endif

static bool load_models(struct face_ort *ort)
{
	char *detect_path = bstrdup(FACE_MODEL_DIR "/face_detection_yunet_2026may.onnx");
	char *embed_path = bstrdup(FACE_MODEL_DIR "/face_recognition_sface_2021dec.onnx");
	wchar_t *detect_wide = NULL;
	wchar_t *embed_wide = NULL;
	bool ok = false;

	if (os_file_exists(detect_path) && os_file_exists(embed_path) &&
	    os_utf8_to_wcs_ptr(detect_path, 0, &detect_wide) && os_utf8_to_wcs_ptr(embed_path, 0, &embed_wide))
		ok = face_ort_load(ort, detect_wide, embed_wide);

	bfree(detect_wide);
	bfree(embed_wide);
	bfree(detect_path);
	bfree(embed_path);
	return ok;
}

int main(void)
{
	struct face_ort ort;

	if (!load_models(&ort)) {
		printf("skip: face models or ONNX Runtime unavailable\n");
		return 0;
	}

	const uint32_t width = 320;
	const uint32_t height = 180;
	const size_t size = (size_t)width * height * 4;
	uint8_t *image = bmalloc(size);

	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint8_t *p = image + ((size_t)y * width + x) * 4;
			p[0] = (uint8_t)(x * 255 / width);
			p[1] = (uint8_t)(y * 255 / height);
			p[2] = 80;
			p[3] = 255;
		}
	}

	struct face_detection faces[FACE_DETECT_MAX_FACES];
	const size_t detected = face_ort_detect(&ort, image, (size_t)width * 4, width, height, 0.9f, 0.3f, faces,
						FACE_DETECT_MAX_FACES);
	printf("detected on gradient: %zu\n", detected);

	struct face_detection face;
	memset(&face, 0, sizeof(face));
	face.x = 100.0f / width;
	face.y = 40.0f / height;
	face.width = 90.0f / width;
	face.height = 110.0f / height;

	const float points[FACE_DETECT_LANDMARK_COUNT][2] = {
		{115.0f / width, 85.0f / height},  {175.0f / width, 85.0f / height},  {145.0f / width, 110.0f / height},
		{120.0f / width, 135.0f / height}, {170.0f / width, 135.0f / height},
	};
	memcpy(face.landmarks, points, sizeof(points));

	float embedding[FACE_EMBED_DIM];
	if (!face_ort_embed(&ort, image, (size_t)width * 4, width, height, &face, embedding)) {
		fprintf(stderr, "embedding failed\n");
		face_ort_unload(&ort);
		bfree(image);
		return 1;
	}

	float norm = 0.0f;
	for (size_t i = 0; i < FACE_EMBED_DIM; i++)
		norm += embedding[i] * embedding[i];
	norm = sqrtf(norm);

	if (fabsf(norm - 1.0f) > 1e-3f) {
		fprintf(stderr, "embedding is not normalised (norm %f)\n", norm);
		face_ort_unload(&ort);
		bfree(image);
		return 1;
	}

	printf("embedding ok (norm %.4f)\n", norm);

	/* Optional manual check against a raw 320x180 RGBA capture.  Enable it
	 * with FACE_RAW_IMAGE=<path> when a real two-person frame is available;
	 * CTest leaves it unset so the committed test stays self-contained. */
	const char *raw_path = getenv("FACE_RAW_IMAGE");
	if (raw_path) {
		FILE *file = fopen(raw_path, "rb");
		if (file) {
			const size_t read = fread(image, 1, size, file);
			fclose(file);

			if (read == size) {
				const size_t real = face_ort_detect(&ort, image, (size_t)width * 4, width, height,
								    0.75f, 0.3f, faces, FACE_DETECT_MAX_FACES);
				printf("detected on real image: %zu\n", real);
				if (real == 0) {
					fprintf(stderr, "expected at least one face in the raw image\n");
					face_ort_unload(&ort);
					bfree(image);
					return 1;
				}

				float first[FACE_EMBED_DIM];
				if (!face_ort_embed(&ort, image, (size_t)width * 4, width, height, &faces[0], first)) {
					fprintf(stderr, "embedding first real face failed\n");
					face_ort_unload(&ort);
					bfree(image);
					return 1;
				}

				if (real >= 2) {
					float second[FACE_EMBED_DIM];
					if (face_ort_embed(&ort, image, (size_t)width * 4, width, height, &faces[1],
							   second)) {
						const float similarity =
							face_embedding_similarity(first, second, FACE_EMBED_DIM);
						printf("real face similarity: %.4f\n", similarity);
					}
				}
			}
		}
	}

	printf("face-ort: all tests passed\n");

	face_ort_unload(&ort);
	bfree(image);
	return 0;
}
