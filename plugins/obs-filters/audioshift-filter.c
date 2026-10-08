#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/deque.h>
#include <util/dstr.h>

#include "audioshift-filter.h"
#include "audioshift-soundtouch.h"

/* -------------------------------------------------------- */

#define do_log(level, format, ...) \
	blog(level, "[audioshift: '%s'] " format, obs_source_get_name(as->context), ##__VA_ARGS__)

#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* -------------------------------------------------------- */

#define S_PRESET "preset"
#define S_PITCH_SEMITONES "pitch_semitones"
#define S_LATENCY_INFO "latency_info"

/* S_PRESET_CUSTOM / S_PRESET_MALE_TO_FEMALE / etc. come from
 * audioshift-filter.h, shared with the preset-to-semitone lookup table
 * defined there. */

#define MT_ obs_module_text
#define TEXT_PRESET MT_("AudioShift.Preset")
#define TEXT_PRESET_CUSTOM MT_("AudioShift.Preset.Custom")
#define TEXT_PRESET_MALE_TO_FEMALE MT_("AudioShift.Preset.MaleToFemale")
#define TEXT_PRESET_FEMALE_TO_MALE MT_("AudioShift.Preset.FemaleToMale")
#define TEXT_PRESET_CHILD MT_("AudioShift.Preset.Child")
#define TEXT_PRESET_MONSTER MT_("AudioShift.Preset.Monster")
#define TEXT_PITCH_SEMITONES MT_("AudioShift.PitchSemitones")
#define TEXT_LATENCY_INFO MT_("AudioShift.LatencyInfo")
#define TEXT_LATENCY_PENDING MT_("AudioShift.LatencyPending")
#define TEXT_UNAVAILABLE MT_("AudioShift.Unavailable")

#define MIN_SEMITONES -12.0
#define MAX_SEMITONES 12.0

/* Drain chunk size for draining SoundTouch's internal FIFO each call; this is
 * not a protocol requirement (unlike RNNoise's fixed frame size in
 * noise-suppress-filter.c), just a reasonably large buffer so the drain loop
 * in audioshift_filter_audio() doesn't need many iterations per call. */
#define AUDIOSHIFT_DRAIN_FRAMES 1024

/* -------------------------------------------------------- */

struct audioshift_info {
	uint32_t frames;
	uint64_t timestamp;
};

struct audioshift_data {
	obs_source_t *context;

	/* Written by audioshift_update() (UI thread), read by
	 * audioshift_filter_audio() (audio thread). A plain scalar: a torn
	 * read just delays the new value by one audio callback, which is
	 * harmless here. See docs/audioshift-design.md §5.1 for why this
	 * doesn't need a mutex. */
	float pending_semitones;

	/* Everything below is touched only by the audio thread. */
	audioshift_soundtouch_t *st;
	float applied_semitones;
	uint32_t sample_rate;
	size_t channels;

	bool create_failed;

	uint64_t samples_fed;
	uint64_t samples_emitted;
	uint64_t last_timestamp;

	struct deque info_buffer;
	struct deque output_buffers[MAX_AUDIO_CHANNELS];

	float *interleave_in;
	size_t interleave_in_frames;
	float *interleave_out;
	float *deinterleave_tmp;

	struct obs_audio_data output_audio;
	DARRAY(float) output_data;
};

/* -------------------------------------------------------- */

static const char *audioshift_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioShiftFilter");
}

static void audioshift_clear_buffers(struct audioshift_data *as)
{
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		deque_pop_front(&as->output_buffers[i], NULL, as->output_buffers[i].size);
	deque_pop_front(&as->info_buffer, NULL, as->info_buffer.size);
}

static void audioshift_reset(struct audioshift_data *as)
{
	audioshift_clear_buffers(as);

	if (as->st)
		audioshift_soundtouch_flush(as->st);

	as->samples_fed = 0;
	as->samples_emitted = 0;
}

static void audioshift_update(void *data, obs_data_t *s)
{
	struct audioshift_data *as = data;
	const char *preset = obs_data_get_string(s, S_PRESET);

	if (strcmp(preset, S_PRESET_CUSTOM) == 0)
		as->pending_semitones = (float)obs_data_get_double(s, S_PITCH_SEMITONES);
	else
		as->pending_semitones = audioshift_preset_semitones(preset);
}

static void *audioshift_create(obs_data_t *settings, obs_source_t *filter)
{
	struct audioshift_data *as = bzalloc(sizeof(*as));
	as->context = filter;

	if (!audioshift_soundtouch_available())
		info("SoundTouch not available at build time, filter will passthrough audio unchanged");

	audioshift_update(as, settings);
	return as;
}

static void audioshift_destroy(void *data)
{
	struct audioshift_data *as = data;

	if (as->st)
		audioshift_soundtouch_destroy(as->st);

	audioshift_clear_buffers(as);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		deque_free(&as->output_buffers[i]);
	deque_free(&as->info_buffer);

	bfree(as->interleave_in);
	bfree(as->interleave_out);
	bfree(as->deinterleave_tmp);
	da_free(as->output_data);
	bfree(as);
}

static void audioshift_reconfigure(struct audioshift_data *as, uint32_t sample_rate, size_t channels)
{
	if (as->st)
		audioshift_soundtouch_destroy(as->st);

	as->st = audioshift_soundtouch_create(sample_rate, channels);
	as->create_failed = !as->st;
	as->sample_rate = sample_rate;
	as->channels = channels;
	as->applied_semitones = as->pending_semitones;

	if (as->st)
		audioshift_soundtouch_set_pitch_semitones(as->st, as->applied_semitones);

	audioshift_reset(as);

	bfree(as->interleave_out);
	bfree(as->deinterleave_tmp);
	as->interleave_out = bmalloc(AUDIOSHIFT_DRAIN_FRAMES * channels * sizeof(float));
	as->deinterleave_tmp = bmalloc(AUDIOSHIFT_DRAIN_FRAMES * sizeof(float));

	/* channels just changed (or this is the first call): interleave_in's
	 * existing capacity was sized for the old channel count, so the
	 * "already big enough" check in audioshift_ensure_in_buf() has to be
	 * forced to reallocate even if the frame count per call hasn't moved. */
	as->interleave_in_frames = 0;
}

static void audioshift_ensure_in_buf(struct audioshift_data *as, uint32_t frames)
{
	if (frames <= as->interleave_in_frames)
		return;

	as->interleave_in_frames = frames;
	as->interleave_in = brealloc(as->interleave_in, (size_t)frames * as->channels * sizeof(float));
}

static struct obs_audio_data *audioshift_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct audioshift_data *as = data;

	if (!audioshift_soundtouch_available())
		return audio;

	const uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
	const size_t channels = audio_output_get_channels(obs_get_audio());

	bool first_call = !as->st && !as->create_failed;
	if (first_call || as->sample_rate != sample_rate || as->channels != channels)
		audioshift_reconfigure(as, sample_rate, channels);

	if (!as->st)
		return audio;

	if (as->applied_semitones != as->pending_semitones) {
		as->applied_semitones = as->pending_semitones;
		audioshift_soundtouch_set_pitch_semitones(as->st, as->applied_semitones);
	}

	/* A large timestamp jump means this is effectively a new stream of
	 * audio (source re-activated, seek, etc.); drop stale buffered state
	 * rather than let it bleed into unrelated audio. Same rationale as
	 * noise-suppress-filter.c's reset_data(). */
	if (as->last_timestamp) {
		int64_t diff = llabs((int64_t)as->last_timestamp - (int64_t)audio->timestamp);
		if (diff > 1000000000LL)
			audioshift_reset(as);
	}
	as->last_timestamp = audio->timestamp;

	struct audioshift_info info = {audio->frames, audio->timestamp};
	deque_push_back(&as->info_buffer, &info, sizeof(info));

	audioshift_ensure_in_buf(as, audio->frames);
	audioshift_interleave(audio, channels, as->interleave_in);

	audioshift_soundtouch_put_samples(as->st, as->interleave_in, audio->frames);
	as->samples_fed += audio->frames;

	for (;;) {
		uint32_t got = audioshift_soundtouch_receive_samples(as->st, as->interleave_out,
								      AUDIOSHIFT_DRAIN_FRAMES);
		if (!got)
			break;

		for (size_t c = 0; c < channels; c++) {
			audioshift_deinterleave_channel(as->interleave_out, channels, c, got, as->deinterleave_tmp);
			deque_push_back(&as->output_buffers[c], as->deinterleave_tmp, got * sizeof(float));
		}
		as->samples_emitted += got;
	}

	struct audioshift_info out_info;
	deque_peek_front(&as->info_buffer, &out_info, sizeof(out_info));
	size_t out_size = (size_t)out_info.frames * sizeof(float);

	if (as->output_buffers[0].size < out_size)
		return NULL;

	deque_pop_front(&as->info_buffer, NULL, sizeof(out_info));

	/* Frames still stuck inside SoundTouch's pipeline right now approximate
	 * how stale the packet we're about to emit is. See
	 * docs/audioshift-design.md §5.2 for why this has to be a running
	 * fed-minus-emitted count rather than a fixed constant. */
	uint64_t buffered_frames = as->samples_fed - as->samples_emitted;
	uint64_t latency_ns = sample_rate ? (buffered_frames * 1000000000ULL) / sample_rate : 0;

	da_resize(as->output_data, out_size * channels);
	for (size_t c = 0; c < channels; c++) {
		as->output_audio.data[c] = (uint8_t *)&as->output_data.array[c * out_info.frames];
		deque_pop_front(&as->output_buffers[c], as->output_audio.data[c], out_size);
	}

	as->output_audio.frames = out_info.frames;
	as->output_audio.timestamp = out_info.timestamp - latency_ns;
	return &as->output_audio;
}

/* -------------------------------------------------------- */

static bool audioshift_preset_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *preset = obs_data_get_string(settings, S_PRESET);
	bool custom = strcmp(preset, S_PRESET_CUSTOM) == 0;

	obs_property_t *p_semitones = obs_properties_get(props, S_PITCH_SEMITONES);
	obs_property_set_enabled(p_semitones, custom);

	if (!custom)
		obs_data_set_double(settings, S_PITCH_SEMITONES, audioshift_preset_semitones(preset));

	return true;
}

static void audioshift_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_PRESET, S_PRESET_CUSTOM);
	obs_data_set_default_double(s, S_PITCH_SEMITONES, 0.0);
}

static obs_properties_t *audioshift_properties(void *data)
{
	struct audioshift_data *as = data;
	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *preset =
		obs_properties_add_list(ppts, S_PRESET, TEXT_PRESET, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset, TEXT_PRESET_CUSTOM, S_PRESET_CUSTOM);
	obs_property_list_add_string(preset, TEXT_PRESET_MALE_TO_FEMALE, S_PRESET_MALE_TO_FEMALE);
	obs_property_list_add_string(preset, TEXT_PRESET_FEMALE_TO_MALE, S_PRESET_FEMALE_TO_MALE);
	obs_property_list_add_string(preset, TEXT_PRESET_CHILD, S_PRESET_CHILD);
	obs_property_list_add_string(preset, TEXT_PRESET_MONSTER, S_PRESET_MONSTER);
	obs_property_set_modified_callback(preset, audioshift_preset_modified);

	obs_property_t *p =
		obs_properties_add_float_slider(ppts, S_PITCH_SEMITONES, TEXT_PITCH_SEMITONES, MIN_SEMITONES,
						MAX_SEMITONES, 0.5);
	obs_property_float_set_suffix(p, " st");

	/* Latency is a running fed-minus-emitted sample count (§5.2), so it only
	 * means something once audio has actually flowed through the filter at
	 * least once; show a generic note until then instead of a misleading
	 * "0 ms". */
	struct dstr label = {0};
	if (!audioshift_soundtouch_available()) {
		dstr_copy(&label, TEXT_UNAVAILABLE);
	} else if (as && as->st && as->sample_rate) {
		uint64_t buffered_frames = as->samples_fed - as->samples_emitted;
		double ms = (double)buffered_frames * 1000.0 / (double)as->sample_rate;
		dstr_printf(&label, "%s %.0f ms", TEXT_LATENCY_INFO, ms);
	} else {
		dstr_copy(&label, TEXT_LATENCY_PENDING);
	}
	obs_properties_add_text(ppts, S_LATENCY_INFO, label.array, OBS_TEXT_INFO);
	dstr_free(&label);

	return ppts;
}

/* -------------------------------------------------------- */

struct obs_source_info audioshift_filter = {
	.id = "audioshift_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = audioshift_name,
	.create = audioshift_create,
	.destroy = audioshift_destroy,
	.update = audioshift_update,
	.filter_audio = audioshift_filter_audio,
	.get_defaults = audioshift_defaults,
	.get_properties = audioshift_properties,
};
