#include "avatar-engine-viseme.h"
#include "avatar-audio-tap.h"
#include "avatar-test-check.h"

#include <math.h>

static bool close_enough(float a, float b)
{
	return fabsf(a - b) < 0.01f;
}

static void test_target_gate(void)
{
	const float gate = -50.0f;
	const float full = -12.0f;

	/* Below the gate the mouth is shut, not slightly ajar: a mouth that
	 * never quite closes is the tell that an avatar is being driven by a
	 * noise floor rather than by speech. */
	CHECK(avatar_viseme_target(-120.0f, gate, full) == 0.0f);
	CHECK(avatar_viseme_target(-60.0f, gate, full) == 0.0f);
	CHECK(avatar_viseme_target(gate, gate, full) == 0.0f);

	/* At and above the full level it is all the way open and stays there -
	 * a shout must not push it past 1 and wrap or overshoot. */
	CHECK(avatar_viseme_target(full, gate, full) == 1.0f);
	CHECK(avatar_viseme_target(0.0f, gate, full) == 1.0f);

	/* Linear in dB between the two, which is what makes the mouth track
	 * loudness the way an ear hears it rather than the way a sample
	 * amplitude measures it. */
	CHECK(close_enough(avatar_viseme_target(-31.0f, gate, full), 0.5f));
	CHECK(close_enough(avatar_viseme_target(-40.5f, gate, full), 0.25f));
}

static void test_target_degenerate_range(void)
{
	/* Sensitivity sliders can be dragged until the gate meets or passes the
	 * full level. That must not divide by zero or invert the mouth: above
	 * the gate there is no range left to interpolate across, so it reads as
	 * fully open. */
	CHECK(avatar_viseme_target(-5.0f, -12.0f, -12.0f) == 1.0f);
	CHECK(avatar_viseme_target(-5.0f, -12.0f, -30.0f) == 1.0f);

	/* And still shut below the gate, whatever the full level says - note
	 * that -20 dBFS is quieter than a -12 dBFS gate, not louder. */
	CHECK(avatar_viseme_target(-20.0f, -12.0f, -12.0f) == 0.0f);
	CHECK(avatar_viseme_target(-60.0f, -12.0f, -30.0f) == 0.0f);
}

static void test_smooth_direction(void)
{
	const float window = 0.02f; /* 20 ms */

	/* Opening covers more ground per window than closing: a mouth that
	 * lags on the way open reads as out of sync, while one that snaps shut
	 * reads as mechanical. */
	const float opening = avatar_viseme_smooth(0.0f, 1.0f, window);
	const float closing = 1.0f - avatar_viseme_smooth(1.0f, 0.0f, window);
	CHECK(opening > 0.0f);
	CHECK(closing > 0.0f);
	CHECK(opening > closing);

	/* Never overshoots the target in one step. */
	CHECK(opening <= 1.0f);
	CHECK(avatar_viseme_smooth(1.0f, 0.0f, window) >= 0.0f);

	/* Already there, stays there. */
	CHECK(close_enough(avatar_viseme_smooth(0.5f, 0.5f, window), 0.5f));
}

static void test_smooth_converges(void)
{
	/* Held open, it has to actually arrive - a filter that approaches 1
	 * asymptotically but visibly short of it would cap the mouth. After a
	 * quarter second of attack at 25 ms the gap should be tiny. */
	float openness = 0.0f;
	for (int i = 0; i < 25; i++)
		openness = avatar_viseme_smooth(openness, 1.0f, 0.01f);
	CHECK(openness > 0.9f);

	for (int i = 0; i < 60; i++)
		openness = avatar_viseme_smooth(openness, 0.0f, 0.01f);
	CHECK(openness < 0.1f);
}

static void test_smooth_degenerate_window(void)
{
	/* A zero-length window has no time to filter over; jumping to the
	 * target is the only answer that is not a divide by zero. */
	CHECK(avatar_viseme_smooth(0.0f, 1.0f, 0.0f) == 1.0f);
	CHECK(avatar_viseme_smooth(1.0f, 0.0f, -1.0f) == 0.0f);
}

static void test_smooth_is_rate_independent(void)
{
	/* Two 10 ms windows must land in nearly the same place as one 20 ms
	 * window.  Without that, changing the audio chunk size would change
	 * how the mouth moves - and the chunk size is whatever the video tick
	 * happened to deliver. */
	float stepped = 0.0f;
	stepped = avatar_viseme_smooth(stepped, 1.0f, 0.01f);
	stepped = avatar_viseme_smooth(stepped, 1.0f, 0.01f);

	const float single = avatar_viseme_smooth(0.0f, 1.0f, 0.02f);
	CHECK(close_enough(stepped, single));
}

static void test_silence_threshold_is_usable_as_gate(void)
{
	/* The engine defaults its gate to the tap's silence threshold, so the
	 * two have to agree about what silence is. */
	CHECK(avatar_viseme_target(AVATAR_TAP_SILENCE_FLOOR_DB, AVATAR_TAP_SILENCE_DB, -12.0f) == 0.0f);
	CHECK(avatar_viseme_target(-6.0f, AVATAR_TAP_SILENCE_DB, -12.0f) == 1.0f);
}

static void test_window_size(void)
{
	/* 20 ms at 16 kHz is 320 samples. The engine's window buffer is sized
	 * from these constants, so a change that made them disagree would
	 * silently shorten the window. */
	const size_t window = (size_t)AVATAR_TAP_SAMPLE_RATE * AVATAR_VISEME_WINDOW_MS / 1000;
	CHECK(window == 320);
	CHECK(window <= AVATAR_TAP_SAMPLE_RATE / 50);
}

int main(void)
{
	test_target_gate();
	test_target_degenerate_range();
	test_smooth_direction();
	test_smooth_converges();
	test_smooth_degenerate_window();
	test_smooth_is_rate_independent();
	test_silence_threshold_is_usable_as_gate();
	test_window_size();
	return 0;
}
