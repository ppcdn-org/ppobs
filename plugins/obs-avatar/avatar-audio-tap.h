/* Taps a named audio source's post-filter audio and hands it over as the
 * 16 kHz mono the lip-sync engines want.
 *
 * The tap is obs_source_add_audio_capture_callback(), the same mechanism
 * obs-filters' compressor uses for its sidechain (compressor-filter.c).  What
 * matters here is where that callback fires: source_output_audio_data() in
 * libobs/obs-source.c, which runs *after* filter_async_audio().  So the audio
 * this sees has already been through every filter on that source - including
 * AudioShift - and is the audio actually being streamed.  Lip sync has to
 * match what the audience hears, so that lands for free.
 *
 * The first half of this header (ring buffer, level maths) is deliberately
 * static inline so avatar-audio-tap-test.c does not have to link
 * avatar-audio-tap.c, which calls obs_* and blog() and would drag the whole
 * module into the test.  Same reasoning as obs-filters' audioshift-filter.h.
 */

#pragma once

#include <util/deque.h>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* whisper's required input rate.  Mode B's viseme decision runs on the same
 * audio, so there is no reason for a second path at a different rate. */
#define AVATAR_TAP_SAMPLE_RATE 16000

/* Ring buffer capacity.  A second is far more than any single frame of lip
 * sync needs (mode A's whisper window is in the hundreds of milliseconds); the
 * headroom is there to absorb jitter between the OBS audio thread and the
 * avatar worker, not to accumulate a backlog. */
#define AVATAR_TAP_BUFFER_MS 1000

/* Below this level nobody is talking.  -50 dBFS sits a little above the noise
 * floor of a microphone in a quiet room. */
#define AVATAR_TAP_SILENCE_DB -50.0f

/* Reported in place of -inf for digital silence; see avatar_amplitude_to_db. */
#define AVATAR_TAP_SILENCE_FLOOR_DB -120.0f

/* ---------------------------------------------------------------------------
 * Ring buffer: 16 kHz mono float
 *
 * Overflow drops the oldest samples rather than refusing the newest.  A live
 * stream would rather lose a stale moment of audio than let the mouth fall
 * further and further behind the voice: once a backlog starts growing there is
 * no way to catch back up.
 * ------------------------------------------------------------------------- */

struct avatar_pcm_ring {
	struct deque pcm;
	size_t capacity; /* samples */
	uint64_t dropped;
};

static inline void avatar_pcm_ring_init(struct avatar_pcm_ring *ring, size_t capacity)
{
	deque_init(&ring->pcm);
	ring->capacity = capacity;
	ring->dropped = 0;
}

static inline void avatar_pcm_ring_free(struct avatar_pcm_ring *ring)
{
	deque_free(&ring->pcm);
	ring->capacity = 0;
	ring->dropped = 0;
}

static inline size_t avatar_pcm_ring_available(const struct avatar_pcm_ring *ring)
{
	return ring->pcm.size / sizeof(float);
}

static inline void avatar_pcm_ring_reset(struct avatar_pcm_ring *ring)
{
	if (ring->pcm.size)
		deque_pop_front(&ring->pcm, NULL, ring->pcm.size);
}

static inline void avatar_pcm_ring_push(struct avatar_pcm_ring *ring, const float *samples, size_t count)
{
	if (!count || !ring->capacity)
		return;

	/* A single push larger than the whole buffer: keep only its tail, as
	 * everything before that is already superseded. */
	if (count >= ring->capacity) {
		ring->dropped += avatar_pcm_ring_available(ring) + (count - ring->capacity);
		avatar_pcm_ring_reset(ring);
		samples += count - ring->capacity;
		count = ring->capacity;
	} else {
		const size_t have = avatar_pcm_ring_available(ring);
		if (have + count > ring->capacity) {
			const size_t drop = have + count - ring->capacity;
			deque_pop_front(&ring->pcm, NULL, drop * sizeof(float));
			ring->dropped += drop;
		}
	}

	deque_push_back(&ring->pcm, samples, count * sizeof(float));
}

/* Takes up to `count` samples, returning how many were actually taken. */
static inline size_t avatar_pcm_ring_read(struct avatar_pcm_ring *ring, float *out, size_t count)
{
	const size_t have = avatar_pcm_ring_available(ring);
	if (count > have)
		count = have;
	if (count)
		deque_pop_front(&ring->pcm, out, count * sizeof(float));
	return count;
}

/* ---------------------------------------------------------------------------
 * Levels
 *
 * Stage A only uses these to show the tap works - the logged level tracks
 * speech.  Mode B's viseme decision will derive mouth opening from the same
 * numbers, so they are written in the shape that path needs.
 * ------------------------------------------------------------------------- */

static inline float avatar_block_peak(const float *samples, size_t count)
{
	float peak = 0.0f;
	for (size_t i = 0; i < count; i++) {
		const float mag = fabsf(samples[i]);
		if (mag > peak)
			peak = mag;
	}
	return peak;
}

static inline float avatar_block_rms(const float *samples, size_t count)
{
	if (!count)
		return 0.0f;

	double sum = 0.0;
	for (size_t i = 0; i < count; i++)
		sum += (double)samples[i] * (double)samples[i];

	return (float)sqrt(sum / (double)count);
}

/* Amplitude to dBFS.  Zero would send log10 to -inf, which formats as "-1.#J"
 * and reads in the log like a different failure, so it is clamped to a floor
 * that means "silent" instead. */
static inline float avatar_amplitude_to_db(float amplitude)
{
	if (amplitude <= 1.0e-6f)
		return AVATAR_TAP_SILENCE_FLOOR_DB;
	return 20.0f * log10f(amplitude);
}

/* ---------------------------------------------------------------------------
 * The tap itself (implemented in avatar-audio-tap.c)
 * ------------------------------------------------------------------------- */

struct avatar_audio_tap;

/* Declared here rather than relying on obs.h so the unit test can include this
 * header on its own without the prototypes below putting obs_source into
 * prototype scope. */
struct obs_source;

/* `owner` only names the avatar source in log lines; no reference is held. */
struct avatar_audio_tap *avatar_audio_tap_create(struct obs_source *owner);
void avatar_audio_tap_destroy(struct avatar_audio_tap *tap);

/* Selects the source to tap by name.  An empty name or "none" taps nothing.
 * Safe to call from any thread. */
void avatar_audio_tap_set_source(struct avatar_audio_tap *tap, const char *name);

/* Call once a frame: retries attaching while the target does not exist yet.
 * It may legitimately not exist - the avatar source can be created before the
 * microphone, or the target may be mid-rename. */
void avatar_audio_tap_tick(struct avatar_audio_tap *tap);

/* Whether the tap is currently attached to its target. */
bool avatar_audio_tap_attached(struct avatar_audio_tap *tap);

/* Takes up to `count` 16 kHz mono samples, returning how many were taken. */
size_t avatar_audio_tap_read(struct avatar_audio_tap *tap, float *out, size_t count);

/* Peak/RMS in dBFS since the last call, plus samples dropped by the ring,
 * clearing all three.  Any out parameter may be NULL.  Returns whether any
 * audio arrived in that interval. */
bool avatar_audio_tap_take_level(struct avatar_audio_tap *tap, float *peak_db, float *rms_db, uint64_t *dropped);

#ifdef __cplusplus
}
#endif
