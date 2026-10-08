/* Measures real CPU cost of the SoundTouch pitch-shift path that
 * audioshift-filter.c's filter_audio() drives, to replace the "预计落在
 * 微秒到低个位数毫秒级" (estimated, not measured) note in
 * docs/audioshift-design.md sec 5.1/8 with an actual number.
 *
 * Not a pass/fail test - hardware varies too much for a threshold to mean
 * anything in CI. It prints timings and exits zero, skipping entirely if
 * this build wasn't compiled against a real SoundTouch (see
 * audioshift_soundtouch_available()).
 */

#include "audioshift-soundtouch.h"

#include <util/platform.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLE_RATE 48000
#define CHUNK_FRAMES 480 /* 10ms @ 48kHz: a typical OBS audio packet size */
#define DRAIN_CAPACITY_FRAMES 4096
#define MAX_CHANNELS 8
#define WARMUP_CHUNKS 20
#define TIMED_CHUNKS 200

static void fill_sine(float *interleaved, size_t channels, uint32_t frames, uint32_t phase)
{
	for (uint32_t i = 0; i < frames; i++) {
		const float t = (float)(phase + i) / (float)SAMPLE_RATE;
		const float sample = 0.3f * sinf(2.0f * 3.14159265f * 220.0f * t);
		for (size_t c = 0; c < channels; c++)
			interleaved[i * channels + c] = sample;
	}
}

static void bench(const char *label, size_t channels, float semitones)
{
	audioshift_soundtouch_t *st = audioshift_soundtouch_create(SAMPLE_RATE, channels);
	if (!st) {
		printf("%-24s skip (SoundTouch unavailable)\n", label);
		return;
	}
	audioshift_soundtouch_set_pitch_semitones(st, semitones);

	float in[CHUNK_FRAMES * MAX_CHANNELS];
	float out[DRAIN_CAPACITY_FRAMES * MAX_CHANNELS];
	uint32_t phase = 0;
	uint64_t fed = 0, emitted = 0;

	/* SoundTouch needs a handful of chunks before its internal FIFO is
	 * primed; untimed warmup keeps that one-time cost out of the
	 * steady-state measurement below. */
	for (uint32_t i = 0; i < WARMUP_CHUNKS; i++) {
		fill_sine(in, channels, CHUNK_FRAMES, phase);
		phase += CHUNK_FRAMES;
		audioshift_soundtouch_put_samples(st, in, CHUNK_FRAMES);
		fed += CHUNK_FRAMES;

		uint32_t got;
		while ((got = audioshift_soundtouch_receive_samples(st, out, DRAIN_CAPACITY_FRAMES)) > 0)
			emitted += got;
	}

	uint64_t total = 0, worst = 0, best = UINT64_MAX;

	for (uint32_t i = 0; i < TIMED_CHUNKS; i++) {
		fill_sine(in, channels, CHUNK_FRAMES, phase);
		phase += CHUNK_FRAMES;

		const uint64_t start = os_gettime_ns();
		audioshift_soundtouch_put_samples(st, in, CHUNK_FRAMES);
		fed += CHUNK_FRAMES;

		uint32_t got;
		while ((got = audioshift_soundtouch_receive_samples(st, out, DRAIN_CAPACITY_FRAMES)) > 0)
			emitted += got;
		const uint64_t elapsed = os_gettime_ns() - start;

		total += elapsed;
		if (elapsed > worst)
			worst = elapsed;
		if (elapsed < best)
			best = elapsed;
	}

	const double mean_us = (double)total / TIMED_CHUNKS / 1000.0;
	const double budget_us = 1000000.0 * CHUNK_FRAMES / SAMPLE_RATE;

	/* fed - emitted at this point is whatever SoundTouch is still holding
	 * onto internally once input/output have reached steady state (by now,
	 * WARMUP_CHUNKS + most of TIMED_CHUNKS in) -- i.e. the algorithmic
	 * latency this pitch setting adds, in frames. This is the same
	 * fed-minus-emitted accounting audioshift-filter.c uses per-call to
	 * compute output timestamps (see docs/audioshift-design.md sec 5.2). */
	const double backlog_ms = 1000.0 * (double)(fed - emitted) / SAMPLE_RATE;

	printf("%-24s mean %7.1f us  min %7.1f  max %7.1f  (10ms chunk budget: %.1f us)  latency ~%.1f ms\n", label,
	       mean_us, (double)best / 1000.0, (double)worst / 1000.0, budget_us, backlog_ms);

	audioshift_soundtouch_destroy(st);
}

int main(void)
{
	if (!audioshift_soundtouch_available()) {
		printf("skip: SoundTouch unavailable in this build\n");
		return 0;
	}

	printf("SoundTouch bench: %u warmup + %u timed %d-frame chunks @ %d Hz\n\n", WARMUP_CHUNKS, TIMED_CHUNKS,
	       CHUNK_FRAMES, SAMPLE_RATE);

	bench("stereo +9 semitones", 2, 9.0f);
	bench("stereo -12 semitones", 2, -12.0f);
	bench("stereo 0 semitones", 2, 0.0f);
	bench("mono +9 semitones", 1, 9.0f);

	return 0;
}
