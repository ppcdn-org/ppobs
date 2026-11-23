#include "face-detect.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float box_iou(const struct face_detection *a, const struct face_detection *b)
{
	const float ax2 = a->x + a->width;
	const float ay2 = a->y + a->height;
	const float bx2 = b->x + b->width;
	const float by2 = b->y + b->height;

	const float ix1 = a->x > b->x ? a->x : b->x;
	const float iy1 = a->y > b->y ? a->y : b->y;
	const float ix2 = ax2 < bx2 ? ax2 : bx2;
	const float iy2 = ay2 < by2 ? ay2 : by2;

	const float iw = ix2 - ix1;
	const float ih = iy2 - iy1;
	if (iw <= 0.0f || ih <= 0.0f)
		return 0.0f;

	const float intersection = iw * ih;
	const float union_area = a->width * a->height + b->width * b->height - intersection;
	return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static int compare_score_desc(const void *lhs, const void *rhs)
{
	const struct face_detection *a = lhs;
	const struct face_detection *b = rhs;

	if (a->score < b->score)
		return 1;
	if (a->score > b->score)
		return -1;
	return 0;
}

size_t face_detect_decode_stride(struct face_detection *faces, size_t count, size_t max_faces, const float *cls,
				 const float *obj, const float *bbox, const float *kps, int stride, int cols, int rows,
				 float score_threshold)
{
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if (count >= max_faces)
				return count;

			const size_t idx = (size_t)r * cols + c;
			const float cls_score = clampf(cls[idx], 0.0f, 1.0f);
			const float obj_score = clampf(obj[idx], 0.0f, 1.0f);
			const float score = sqrtf(cls_score * obj_score);

			if (score < score_threshold)
				continue;

			struct face_detection *face = &faces[count];

			const float cx = ((float)c + bbox[idx * 4 + 0]) * stride;
			const float cy = ((float)r + bbox[idx * 4 + 1]) * stride;
			const float w = expf(bbox[idx * 4 + 2]) * stride;
			const float h = expf(bbox[idx * 4 + 3]) * stride;

			face->x = cx - w * 0.5f;
			face->y = cy - h * 0.5f;
			face->width = w;
			face->height = h;
			face->score = score;

			for (int n = 0; n < FACE_DETECT_LANDMARK_COUNT; n++) {
				face->landmarks[n][0] = (kps[idx * 10 + n * 2] + (float)c) * stride;
				face->landmarks[n][1] = (kps[idx * 10 + n * 2 + 1] + (float)r) * stride;
			}

			count++;
		}
	}

	return count;
}

size_t face_detect_nms(struct face_detection *faces, size_t count, float iou_threshold, size_t max_faces)
{
	if (count <= 1 || max_faces <= 1)
		return count;

	qsort(faces, count, sizeof(*faces), compare_score_desc);

	size_t kept = 0;
	for (size_t i = 0; i < count && kept < max_faces; i++) {
		bool keep = true;

		for (size_t j = 0; j < kept; j++) {
			if (box_iou(&faces[i], &faces[j]) > iou_threshold) {
				keep = false;
				break;
			}
		}

		if (keep) {
			if (kept != i)
				faces[kept] = faces[i];
			kept++;
		}
	}

	return kept;
}

bool face_align_similarity(const float src[FACE_DETECT_LANDMARK_COUNT][2], float matrix[6])
{
	static const float dst[FACE_DETECT_LANDMARK_COUNT][2] = {
		{38.2946f, 51.6963f}, {73.5318f, 51.5014f}, {56.0252f, 71.7366f},
		{41.5493f, 92.3655f}, {70.7299f, 92.2041f},
	};

	float src_mean_x = 0.0f, src_mean_y = 0.0f;
	float dst_mean_x = 0.0f, dst_mean_y = 0.0f;

	for (int i = 0; i < FACE_DETECT_LANDMARK_COUNT; i++) {
		src_mean_x += src[i][0];
		src_mean_y += src[i][1];
		dst_mean_x += dst[i][0];
		dst_mean_y += dst[i][1];
	}

	src_mean_x /= FACE_DETECT_LANDMARK_COUNT;
	src_mean_y /= FACE_DETECT_LANDMARK_COUNT;
	dst_mean_x /= FACE_DETECT_LANDMARK_COUNT;
	dst_mean_y /= FACE_DETECT_LANDMARK_COUNT;

	float dot = 0.0f;
	float cross = 0.0f;
	float norm = 0.0f;

	for (int i = 0; i < FACE_DETECT_LANDMARK_COUNT; i++) {
		const float sx = src[i][0] - src_mean_x;
		const float sy = src[i][1] - src_mean_y;
		const float dx = dst[i][0] - dst_mean_x;
		const float dy = dst[i][1] - dst_mean_y;

		dot += dx * sx + dy * sy;
		cross += dx * sy - dy * sx;
		norm += sx * sx + sy * sy;
	}

	if (norm < 1e-6f)
		return false;

	const float a = dot / norm;
	const float b = cross / norm;

	matrix[0] = a;
	matrix[1] = -b;
	matrix[2] = dst_mean_x - (a * src_mean_x - b * src_mean_y);
	matrix[3] = b;
	matrix[4] = a;
	matrix[5] = dst_mean_y - (b * src_mean_x + a * src_mean_y);

	return true;
}

static inline void sample_rgba(const uint8_t *rgba, size_t stride, uint32_t width, uint32_t height, float x, float y,
			       float out[3])
{
	if (x < 0.0f)
		x = 0.0f;
	if (y < 0.0f)
		y = 0.0f;
	if (x > (float)(width - 1))
		x = (float)(width - 1);
	if (y > (float)(height - 1))
		y = (float)(height - 1);

	const int x0 = (int)floorf(x);
	const int y0 = (int)floorf(y);
	const int x1 = x0 + 1 < (int)width ? x0 + 1 : x0;
	const int y1 = y0 + 1 < (int)height ? y0 + 1 : y0;
	const float fx = x - (float)x0;
	const float fy = y - (float)y0;

	const uint8_t *p00 = rgba + (size_t)y0 * stride + (size_t)x0 * 4;
	const uint8_t *p10 = rgba + (size_t)y0 * stride + (size_t)x1 * 4;
	const uint8_t *p01 = rgba + (size_t)y1 * stride + (size_t)x0 * 4;
	const uint8_t *p11 = rgba + (size_t)y1 * stride + (size_t)x1 * 4;

	for (int c = 0; c < 3; c++) {
		const float top = (float)p00[c] + ((float)p10[c] - (float)p00[c]) * fx;
		const float bottom = (float)p01[c] + ((float)p11[c] - (float)p01[c]) * fx;
		out[c] = top + (bottom - top) * fy;
	}
}

bool face_align_rgba_to_tensor(const uint8_t *rgba, size_t stride, uint32_t width, uint32_t height,
			       const float matrix[6], float *tensor)
{
	if (!rgba || !tensor || !width || !height)
		return false;

	const float a = matrix[0];
	const float b = matrix[3];
	const float tx = matrix[2];
	const float ty = matrix[5];
	const float det = a * a + b * b;

	if (det < 1e-9f)
		return false;

	const size_t plane = (size_t)FACE_ALIGN_SIZE * FACE_ALIGN_SIZE;

	for (int v = 0; v < FACE_ALIGN_SIZE; v++) {
		for (int u = 0; u < FACE_ALIGN_SIZE; u++) {
			const float dx = (float)u - tx;
			const float dy = (float)v - ty;
			const float sx = (a * dx + b * dy) / det;
			const float sy = (a * dy - b * dx) / det;
			float rgb[3];

			sample_rgba(rgba, stride, width, height, sx, sy, rgb);

			const size_t index = (size_t)v * FACE_ALIGN_SIZE + u;
			tensor[index] = rgb[0];
			tensor[plane + index] = rgb[1];
			tensor[plane * 2 + index] = rgb[2];
		}
	}

	return true;
}

void face_embedding_normalize(float *embedding, size_t count)
{
	float sum = 0.0f;

	for (size_t i = 0; i < count; i++)
		sum += embedding[i] * embedding[i];

	const float norm = sqrtf(sum);
	if (norm < 1e-9f)
		return;

	const float inv = 1.0f / norm;
	for (size_t i = 0; i < count; i++)
		embedding[i] *= inv;
}

float face_embedding_similarity(const float *a, const float *b, size_t count)
{
	float dot = 0.0f;
	float na = 0.0f;
	float nb = 0.0f;

	for (size_t i = 0; i < count; i++) {
		dot += a[i] * b[i];
		na += a[i] * a[i];
		nb += b[i] * b[i];
	}

	const float denom = sqrtf(na * nb);
	return denom > 1e-9f ? dot / denom : 0.0f;
}
