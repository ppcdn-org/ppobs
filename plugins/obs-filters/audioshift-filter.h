#pragma once

#include <obs-module.h>
#include <math.h>
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

/* ---------------------------------------------------------------- */
/* Effect layers (P1; see docs/audioshift-design.md sections 6.2/7). The
 * effect runs after the pitch shift, on the filter's own deinterleaved output
 * buffer; these helpers hold the parts that don't depend on OBS or on any
 * per-call state, so audioshift-filter-test.c can cover them. */

#define S_EFFECT_NONE "none"
#define S_EFFECT_ROBOT "robot"
#define S_EFFECT_ECHO "echo"

enum audioshift_effect {
	AUDIOSHIFT_EFFECT_NONE = 0,
	AUDIOSHIFT_EFFECT_ROBOT,
	AUDIOSHIFT_EFFECT_ECHO,
};

static inline enum audioshift_effect audioshift_effect_from_string(const char *effect)
{
	if (effect) {
		if (strcmp(effect, S_EFFECT_ROBOT) == 0)
			return AUDIOSHIFT_EFFECT_ROBOT;
		if (strcmp(effect, S_EFFECT_ECHO) == 0)
			return AUDIOSHIFT_EFFECT_ECHO;
	}
	return AUDIOSHIFT_EFFECT_NONE;
}

/* Ring modulation: blend the dry signal with a copy multiplied by a low
 * frequency carrier. mix=0 is a bit-exact passthrough; because |carrier| <= 1
 * the result can never exceed the dry signal's own peak at any mix, so the
 * robot layer alone cannot clip (the design doc's "does P1 need a limiter"
 * question resolves to "no" for this layer). */
static inline float audioshift_robot_mix(float in, float carrier, float mix)
{
	return in * (1.0f - mix) + in * carrier * mix;
}

/* Transparent below +-knee, then approaches +-1 asymptotically, so a feedback
 * delay's repeats cannot run away into hard clipping. Only the echo layer
 * uses it. */
#define AUDIOSHIFT_SOFT_LIMIT_KNEE 0.8f

static inline float audioshift_soft_limit(float x)
{
	const float knee = AUDIOSHIFT_SOFT_LIMIT_KNEE;
	const float sign = x < 0.0f ? -1.0f : 1.0f;
	const float magnitude = x < 0.0f ? -x : x;

	if (magnitude < knee)
		return x;

	return sign * (knee + (1.0f - knee) * tanhf((magnitude - knee) / (1.0f - knee)));
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
