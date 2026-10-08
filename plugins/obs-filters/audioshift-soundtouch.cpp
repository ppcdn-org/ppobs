#include "audioshift-soundtouch.h"

#if defined(AUDIOSHIFT_HAS_SOUNDTOUCH)

#include <SoundTouch.h>

struct audioshift_soundtouch {
	soundtouch::SoundTouch st;
};

bool audioshift_soundtouch_available(void)
{
	return true;
}

audioshift_soundtouch_t *audioshift_soundtouch_create(uint32_t sample_rate, size_t channels)
{
	audioshift_soundtouch_t *wrap = new audioshift_soundtouch();

	/* setTempo stays at the default 1.0 (unchanged): AudioShift only shifts
	 * pitch, it must never alter playback speed. */
	wrap->st.setSampleRate(sample_rate);
	wrap->st.setChannels((uint)channels);
	wrap->st.setPitchSemiTones(0.0f);
	return wrap;
}

void audioshift_soundtouch_destroy(audioshift_soundtouch_t *st)
{
	delete st;
}

void audioshift_soundtouch_set_pitch_semitones(audioshift_soundtouch_t *st, float semitones)
{
	st->st.setPitchSemiTones(semitones);
}

void audioshift_soundtouch_put_samples(audioshift_soundtouch_t *st, const float *samples, uint32_t frames)
{
	st->st.putSamples(samples, frames);
}

uint32_t audioshift_soundtouch_receive_samples(audioshift_soundtouch_t *st, float *out_samples, uint32_t max_frames)
{
	return st->st.receiveSamples(out_samples, max_frames);
}

void audioshift_soundtouch_flush(audioshift_soundtouch_t *st)
{
	st->st.clear();
}

#else /* !AUDIOSHIFT_HAS_SOUNDTOUCH */

/* No SoundTouch at build time: every entry point is a harmless no-op and
 * audioshift-filter.c passes audio through untouched. See
 * docs/audioshift-design.md §3.3/§12 for why this isn't vendored yet. */

bool audioshift_soundtouch_available(void)
{
	return false;
}

audioshift_soundtouch_t *audioshift_soundtouch_create(uint32_t sample_rate, size_t channels)
{
	(void)sample_rate;
	(void)channels;
	return nullptr;
}

void audioshift_soundtouch_destroy(audioshift_soundtouch_t *st)
{
	(void)st;
}

void audioshift_soundtouch_set_pitch_semitones(audioshift_soundtouch_t *st, float semitones)
{
	(void)st;
	(void)semitones;
}

void audioshift_soundtouch_put_samples(audioshift_soundtouch_t *st, const float *samples, uint32_t frames)
{
	(void)st;
	(void)samples;
	(void)frames;
}

uint32_t audioshift_soundtouch_receive_samples(audioshift_soundtouch_t *st, float *out_samples, uint32_t max_frames)
{
	(void)st;
	(void)out_samples;
	(void)max_frames;
	return 0;
}

void audioshift_soundtouch_flush(audioshift_soundtouch_t *st)
{
	(void)st;
}

#endif
