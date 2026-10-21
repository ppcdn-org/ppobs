/* Where the eyes are, as the worker found them.
 *
 * A lid is a parabola y = a*x'*x' + b*x' + c with x' = x - cx, in the pixels of
 * the worker's reference frame. That is all a lid is at the size of an avatar,
 * and it is three numbers for the shader instead of a polyline it would have to
 * walk. See AVATAR_MSG_EYES in avatar-sidecar-protocol.h for the wire form.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVATAR_EYES_CAPACITY 2

struct avatar_eye {
	float cx;    /* the mean x of the six landmarks, which x' is measured from */
	float left;  /* extent of the eye, from corner to corner */
	float right;
	float eh;    /* the widest the eye opens, in pixels */
	float up[3]; /* upper lid: a, b, c */
	float lo[3]; /* lower lid: a, b, c */
};

struct avatar_eyes {
	bool valid;
	uint32_t count;
	uint32_t ref_width;
	uint32_t ref_height;
	struct avatar_eye eye[AVATAR_EYES_CAPACITY];
};

#ifdef __cplusplus
}
#endif
