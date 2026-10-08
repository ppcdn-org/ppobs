#pragma once

#include <obs-module.h>
#include <string.h>

/* Pure, OBS-module-independent helpers factored out of audioshift-filter.c,
 * defined here as `static inline` (like libobs/util/deque.h) rather than out
 * of line in audioshift-filter.c, specifically so audioshift-filter-test.c
 * can exercise them without linking the rest of that file -- audioshift-
 * filter.c's create()/audioshift_name() call obs_module_text(), which is
 * only defined by the OBS_MODULE_USE_DEFAULT_LOCALE() macro invocation in
 * obs-filters.c, and pulling that whole module-registration file in just to
 * satisfy the linker would mean linking every other filter's .c file too. */

#define S_PRESET_CUSTOM "custom"
#define S_PRESET_MALE_TO_FEMALE "male_to_female"
#define S_PRESET_FEMALE_TO_MALE "female_to_male"
#define S_PRESET_CHILD "child"
#define S_PRESET_MONSTER "monster"

struct audioshift_preset {
	const char *id;
	float semitones;
};

/* Semitone offsets only; see docs/audioshift-design.md §6.1. SoundTouch does
 * not do formant correction (§3.2), so these deliberately sit in the
 * "obviously pitch-shifted, cartoonish" range documented for pitch-only
 * voice changers rather than the smaller natural-with-formant-correction
 * range -- without formant shifting, a small offset just sounds like the
 * same voice with a slight wobble, not a different character. Still
 * research-informed estimates, not ear-validated against real voices (no
 * audio playback in the environment these were written in); re-tune in
 * Stage C. */
static const struct audioshift_preset audioshift_presets[] = {
	{S_PRESET_MALE_TO_FEMALE, 9.0f},
	{S_PRESET_FEMALE_TO_MALE, -8.0f},
	{S_PRESET_CHILD, 12.0f},
	{S_PRESET_MONSTER, -12.0f},
};

static inline float audioshift_preset_semitones(const char *preset)
{
	for (size_t i = 0; i < sizeof(audioshift_presets) / sizeof(audioshift_presets[0]); i++) {
		if (strcmp(audioshift_presets[i].id, preset) == 0)
			return audioshift_presets[i].semitones;
	}
	return 0.0f;
}

static inline void audioshift_interleave(const struct obs_audio_data *audio, size_t channels, float *out)
{
	float **in = (float **)audio->data;

	for (uint32_t i = 0; i < audio->frames; i++) {
		for (size_t c = 0; c < channels; c++)
			out[i * channels + c] = in[c] ? in[c][i] : 0.0f;
	}
}

static inline void audioshift_deinterleave_channel(const float *interleaved, size_t channels, size_t channel,
						    uint32_t frames, float *out)
{
	for (uint32_t i = 0; i < frames; i++)
		out[i] = interleaved[i * channels + channel];
}
