#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lightweight skin-tone face tracker used by the big-head filter.
 *
 * This deliberately does not use a model: the existing beauty filter already
 * proves that the largest plausible skin blob is a good enough face box for
 * camera footage, and running it on a small readback costs well under a
 * millisecond.  The tracker only needs a box, not landmarks, so the whole
 * connected-component scan is kept to a coarse grid.
 *
 * The returned box is normalised to the input frame (0..1), so it maps onto
 * any source resolution without knowing the camera's native size.
 */

struct face_track {
	bool present;
	int hold;

	uint32_t grid_width;
	uint32_t grid_height;
	size_t capacity;
	uint8_t *skin;
	uint8_t *visited;
	uint32_t *queue;

	/* Smoothed face box, normalised to 0..1 of the input frame.  width stays
	 * zero until the first plausible detection, which lets the filter tell
	 * "never locked" apart from "lost after locking". */
	float x;
	float y;
	float width;
	float height;
};

void face_track_init(struct face_track *track);
void face_track_free(struct face_track *track);

/* Feeds one RGBA frame (row stride in bytes) and updates the tracked box.
 * Returns true while a face is considered present, including the short hold
 * period that bridges detection dropouts. */
bool face_track_update_rgba(struct face_track *track, const uint8_t *rgba, size_t stride, uint32_t width,
			    uint32_t height);
