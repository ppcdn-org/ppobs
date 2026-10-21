/* Mode B: the mouth is opened on the artwork the user already chose.
 *
 * MuseTalk cannot do animal avatars - its preprocessing requires face
 * detection and face parsing, so a photograph of a cat either fails detection
 * or has its "mouth region" repainted as a human mouth. That is a dead end
 * rather than a tuning problem, which is why this is a separate engine and not
 * a setting on the other one.
 *
 * What this delivers is a jaw that opens with the voice, not distinct vowel
 * shapes. Telling A from O needs either drawn viseme sprites (which only exist
 * if someone drew them) or a classifier; both are increments on top of this.
 * Opening and closing in time with speech is the part that reads as talking.
 *
 * The openness maths is static inline here so avatar-viseme-test.c can check
 * it without linking the engine, which calls obs_* and touches the graphics
 * context. Same arrangement as avatar-audio-tap.h.
 */

#pragma once

#include "avatar-engine.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio is examined in windows this long. 20 ms is short enough to track
 * syllables and long enough that a single glottal pulse does not read as a
 * word. */
#define AVATAR_VISEME_WINDOW_MS 20

/* The mouth opens quickly and closes a little more slowly: a mouth that snaps
 * shut the instant a word ends looks mechanical, while one that lags on the
 * way open looks out of sync. */
#define AVATAR_VISEME_ATTACK_MS 25.0f
#define AVATAR_VISEME_RELEASE_MS 90.0f

/* Where the mouth is on the artwork, and how far the jaw travels. All in
 * fractions of the image, so the numbers survive the asset being re-exported
 * at another resolution. */
struct avatar_viseme_mouth {
	float center_x;
	float center_y;
	float radius_x;
	float radius_y;
	float travel;
	/* Level at which the mouth is shut, and the level at which it is fully
	 * open, in dBFS. */
	float gate_db;
	float full_db;
	/* How strongly the teeth and the tongue show inside the opening, 0..1.
	 * An open mouth is recognised by its teeth before anything else, so
	 * these are what make the gap read as a mouth rather than a smudge -
	 * but a long-muzzled animal or a flat cartoon can want much less of
	 * them than a face-on portrait does. */
	float teeth;
	float tongue;
	/* Outline the region while it is being positioned. */
	bool show_region;
};

/* Maps a window's level onto how far the mouth should be open.
 *
 * Below the gate the mouth is shut rather than slightly ajar: a mouth that
 * never quite closes is the tell that an avatar is driven by a noise floor. */
static inline float avatar_viseme_target(float peak_db, float gate_db, float full_db)
{
	if (peak_db <= gate_db)
		return 0.0f;
	if (full_db <= gate_db)
		return 1.0f;
	if (peak_db >= full_db)
		return 1.0f;

	return (peak_db - gate_db) / (full_db - gate_db);
}

/* One-pole smoothing with separate opening and closing speeds.
 *
 * `seconds` is the window's own duration rather than a wall-clock delta, so
 * the motion is tied to the audio it came from and does not change shape when
 * the video frame rate does. */
static inline float avatar_viseme_smooth(float current, float target, float seconds)
{
	const float tau_ms = (target > current) ? AVATAR_VISEME_ATTACK_MS : AVATAR_VISEME_RELEASE_MS;
	const float tau = tau_ms / 1000.0f;

	if (tau <= 0.0f || seconds <= 0.0f)
		return target;

	/* 1 - exp(-dt/tau): the fraction of the remaining distance to cover in
	 * this window, independent of how long the window happens to be. */
	const float coeff = 1.0f - expf(-seconds / tau);
	return current + (target - current) * coeff;
}

extern const struct avatar_engine_ops avatar_engine_viseme_ops;

/* The mouth region is adjusted live while the user drags the sliders, so it is
 * set on the running engine rather than carried in the config at creation.
 * Mode-B specific, hence not in avatar_engine_ops. */
void avatar_engine_viseme_set_mouth(void *data, const struct avatar_viseme_mouth *mouth);

/* How far open the mouth currently is, for logging. */
float avatar_engine_viseme_openness(void *data);

/* The parameters the engine is actually handing the shader. Worth being able
 * to read back rather than inferring from the settings: "I see the opening but
 * no teeth" is indistinguishable on screen from a teeth value that never
 * arrived, and one of those is a bug while the other is a slider. */
void avatar_engine_viseme_get_mouth(void *data, struct avatar_viseme_mouth *out);

#ifdef __cplusplus
}
#endif
