#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FACE_DETECT_MAX_FACES 8
#define FACE_DETECT_LANDMARK_COUNT 5
#define FACE_EMBED_DIM 128
#define FACE_ALIGN_SIZE 112

struct face_detection {
	float x;
	float y;
	float width;
	float height;
	float landmarks[FACE_DETECT_LANDMARK_COUNT][2];
	float score;
};

/* Decodes one YuNet stride's raw outputs into `faces`, appending after `count`.
 * Coordinates are in the detector canvas; the caller normalises them. The
 * arrays are row-major with cols*rows entries for cls/obj, *4 for bbox and *10
 * for kps, matching the model's output layout. */
size_t face_detect_decode_stride(struct face_detection *faces, size_t count, size_t max_faces, const float *cls,
				 const float *obj, const float *bbox, const float *kps, int stride, int cols, int rows,
				 float score_threshold);

/* Sorts by score and greedily suppresses overlapping boxes. Returns the number
 * left in `faces`, at most max_faces. */
size_t face_detect_nms(struct face_detection *faces, size_t count, float iou_threshold, size_t max_faces);

/* Least-squares similarity transform (rotation + uniform scale + translation)
 * from the five source landmarks to SFace's reference points. matrix is
 * [a, -b, tx, b, a, ty]. Returns false when the source points are degenerate. */
bool face_align_similarity(const float src[FACE_DETECT_LANDMARK_COUNT][2], float matrix[6]);

/* Warps the RGBA source into a 3-plane (RGB) float tensor of
 * FACE_ALIGN_SIZE x FACE_ALIGN_SIZE, values 0-255. Bilinear sampling. */
bool face_align_rgba_to_tensor(const uint8_t *rgba, size_t stride, uint32_t width, uint32_t height,
			       const float matrix[6], float *tensor);

void face_embedding_normalize(float *embedding, size_t count);
float face_embedding_similarity(const float *a, const float *b, size_t count);
