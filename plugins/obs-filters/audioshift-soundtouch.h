#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin C bridge over soundtouch::SoundTouch (LGPL-2.1, C++ API) so the plain-C
 * audioshift-filter.c can drive it without becoming a C++ translation unit
 * itself. Mirrors how matting-ort.c wraps the ONNX Runtime C API for the
 * background segmentation filter: when the dependency was not available at
 * build time (AUDIOSHIFT_HAS_SOUNDTOUCH undefined), every call below becomes
 * a harmless no-op and audioshift_soundtouch_available() returns false, so
 * callers can fall back to a pure passthrough instead of crashing or failing
 * to build. */

typedef struct audioshift_soundtouch audioshift_soundtouch_t;

/* True if this build was compiled against a real SoundTouch library. */
bool audioshift_soundtouch_available(void);

/* Returns NULL if unavailable at build time, or on allocation failure. */
audioshift_soundtouch_t *audioshift_soundtouch_create(uint32_t sample_rate, size_t channels);
void audioshift_soundtouch_destroy(audioshift_soundtouch_t *st);

void audioshift_soundtouch_set_pitch_semitones(audioshift_soundtouch_t *st, float semitones);

/* samples/out_samples are interleaved; frame counts are per-channel sample
 * frames, not individual floats. */
void audioshift_soundtouch_put_samples(audioshift_soundtouch_t *st, const float *samples, uint32_t frames);
uint32_t audioshift_soundtouch_receive_samples(audioshift_soundtouch_t *st, float *out_samples, uint32_t max_frames);

/* Drops any buffered input/output and resets internal algorithm state, e.g.
 * after a large timestamp discontinuity. */
void audioshift_soundtouch_flush(audioshift_soundtouch_t *st);

#ifdef __cplusplus
}
#endif
