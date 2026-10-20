#include "avatar-audio-tap.h"

#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <media-io/media-io-defs.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

/* Retry interval while the target source does not exist.  During scene load
 * the avatar source can come up before the microphone, so "not found" is a
 * normal state rather than an error.  Three seconds, as in the compressor's
 * sidechain. */
#define AVATAR_TAP_RETRY_NS 3000000000ULL

struct avatar_audio_tap {
	obs_source_t *owner;

	/* Name and weak reference.  Attaching and detaching happen outside the
	 * lock: obs_source_remove_audio_capture_callback waits for a callback
	 * already in flight, so holding this while calling it invites a
	 * deadlock against the audio thread. */
	pthread_mutex_t update_mutex;
	char *name;
	obs_weak_source_t *weak;
	uint64_t retry_time;

	/* Resampler, ring buffer and interval levels: written by the audio
	 * thread, read by the avatar worker. */
	pthread_mutex_t data_mutex;
	audio_resampler_t *resampler;
	uint32_t in_rate;
	enum speaker_layout in_speakers;
	struct avatar_pcm_ring ring;

	float *silence;
	size_t silence_frames;

	float peak;
	double sum_sq;
	uint64_t level_samples;

	bool warned_resampler;
};

static inline obs_source_t *avatar_tap_get_source(struct avatar_audio_tap *tap)
{
	return tap->weak ? obs_weak_source_get_source(tap->weak) : NULL;
}

/* Takes a block of 16 kHz mono samples into the buffer and folds it into the
 * interval levels.  data_mutex is held. */
static void avatar_tap_consume(struct avatar_audio_tap *tap, const float *mono, size_t count)
{
	if (!count)
		return;

	const float peak = avatar_block_peak(mono, count);
	if (peak > tap->peak)
		tap->peak = peak;

	for (size_t i = 0; i < count; i++)
		tap->sum_sq += (double)mono[i] * (double)mono[i];
	tap->level_samples += count;

	avatar_pcm_ring_push(&tap->ring, mono, count);
}

/* Keeps the resampler in step with the current mix format, rebuilding it when
 * the rate or speaker layout changes under us (the user edited OBS's audio
 * settings).  NULL means no resampling is needed and the caller should take
 * the mono fast path - or, if creation failed, that there is nothing to take. */
static audio_resampler_t *avatar_tap_ensure_resampler(struct avatar_audio_tap *tap,
						      const struct audio_output_info *aoi)
{
	if (aoi->samples_per_sec == AVATAR_TAP_SAMPLE_RATE && aoi->speakers == SPEAKERS_MONO)
		return NULL;

	if (tap->resampler && tap->in_rate == aoi->samples_per_sec && tap->in_speakers == aoi->speakers)
		return tap->resampler;

	if (tap->resampler) {
		audio_resampler_destroy(tap->resampler);
		tap->resampler = NULL;
	}

	const struct resample_info src = {
		.samples_per_sec = aoi->samples_per_sec,
		/* OBS's internal mix format.  What the capture callback sees has
		 * already been converted to it by process_audio, whatever the
		 * source itself captures in. */
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = aoi->speakers,
	};
	const struct resample_info dst = {
		.samples_per_sec = AVATAR_TAP_SAMPLE_RATE,
		.format = AUDIO_FORMAT_FLOAT,
		.speakers = SPEAKERS_MONO,
	};

	tap->resampler = audio_resampler_create(&dst, &src);
	tap->in_rate = aoi->samples_per_sec;
	tap->in_speakers = aoi->speakers;

	if (!tap->resampler && !tap->warned_resampler) {
		/* Once only: this will not recover on its own, and a line per
		 * audio block would flood the log. */
		tap->warned_resampler = true;
		blog(LOG_WARNING,
		     "[obs-avatar: '%s'] failed to create a %u Hz/%d channel -> 16 kHz mono resampler; "
		     "lip sync will not be driven",
		     obs_source_get_name(tap->owner), aoi->samples_per_sec, (int)aoi->speakers);
	}

	return tap->resampler;
}

/* Feeds silence of the same length rather than skipping a muted block: the
 * resampler's internal delay advances with what is fed to it, so a gap would
 * shift everything after it earlier in time. */
static const float *avatar_tap_silence(struct avatar_audio_tap *tap, size_t frames)
{
	if (tap->silence_frames < frames) {
		tap->silence = brealloc(tap->silence, frames * sizeof(float));
		tap->silence_frames = frames;
	}
	memset(tap->silence, 0, frames * sizeof(float));
	return tap->silence;
}

static void avatar_tap_capture(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	struct avatar_audio_tap *tap = param;

	UNUSED_PARAMETER(source);

	if (!audio->frames)
		return;

	const audio_t *obs_audio = obs_get_audio();
	if (!obs_audio)
		return;

	const struct audio_output_info *aoi = audio_output_get_info(obs_audio);
	if (!aoi)
		return;

	const size_t channels = audio_output_get_channels(obs_audio);
	if (!channels)
		return;

	pthread_mutex_lock(&tap->data_mutex);

	audio_resampler_t *resampler = avatar_tap_ensure_resampler(tap, aoi);

	if (!resampler) {
		/* The mix is already 16 kHz mono (unusual but legal), so take it
		 * as-is.  Resampler creation failure lands here too, which is
		 * why the format is re-checked rather than assumed. */
		if (aoi->samples_per_sec == AVATAR_TAP_SAMPLE_RATE && aoi->speakers == SPEAKERS_MONO) {
			const float *mono = muted ? avatar_tap_silence(tap, audio->frames)
						  : (const float *)audio->data[0];
			if (mono)
				avatar_tap_consume(tap, mono, audio->frames);
		}
	} else {
		const uint8_t *planes[MAX_AV_PLANES] = {0};
		const uint8_t *zeros = muted ? (const uint8_t *)avatar_tap_silence(tap, audio->frames) : NULL;

		/* Planar and read-only, so every muted channel can point at the
		 * same block of zeroes. */
		for (size_t i = 0; i < channels && i < MAX_AV_PLANES; i++)
			planes[i] = muted ? zeros : audio->data[i];

		uint8_t *out[MAX_AV_PLANES] = {0};
		uint32_t out_frames = 0;
		uint64_t ts_offset = 0;

		if (audio_resampler_resample(resampler, out, &out_frames, &ts_offset,
					     (const uint8_t *const *)planes, audio->frames) &&
		    out[0])
			avatar_tap_consume(tap, (const float *)out[0], out_frames);
	}

	pthread_mutex_unlock(&tap->data_mutex);
}

struct avatar_audio_tap *avatar_audio_tap_create(struct obs_source *owner)
{
	struct avatar_audio_tap *tap = bzalloc(sizeof(*tap));
	tap->owner = owner;

	if (pthread_mutex_init(&tap->update_mutex, NULL) != 0) {
		bfree(tap);
		return NULL;
	}

	if (pthread_mutex_init(&tap->data_mutex, NULL) != 0) {
		pthread_mutex_destroy(&tap->update_mutex);
		bfree(tap);
		return NULL;
	}

	avatar_pcm_ring_init(&tap->ring, AVATAR_TAP_SAMPLE_RATE * AVATAR_TAP_BUFFER_MS / 1000);
	return tap;
}

void avatar_audio_tap_destroy(struct avatar_audio_tap *tap)
{
	if (!tap)
		return;

	/* Detach before tearing anything down.  Once remove returns, no new
	 * callback can start and the one that may have been running has
	 * finished - libobs walks the callback list under audio_cb_mutex. */
	obs_source_t *source = avatar_tap_get_source(tap);
	if (source) {
		obs_source_remove_audio_capture_callback(source, avatar_tap_capture, tap);
		obs_source_release(source);
	}
	obs_weak_source_release(tap->weak);

	avatar_pcm_ring_free(&tap->ring);
	audio_resampler_destroy(tap->resampler);
	bfree(tap->silence);
	bfree(tap->name);

	pthread_mutex_destroy(&tap->data_mutex);
	pthread_mutex_destroy(&tap->update_mutex);
	bfree(tap);
}

void avatar_audio_tap_set_source(struct avatar_audio_tap *tap, const char *name)
{
	const bool valid = name && *name && strcmp(name, "none") != 0;
	obs_weak_source_t *old_weak = NULL;

	pthread_mutex_lock(&tap->update_mutex);

	if (!valid) {
		old_weak = tap->weak;
		tap->weak = NULL;
		bfree(tap->name);
		tap->name = NULL;
	} else if (!tap->name || strcmp(tap->name, name) != 0) {
		old_weak = tap->weak;
		tap->weak = NULL;
		bfree(tap->name);
		tap->name = bstrdup(name);
		/* Attach on the next tick instead of after a retry interval. */
		tap->retry_time = os_gettime_ns() - AVATAR_TAP_RETRY_NS;
	}

	pthread_mutex_unlock(&tap->update_mutex);

	if (old_weak) {
		obs_source_t *old = obs_weak_source_get_source(old_weak);
		if (old) {
			obs_source_remove_audio_capture_callback(old, avatar_tap_capture, tap);
			obs_source_release(old);
		}
		obs_weak_source_release(old_weak);
	}

	/* Whatever is left in the buffer belongs to the previous source; keeping
	 * it would drive the mouth from audio nobody is hearing. */
	pthread_mutex_lock(&tap->data_mutex);
	avatar_pcm_ring_reset(&tap->ring);
	tap->peak = 0.0f;
	tap->sum_sq = 0.0;
	tap->level_samples = 0;
	pthread_mutex_unlock(&tap->data_mutex);
}

void avatar_audio_tap_tick(struct avatar_audio_tap *tap)
{
	char *pending = NULL;

	pthread_mutex_lock(&tap->update_mutex);

	if (tap->name && !tap->weak) {
		const uint64_t now = os_gettime_ns();
		if (now - tap->retry_time > AVATAR_TAP_RETRY_NS) {
			pending = bstrdup(tap->name);
			tap->retry_time = now;
		}
	}

	pthread_mutex_unlock(&tap->update_mutex);

	if (!pending)
		return;

	obs_source_t *source = obs_get_source_by_name(pending);
	obs_weak_source_t *weak = source ? obs_source_get_weak_source(source) : NULL;
	bool attach = false;

	pthread_mutex_lock(&tap->update_mutex);

	/* The name may have changed again while the lookup ran, in which case
	 * what was just found is dropped: attaching to a source that is no
	 * longer the target would leave a callback nothing can remove. */
	if (weak && tap->name && strcmp(tap->name, pending) == 0 && !tap->weak) {
		tap->weak = weak;
		weak = NULL;
		attach = true;
	}

	pthread_mutex_unlock(&tap->update_mutex);

	if (attach)
		obs_source_add_audio_capture_callback(source, avatar_tap_capture, tap);

	obs_weak_source_release(weak);
	obs_source_release(source);
	bfree(pending);
}

bool avatar_audio_tap_attached(struct avatar_audio_tap *tap)
{
	pthread_mutex_lock(&tap->update_mutex);
	const bool attached = tap->weak != NULL;
	pthread_mutex_unlock(&tap->update_mutex);
	return attached;
}

size_t avatar_audio_tap_read(struct avatar_audio_tap *tap, float *out, size_t count)
{
	pthread_mutex_lock(&tap->data_mutex);
	const size_t read = avatar_pcm_ring_read(&tap->ring, out, count);
	pthread_mutex_unlock(&tap->data_mutex);
	return read;
}

bool avatar_audio_tap_take_level(struct avatar_audio_tap *tap, float *peak_db, float *rms_db, uint64_t *dropped)
{
	pthread_mutex_lock(&tap->data_mutex);

	const uint64_t samples = tap->level_samples;
	const float peak = tap->peak;
	const double sum_sq = tap->sum_sq;
	const uint64_t drops = tap->ring.dropped;

	tap->peak = 0.0f;
	tap->sum_sq = 0.0;
	tap->level_samples = 0;
	tap->ring.dropped = 0;

	pthread_mutex_unlock(&tap->data_mutex);

	if (peak_db)
		*peak_db = avatar_amplitude_to_db(peak);
	if (rms_db)
		*rms_db = avatar_amplitude_to_db(samples ? (float)sqrt(sum_sq / (double)samples) : 0.0f);
	if (dropped)
		*dropped = drops;

	return samples != 0;
}
