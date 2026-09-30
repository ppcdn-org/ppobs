/* Verifies the background segmentation filter can attach to the source types
 * users will actually put it on.
 *
 * Both OBS gates are replicated here rather than exercised through libobs,
 * which would need a graphics device and a loaded module.  They must stay in
 * sync with:
 *   - filter_compatible() in libobs/obs-source.c
 *   - filter_compatible() in frontend/dialogs/OBSBasicFilters.cpp
 *
 * The filter used to declare OBS_SOURCE_ASYNC, which silently excluded it from
 * image sources: an async filter is only offered on async sources, so there was
 * no way to preview a matte against a still image.  These cases pin that down.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* Mirrors of the libobs flags, kept local so the test needs no OBS headers. */
#define SRC_VIDEO (1 << 0)
#define SRC_AUDIO (1 << 1)
#define SRC_ASYNC (1 << 2)
#define SRC_ASYNC_VIDEO (SRC_ASYNC | SRC_VIDEO)
#define SRC_AV (SRC_ASYNC_VIDEO | SRC_AUDIO)

/* libobs/obs-source.c: the filter's capabilities must be a subset of the
 * source's, which is what rejects an async filter on a plain video source. */
static bool core_compatible(uint32_t source_flags, uint32_t filter_flags)
{
	uint32_t s_caps = source_flags & SRC_AV;
	uint32_t f_caps = filter_flags & SRC_AV;

	if ((f_caps & SRC_AUDIO) != 0 && (f_caps & SRC_VIDEO) == 0)
		f_caps &= ~SRC_ASYNC;

	return (s_caps & f_caps) == f_caps;
}

/* frontend/dialogs/OBSBasicFilters.cpp: decides which filters appear in the
 * "Effect Filters" (async == false) and "Audio/Async Filters" (async == true)
 * menus. */
static bool ui_compatible(bool async, uint32_t source_flags, uint32_t filter_flags)
{
	bool filterVideo = (filter_flags & SRC_VIDEO) != 0;
	bool filterAsync = (filter_flags & SRC_ASYNC) != 0;
	bool filterAudio = (filter_flags & SRC_AUDIO) != 0;
	bool audio = (source_flags & SRC_AUDIO) != 0;
	bool audioOnly = (source_flags & SRC_VIDEO) == 0;
	bool asyncSource = (source_flags & SRC_ASYNC) != 0;

	if (async && ((audioOnly && filterVideo) || (!audio && !asyncSource) || (filterAudio && !audio) ||
		      (!asyncSource && !filterAudio)))
		return false;

	return (async && (filterAudio || filterAsync)) || (!async && !filterAudio && !filterAsync);
}

/* The flags the filter now registers with (see the obs_source_info at the
 * bottom of background-segmentation-filter.c). */
#define BACKGROUND_FILTER_FLAGS (SRC_VIDEO)

/* The flags the filter used to register with, kept as a regression guard. */
#define BACKGROUND_FILTER_FLAGS_OLD (SRC_VIDEO | SRC_ASYNC)

/* Real source types this filter is expected to work on, taken from their
 * obs_source_info definitions. */
#define IMAGE_SOURCE_FLAGS (SRC_VIDEO)                          /* image-source.c */
#define COLOR_SOURCE_FLAGS (SRC_VIDEO)                          /* color-source.c */
#define CAMERA_SOURCE_FLAGS (SRC_ASYNC_VIDEO | SRC_AUDIO)       /* win-dshow */
#define MEDIA_SOURCE_FLAGS (SRC_ASYNC_VIDEO | SRC_AUDIO)        /* ffmpeg_source */
#define WINDOW_CAPTURE_FLAGS (SRC_VIDEO)                        /* win-capture */
#define AUDIO_ONLY_FLAGS (SRC_AUDIO)                            /* wasapi input */

static void test_attaches_to_image_source(void)
{
	/* The point of the rework: a still image must accept the filter, both
	 * at the core level and in the effect-filter menu. */
	assert(core_compatible(IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(ui_compatible(false, IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));

	/* And it must not show up in the async menu for that source. */
	assert(!ui_compatible(true, IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
}

static void test_old_flags_rejected_image_source(void)
{
	/* Regression guard: with the previous flags the core rejected the
	 * attachment outright and the UI never offered it, which is the bug
	 * this rework fixes.  If someone re-adds OBS_SOURCE_ASYNC, this fails.
	 */
	assert(!core_compatible(IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS_OLD));
	assert(!ui_compatible(false, IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS_OLD));
	assert(!ui_compatible(true, IMAGE_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS_OLD));
}

static void test_still_attaches_to_camera(void)
{
	/* Widening to sync sources must not lose the original use case: a
	 * camera is an async source, and a plain video filter is a subset of
	 * its capabilities. */
	assert(core_compatible(CAMERA_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(ui_compatible(false, CAMERA_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
}

static void test_attaches_to_other_video_sources(void)
{
	assert(core_compatible(MEDIA_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(ui_compatible(false, MEDIA_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));

	assert(core_compatible(WINDOW_CAPTURE_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(ui_compatible(false, WINDOW_CAPTURE_FLAGS, BACKGROUND_FILTER_FLAGS));

	assert(core_compatible(COLOR_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(ui_compatible(false, COLOR_SOURCE_FLAGS, BACKGROUND_FILTER_FLAGS));
}

static void test_not_offered_on_audio_only(void)
{
	/* A video filter on an audio-only source is meaningless; the core
	 * rejects it because VIDEO is not among the source's capabilities. */
	assert(!core_compatible(AUDIO_ONLY_FLAGS, BACKGROUND_FILTER_FLAGS));
	assert(!ui_compatible(true, AUDIO_ONLY_FLAGS, BACKGROUND_FILTER_FLAGS));
}

int main(void)
{
	test_attaches_to_image_source();
	test_old_flags_rejected_image_source();
	test_still_attaches_to_camera();
	test_attaches_to_other_video_sources();
	test_not_offered_on_audio_only();
	return 0;
}
