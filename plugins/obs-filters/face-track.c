#include "face-track.h"

#include <math.h>
#include <string.h>

#include <util/bmem.h>

/* Detection grid: every 4th pixel in both axes.  At the filter's 256px capture
 * this is a 64x64 grid, small enough that the whole scan is negligible next to
 * the readback itself. */
#define FACE_TRACK_STEP 4

/* Frames a lock survives without a new plausible blob. */
#define FACE_TRACK_HOLD 15

/* Dead-band + slow EMA, same idea as the beauty filter.  Sub-dead-band jitter
 * freezes the box instead of making the circle wobble every frame; the
 * dead-band is subtracted from the delta so the box still converges rather
 * than lagging a constant offset. */
#define FACE_TRACK_DEADBAND 0.004f
#define FACE_TRACK_ALPHA 0.35f

static inline bool is_skin(int y, int cb, int cr)
{
	return y > 30 && y < 250 && cb >= 74 && cb <= 130 && cr >= 132 && cr <= 178;
}

static inline float stabilize(float current, float target, float dead, float alpha)
{
	const float delta = target - current;

	if (fabsf(delta) <= dead)
		return current;

	return current + alpha * (delta - (delta > 0.0f ? dead : -dead));
}

static inline void rgba_to_ycbcr(uint8_t r, uint8_t g, uint8_t b, int *y, int *cb, int *cr)
{
	*y = (int)(0.299f * r + 0.587f * g + 0.114f * b);
	*cb = (int)(128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b);
	*cr = (int)(128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b);
}

void face_track_init(struct face_track *track)
{
	memset(track, 0, sizeof(*track));
}

void face_track_free(struct face_track *track)
{
	if (!track)
		return;

	bfree(track->skin);
	bfree(track->visited);
	bfree(track->queue);
	memset(track, 0, sizeof(*track));
}

static bool face_track_reserve(struct face_track *track, size_t count)
{
	if (track->capacity >= count)
		return true;

	uint8_t *skin = bmalloc(count);
	uint8_t *visited = bmalloc(count);
	uint32_t *queue = bmalloc(count * sizeof(uint32_t));

	if (!skin || !visited || !queue) {
		bfree(skin);
		bfree(visited);
		bfree(queue);
		return false;
	}

	bfree(track->skin);
	bfree(track->visited);
	bfree(track->queue);
	track->skin = skin;
	track->visited = visited;
	track->queue = queue;
	track->capacity = count;
	return true;
}

bool face_track_update_rgba(struct face_track *track, const uint8_t *rgba, size_t stride, uint32_t width,
			    uint32_t height)
{
	if (!track)
		return false;

	if (!rgba || !width || !height || stride < (size_t)width * 4)
		return track->present;

	const uint32_t gw = width / FACE_TRACK_STEP;
	const uint32_t gh = height / FACE_TRACK_STEP;

	if (!gw || !gh)
		return track->present;

	if (!face_track_reserve(track, (size_t)gw * gh))
		return track->present;

	if (track->grid_width != gw || track->grid_height != gh) {
		track->grid_width = gw;
		track->grid_height = gh;
	}

	const size_t grid_size = (size_t)gw * gh;

	for (uint32_t j = 0; j < gh; j++) {
		const uint8_t *row = rgba + (size_t)(j * FACE_TRACK_STEP) * stride;

		for (uint32_t i = 0; i < gw; i++) {
			const uint8_t *pixel = row + (size_t)(i * FACE_TRACK_STEP) * 4;
			int y, cb, cr;

			rgba_to_ycbcr(pixel[0], pixel[1], pixel[2], &y, &cb, &cr);
			track->skin[(size_t)j * gw + i] = is_skin(y, cb, cr);
		}
	}

	memset(track->visited, 0, grid_size);

	uint32_t best_count = 0;
	uint32_t best_minx = 0, best_miny = 0, best_maxx = 0, best_maxy = 0;

	for (uint32_t start = 0; start < grid_size; start++) {
		if (!track->skin[start] || track->visited[start])
			continue;

		uint32_t head = 0, tail = 0;
		uint32_t count = 0;
		uint32_t minx = gw, miny = gh, maxx = 0, maxy = 0;

		track->visited[start] = 1;
		track->queue[tail++] = start;

		while (head < tail) {
			const uint32_t idx = track->queue[head++];
			const uint32_t x = idx % gw;
			const uint32_t y = idx / gw;

			count++;
			if (x < minx)
				minx = x;
			if (x > maxx)
				maxx = x;
			if (y < miny)
				miny = y;
			if (y > maxy)
				maxy = y;

			const uint32_t neighbors[4] = {
				x ? idx - 1 : idx,
				x + 1 < gw ? idx + 1 : idx,
				y ? idx - gw : idx,
				y + 1 < gh ? idx + gw : idx,
			};

			for (int k = 0; k < 4; k++) {
				if (neighbors[k] != idx && track->skin[neighbors[k]] && !track->visited[neighbors[k]]) {
					track->visited[neighbors[k]] = 1;
					track->queue[tail++] = neighbors[k];
				}
			}
		}

		if (count > best_count) {
			best_count = count;
			best_minx = minx;
			best_miny = miny;
			best_maxx = maxx;
			best_maxy = maxy;
		}
	}

	const uint32_t bw = best_maxx - best_minx + 1;
	const uint32_t bh = best_maxy - best_miny + 1;
	const float short_side = (float)(width < height ? width : height);
	const float min_face_width = (track->present ? 0.10f : 0.12f) * short_side;
	const float density = bw && bh ? (float)best_count / ((float)bw * bh) : 0.0f;
	const float aspect = bw ? (float)bh / (float)bw : 0.0f;

	/* A face is a dense, roughly upright blob that is large enough to be
	 * worth tracking.  The loose aspect ceiling accommodates the neck and
	 * chest that skin tone usually drags into the blob. */
	const bool plausible = best_count >= 40 && best_count >= grid_size / 400 && bw >= 8 && bh >= 8 &&
			       density >= 0.30f && aspect >= 0.60f && aspect <= 3.0f &&
			       (float)(bw * FACE_TRACK_STEP) >= min_face_width;

	if (!plausible) {
		if (track->present && ++track->hold > FACE_TRACK_HOLD) {
			track->present = false;
			track->hold = 0;
		}
		return track->present;
	}

	const float rx = (float)(best_minx * FACE_TRACK_STEP) / (float)width;
	const float ry = (float)(best_miny * FACE_TRACK_STEP) / (float)height;
	const float rw = (float)(bw * FACE_TRACK_STEP) / (float)width;
	const float rh = (float)(bh * FACE_TRACK_STEP) / (float)height;

	if (!track->present || track->width <= 0.0f) {
		track->x = rx;
		track->y = ry;
		track->width = rw;
		track->height = rh;
		track->present = true;
	} else {
		track->x = stabilize(track->x, rx, FACE_TRACK_DEADBAND, FACE_TRACK_ALPHA);
		track->y = stabilize(track->y, ry, FACE_TRACK_DEADBAND, FACE_TRACK_ALPHA);
		track->width = stabilize(track->width, rw, FACE_TRACK_DEADBAND, FACE_TRACK_ALPHA);
		track->height = stabilize(track->height, rh, FACE_TRACK_DEADBAND, FACE_TRACK_ALPHA);
	}

	track->hold = 0;
	return true;
}
