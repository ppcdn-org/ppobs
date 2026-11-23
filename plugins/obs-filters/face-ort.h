#pragma once

#include "face-detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* ONNX Runtime wrapper for the YuNet detector and SFace embedder.  The struct
 * keeps the runtime opaque so the rest of the filter does not need the ORT
 * headers; face-ort.c casts the void pointers internally. */
struct face_ort {
	void *module;
	const void *api;
	void *env;
	void *memory_info;
	void *allocator;
	const char *version;
	bool sdk_available;

	void *detect_session;
	void *embed_session;

	char *detect_input_name;
	char *embed_input_name;
	char *embed_output_name;

	int detect_input_width;
	int detect_input_height;
	float *detect_input;
	size_t detect_input_count;
	float *embed_input;
	size_t embed_input_count;
};

bool face_ort_load(struct face_ort *ort, const wchar_t *detect_model, const wchar_t *embed_model);
void face_ort_unload(struct face_ort *ort);

/* Runs YuNet on a FACE_ORT_CANVAS x FACE_ORT_CANVAS RGBA canvas.  The actual
 * image occupies the top-left image_width x image_height region; the rest is
 * black padding.  Returned faces are normalised to that image region. */
size_t face_ort_detect(struct face_ort *ort, const uint8_t *rgba, size_t stride, uint32_t image_width,
		       uint32_t image_height, float score_threshold, float nms_threshold, struct face_detection *faces,
		       size_t max_faces);

/* Aligns one normalised detection from the same canvas and returns a
 * normalised 128-dimensional SFace embedding. */
bool face_ort_embed(struct face_ort *ort, const uint8_t *rgba, size_t stride, uint32_t image_width,
		    uint32_t image_height, const struct face_detection *face, float *embedding);

#define FACE_ORT_CANVAS 320
