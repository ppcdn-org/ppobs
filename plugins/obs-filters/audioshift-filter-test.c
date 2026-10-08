#include "audioshift-filter.h"

#include <assert.h>
#include <stdint.h>

static void test_preset_semitones(void)
{
	assert(audioshift_preset_semitones("male_to_female") == 9.0f);
	assert(audioshift_preset_semitones("female_to_male") == -8.0f);
	assert(audioshift_preset_semitones("child") == 12.0f);
	assert(audioshift_preset_semitones("monster") == -12.0f);

	/* Unknown/custom preset ids fall back to 0 rather than inheriting a
	 * stale value: audioshift-filter.c only consults this table when the
	 * preset isn't "custom", but a typo'd or future preset id must not
	 * silently resolve to someone else's offset. */
	assert(audioshift_preset_semitones("custom") == 0.0f);
	assert(audioshift_preset_semitones("not_a_real_preset") == 0.0f);
	assert(audioshift_preset_semitones("") == 0.0f);
}

static void test_interleave(void)
{
	float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	float ch1[4] = {10.0f, 20.0f, 30.0f, 40.0f};
	float out[8] = {0};

	struct obs_audio_data audio = {0};
	audio.data[0] = (uint8_t *)ch0;
	audio.data[1] = (uint8_t *)ch1;
	audio.frames = 4;

	audioshift_interleave(&audio, 2, out);

	const float expected[8] = {1.0f, 10.0f, 2.0f, 20.0f, 3.0f, 30.0f, 4.0f, 40.0f};
	for (size_t i = 0; i < 8; i++)
		assert(out[i] == expected[i]);
}

static void test_interleave_missing_channel_is_silent(void)
{
	/* audio->data[c] can be NULL for a channel OBS isn't delivering;
	 * interleave must not dereference it. */
	float ch0[2] = {5.0f, 6.0f};
	float out[4] = {0};

	struct obs_audio_data audio = {0};
	audio.data[0] = (uint8_t *)ch0;
	audio.data[1] = NULL;
	audio.frames = 2;

	audioshift_interleave(&audio, 2, out);

	assert(out[0] == 5.0f);
	assert(out[1] == 0.0f);
	assert(out[2] == 6.0f);
	assert(out[3] == 0.0f);
}

static void test_deinterleave_round_trip(void)
{
	float ch0[3] = {1.5f, 2.5f, 3.5f};
	float ch1[3] = {-1.0f, -2.0f, -3.0f};
	float interleaved[6] = {0};
	float back0[3] = {0};
	float back1[3] = {0};

	struct obs_audio_data audio = {0};
	audio.data[0] = (uint8_t *)ch0;
	audio.data[1] = (uint8_t *)ch1;
	audio.frames = 3;

	audioshift_interleave(&audio, 2, interleaved);
	audioshift_deinterleave_channel(interleaved, 2, 0, 3, back0);
	audioshift_deinterleave_channel(interleaved, 2, 1, 3, back1);

	for (size_t i = 0; i < 3; i++) {
		assert(back0[i] == ch0[i]);
		assert(back1[i] == ch1[i]);
	}
}

int main(void)
{
	test_preset_semitones();
	test_interleave();
	test_interleave_missing_channel_is_silent();
	test_deinterleave_round_trip();
	return 0;
}
