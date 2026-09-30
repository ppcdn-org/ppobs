/*
 * Portrait segmentation filter (virtual background, stage P0).
 *
 * Runs a portrait matting model (MODNet-style: one image input, one
 * single-channel alpha output) over the source and turns the result into the
 * frame's alpha channel.  The replacement background is whatever the user puts
 * below this source in the scene, so OBS keeps owning image/video decoding,
 * looping and audio.  See docs/virtual-background-design.md.
 *
 * This is a synchronous (GPU) filter rather than an async frame filter, so it
 * attaches to any video source - camera, media file, image, window capture.
 * An async filter would be limited to async sources and could not be used on a
 * still image, which makes previewing the matte awkward.  The cost is that the
 * model input has to come back from the GPU: the target is rendered into a
 * small texrender and staged to CPU memory, which happens at the inference
 * rate rather than once per frame.
 *
 * Threading contract: video_render never blocks on the worker.  It performs the
 * readback (graphics thread only), hands the pixels over under a try-lock, and
 * composites using whatever matte is currently published.  Inference therefore
 * runs behind the live frame, which is fine for a matte that changes slowly.
 * With no model, a failed inference, or a stale matte, the filter is an exact
 * passthrough rather than a black frame.
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <wchar.h>

#include "background-mask.h"
#include "matting-models.h"
#include "matting-ort.h"

#define S_MODEL "model"
#define S_MODEL_FAMILY "model_family"
#define S_INFERENCE_FPS "inference_fps"
#define S_THRESHOLD "threshold"
#define S_SOFTNESS "softness"
#define S_FEATHER "feather"
#define S_EXPAND "expand"
#define S_SMOOTHING "smoothing"
#define S_DESPILL "despill"
#define S_REJECT "reject"
#define S_SHOW_MASK "show_mask"

/* Models live in the OBS configuration directory:
 *
 *   portable:  <obs>/config/background-models
 *   installed: %APPDATA%/background-models
 *
 * This is a user-owned location, so models can be added without touching the
 * installation or needing administrator rights, and it survives replacing the
 * OBS folder on upgrade.  An earlier revision kept them in the plugin's data
 * directory, which is not writable on a per-machine install.
 *
 * The path is derived from obs_module_config_path rather than
 * os_get_config_path, because the latter is hard-wired to %APPDATA% on Windows
 * and ignores portable mode: a portable copy would reach into the roaming
 * profile of whoever happened to be logged in.  The module API is handed a
 * portable-aware base by the frontend (GetAppConfigPath -> obs_startup).
 *
 * The plugin's own data directory is still searched as a fallback, so models
 * shipped alongside a deployment keep working.
 */
#define BACKGROUND_MODEL_DIR "background-models"

/* obs_module_config_path yields "<root>/obs-studio/plugin_config/obs-filters",
 * where <root> is <obs>/config when portable and %APPDATA% when installed.
 * The path is cut at this marker rather than by trimming a fixed number of
 * components: the two layouts do not nest to the same depth, so a fixed count
 * is wrong for one of them. */
#define BACKGROUND_CONFIG_MARKER "/obs-studio/"

/* The matte starts fading once it is this old, and is gone by the time it
 * reaches the timeout.  Without the ramp the filter snaps straight back to the
 * untouched source, which on screen reads as the real background appearing all
 * at once - far more noticeable than the matte quietly softening. */
#define BACKGROUND_MASK_FADE_START_NS 400000000ULL
#define BACKGROUND_MASK_TIMEOUT_NS 1000000000ULL

/* A recurrent model's temporal memory is only meaningful for a continuous run
 * of frames.  If the graphics thread stops feeding it for longer than this -
 * the source was hidden, the scene changed, playback was paused - the carried
 * state describes a moment that is no longer on screen, so it is discarded. */
#define BACKGROUND_STATE_GAP_NS 500000000ULL

/* Divisor floor for edge unmixing.  Solving for the subject colour divides by
 * alpha, so this bounds how much matte noise a near-transparent pixel can
 * amplify.  Tuned with `despill_strength` against AI10-Nina: measured over
 * eight frames, the mean edge-vs-subject luminance error falls from about 78
 * to under 4, and lowering the floor or raising the strength turns the pale
 * fringe into an equally visible dark one. */
#define BACKGROUND_DESPILL_FLOOR 0.5f

/* Pulling the matte in by one mask pixel before feathering is what lets the
 * unmixing work: it drops the most contaminated pixels, which sit outside the
 * true silhouette and carry almost pure wall colour, instead of asking the
 * unmix to reconstruct a subject colour that was never really there. */
#define BACKGROUND_DESPILL_SHRINK 1

/* Width of the ramp above the wall-rejection tolerance, in 0-1 colour units
 * (16/255).  A hard cut would alias along the trailing edge of a moving arm,
 * which is exactly where the rejection does its work. */
#define BACKGROUND_REJECT_SOFTNESS (16.0f / 255.0f)

struct background_data {
	obs_source_t *context;

	pthread_mutex_t mutex;
	os_event_t *wake_event;
	pthread_t worker;
	bool worker_started;
	bool stop;

	/* Readback staging, graphics thread only. */
	gs_effect_t *effect;
	gs_texrender_t *readback_render;
	gs_stagesurf_t *readback_stage;
	uint32_t readback_size;
	uint64_t last_readback_time;

	/* RGBA pixels handed from the graphics thread to the worker.  Two
	 * buffers so the worker can read one while the next readback fills the
	 * other.  Sized for the active model, which is why they are allocated
	 * rather than inline: MODNet and RVM are exported at different
	 * resolutions. */
	uint8_t *rgba[2];
	size_t rgba_capacity;
	uint32_t rgba_size;
	int write_buffer;
	int pending_buffer;
	bool input_pending;
	uint64_t pending_timestamp;

	/* Model geometry published by the worker for the graphics thread to
	 * size its readback with.  Zero until a model is loaded. */
	uint32_t required_size;

	/* worker-owned inference state */
	struct matting_ort ort;
	struct matting_ort_session session;
	bool session_ready;
	const struct matting_model_info *active_model;
	char *loaded_model;
	enum matting_model_family loaded_family;
	bool warned_inference;
	float *input_tensor;
	size_t input_tensor_count;
	/* Raw model output, kept at full float precision so the threshold ramp
	 * is applied to the probabilities rather than to a quantised copy. */
	float *matte_raw;
	size_t matte_raw_count;
	uint64_t last_inference_time;
	struct background_mask worker_mask;
	struct background_mask history_mask;
	struct background_mask scratch_mask;

	/* published matte, guarded by mutex */
	struct background_mask published_mask;
	bool published_ready;
	uint64_t published_timestamp;
	uint64_t published_serial;
	struct background_colour published_colour;

	/* graphics-thread copy of the published matte */
	gs_texture_t *mask_texture;
	uint32_t mask_texture_width;
	uint32_t mask_texture_height;
	uint64_t uploaded_serial;
	struct background_mask upload_mask;
	struct background_colour upload_colour;
	/* Last computed fade level, reused when the lock is contended. */
	float last_strength;

	char *model;
	enum matting_model_family family;
	int inference_fps;
	double threshold;
	double softness;
	int feather;
	int expand;
	double smoothing;
	double despill;
	double reject;
	bool show_mask;
};

static const char *background_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BackgroundSegmentationFilter");
}

/* Model directory inside the OBS configuration folder, whether or not it
 * exists yet.  This is the documented location and the one the properties page
 * shows, so it has to resolve even before the user has created it.
 *
 * `below_obs_studio` selects one of the two sensible readings of "the config
 * directory", which differ by whether "obs-studio" is kept:
 *
 *   false -> <obs>/config/background-models   (portable)
 *            %APPDATA%/background-models      (installed)
 *   true  -> <obs>/config/obs-studio/background-models   (portable)
 *            %APPDATA%/obs-studio/background-models      (installed)
 *
 * Both are searched, because either is a reasonable place for someone to have
 * put the folder. */
static char *background_model_dir_variant(bool below_obs_studio)
{
	/* Asking for "" gives the module's own config directory, which sits
	 * under "<root>/obs-studio/plugin_config". */
	char *module_config = obs_module_config_path("");
	if (!module_config)
		return NULL;

	struct dstr path = {0};
	dstr_copy(&path, module_config);
	bfree(module_config);

	dstr_replace(&path, "\\", "/");

	/* Cut at the marker, keeping or dropping "obs-studio" as asked. */
	const char *marker = strstr(path.array, BACKGROUND_CONFIG_MARKER);
	if (!marker) {
		dstr_free(&path);
		return NULL;
	}

	size_t keep = (size_t)(marker - path.array);
	if (below_obs_studio)
		keep += strlen(BACKGROUND_CONFIG_MARKER) - 1;
	dstr_resize(&path, keep);

	dstr_cat(&path, "/" BACKGROUND_MODEL_DIR);
	return path.array;
}

/* The location shown in the properties page and created on demand. */
static char *background_model_dir(void)
{
	return background_model_dir_variant(false);
}

/* Full path of a model file, or NULL if it is not in any searched location.
 * The configuration directories win, so a user-supplied model overrides one
 * shipped with the deployment under the same name. */
static char *background_model_path(const char *name)
{
	for (int variant = 0; variant < 2; variant++) {
		char *dir = background_model_dir_variant(variant != 0);
		if (!dir)
			continue;

		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", dir, name);
		bfree(dir);

		if (os_file_exists(path.array))
			return path.array;
		dstr_free(&path);
	}

	struct dstr relative = {0};
	dstr_printf(&relative, "%s/%s", BACKGROUND_MODEL_DIR, name);
	char *bundled = obs_module_file(relative.array);
	dstr_free(&relative);
	return bundled;
}

/* Adds every .onnx in `dir_path` to the list, skipping names already present
 * so a bundled model does not shadow the user's copy of the same name. */
static void background_add_models_from(obs_property_t *list, const char *dir_path)
{
	os_dir_t *dir = os_opendir(dir_path);
	if (!dir)
		return;

	struct os_dirent *entry;
	while ((entry = os_readdir(dir)) != NULL) {
		if (entry->directory)
			continue;

		const size_t len = strlen(entry->d_name);
		if (len < 6 || astrcmpi(entry->d_name + len - 5, ".onnx") != 0)
			continue;

		bool duplicate = false;
		const size_t count = obs_property_list_item_count(list);
		for (size_t i = 0; i < count; i++) {
			const char *existing = obs_property_list_item_string(list, i);
			if (existing && astrcmpi(existing, entry->d_name) == 0) {
				duplicate = true;
				break;
			}
		}

		if (!duplicate)
			obs_property_list_add_string(list, entry->d_name, entry->d_name);
	}

	os_closedir(dir);
}

/* Loads the requested model, resolving which family it belongs to.  Worker
 * thread only. */
static void background_reload_model(struct background_data *filter, char *requested,
				    enum matting_model_family requested_family)
{
	matting_ort_release_session(&filter->ort, &filter->session);
	filter->session_ready = false;
	filter->active_model = NULL;
	filter->warned_inference = false;

	if (*requested) {
		enum matting_model_family family = requested_family;

		if (family == MATTING_MODEL_AUTO) {
			family = matting_model_detect(requested);
			if (family == MATTING_MODEL_AUTO)
				blog(LOG_WARNING,
				     "[background segmentation: '%s'] could not infer the family of '%s'; "
				     "select it explicitly",
				     obs_source_get_name(filter->context), requested);
		}

		const struct matting_model_info *info = matting_model_get(family);
		char *path = info ? background_model_path(requested) : NULL;
		wchar_t *wide = NULL;

		if (path && os_file_exists(path) && os_utf8_to_wcs_ptr(path, 0, &wide) && wide) {
			filter->session_ready =
				matting_ort_create_session(&filter->ort, wide, &info->spec, &filter->session);
		}

		bfree(wide);
		bfree(path);

		if (filter->session_ready) {
			filter->active_model = info;

			const size_t pixels = (size_t)info->size * (size_t)info->size;
			const size_t tensor_count = pixels * 3;

			/* Buffers follow the model, so switching between a 512
			 * MODNet and a 256 RVM does not leave the previous
			 * model's geometry behind. */
			if (tensor_count != filter->input_tensor_count) {
				bfree(filter->input_tensor);
				filter->input_tensor = bmalloc(tensor_count * sizeof(float));
				filter->input_tensor_count = filter->input_tensor ? tensor_count : 0;
			}
			if (pixels != filter->matte_raw_count) {
				bfree(filter->matte_raw);
				filter->matte_raw = bmalloc(pixels * sizeof(float));
				filter->matte_raw_count = filter->matte_raw ? pixels : 0;
			}

			if (!filter->input_tensor || !filter->matte_raw) {
				blog(LOG_WARNING,
				     "[background segmentation: '%s'] could not allocate inference buffers",
				     obs_source_get_name(filter->context));
				matting_ort_release_session(&filter->ort, &filter->session);
				filter->session_ready = false;
				filter->active_model = NULL;
			} else {
				blog(LOG_INFO,
				     "[background segmentation: '%s'] %s model '%s' ready (%s, %" PRId64 "px%s)",
				     obs_source_get_name(filter->context), info->label, requested, info->licence,
				     info->size, info->recurrent ? ", recurrent" : "");
			}
		} else {
			blog(LOG_WARNING,
			     "[background segmentation: '%s'] model '%s' unavailable or incompatible, passing through",
			     obs_source_get_name(filter->context), requested);
		}
	}

	bfree(filter->loaded_model);
	filter->loaded_model = requested;
	filter->loaded_family = requested_family;
	filter->last_inference_time = 0;

	/* The old silhouette does not describe the new model's output, and the
	 * graphics thread needs the new readback size. */
	pthread_mutex_lock(&filter->mutex);
	filter->published_ready = false;
	filter->required_size = filter->active_model ? (uint32_t)filter->active_model->size : 0;
	pthread_mutex_unlock(&filter->mutex);
	background_mask_free(&filter->history_mask);
}

/* Converts the staged RGBA readback into the model's planar CHW input.  The
 * readback is already at model resolution, so this is a straight per-pixel
 * normalisation with no resampling.  The constants come from the model family,
 * since MODNet expects [-1, 1] and RVM expects [0, 1]. */
static void background_rgba_to_tensor(struct background_data *filter, const uint8_t *rgba, size_t pixels)
{
	const float mean = filter->active_model->mean;
	const float scale = filter->active_model->scale;

	float *const red = filter->input_tensor;
	float *const green = red + pixels;
	float *const blue = green + pixels;

	for (size_t i = 0; i < pixels; i++) {
		const uint8_t *pixel = rgba + i * 4;
		red[i] = ((float)pixel[0] - mean) * scale;
		green[i] = ((float)pixel[1] - mean) * scale;
		blue[i] = ((float)pixel[2] - mean) * scale;
	}
}

static void background_run_inference(struct background_data *filter, const uint8_t *rgba, uint64_t timestamp,
				     float threshold, float softness, int feather, int expand, float smoothing,
				     float despill)
{
	if (!filter->session_ready || !filter->active_model)
		return;

	const uint32_t size = (uint32_t)filter->active_model->size;
	const size_t pixels = (size_t)size * size;

	if (filter->input_tensor_count != pixels * 3 || filter->matte_raw_count != pixels)
		return;

	/* A recurrent model's carried state assumes the previous frame was
	 * recent.  After a gap it describes a scene that is no longer on
	 * screen, so start its memory over rather than blending the two. */
	if (filter->active_model->recurrent && filter->last_inference_time &&
	    timestamp > filter->last_inference_time &&
	    timestamp - filter->last_inference_time > BACKGROUND_STATE_GAP_NS) {
		matting_ort_reset_states(&filter->ort, &filter->session);
		background_mask_free(&filter->history_mask);
	}
	filter->last_inference_time = timestamp;

	background_rgba_to_tensor(filter, rgba, pixels);

	if (!matting_ort_run(&filter->ort, &filter->session, filter->input_tensor, filter->input_tensor_count,
			     filter->matte_raw, filter->matte_raw_count)) {
		if (!filter->warned_inference) {
			blog(LOG_WARNING,
			     "[background segmentation: '%s'] inference produced no usable matte, passing through",
			     obs_source_get_name(filter->context));
			filter->warned_inference = true;
		}
		return;
	}

	/* Clamp to the [0, 1] the threshold ramp assumes; exports occasionally
	 * emit values marginally outside it. */
	for (size_t i = 0; i < filter->matte_raw_count; i++) {
		if (filter->matte_raw[i] < 0.0f)
			filter->matte_raw[i] = 0.0f;
		else if (filter->matte_raw[i] > 1.0f)
			filter->matte_raw[i] = 1.0f;
	}

	if (!background_mask_from_probabilities(&filter->worker_mask, filter->matte_raw, filter->matte_raw_count, size,
						size, threshold, softness))
		return;

	/* Measure the wall colour against the un-shrunk, un-feathered matte:
	 * this is the only point where "definitely background" is still a
	 * strict statement, before the edge is deliberately blurred.  The
	 * readback is RGBA at model resolution, so the mask and the pixels
	 * already line up. */
	struct background_colour colour = {{0.0f, 0.0f, 0.0f}, false};
	if (despill > 0.0f)
		colour = background_mask_estimate_colour(&filter->worker_mask, rgba, size, size, 4);

	/* Order matters: grow or shrink the silhouette first, then soften the
	 * result.  Feathering before morphology would be undone by the min/max
	 * passes. */
	int shrink = 0;
	if (expand > 0)
		background_mask_dilate(&filter->worker_mask, &filter->scratch_mask, expand);
	else if (expand < 0)
		shrink = -expand;

	/* Unmixing can only recover a pixel that actually contains subject.
	 * The outermost band does not, so pull the matte in before feathering
	 * and let the unmix clean up what remains. */
	if (colour.valid)
		shrink += BACKGROUND_DESPILL_SHRINK;

	if (shrink > 0)
		background_mask_erode(&filter->worker_mask, &filter->scratch_mask, shrink);

	if (feather > 0)
		background_mask_feather(&filter->worker_mask, &filter->scratch_mask, feather);

	background_mask_smooth(&filter->worker_mask, &filter->history_mask, smoothing);

	pthread_mutex_lock(&filter->mutex);
	if (background_mask_resize(&filter->published_mask, filter->worker_mask.width, filter->worker_mask.height)) {
		memcpy(filter->published_mask.data, filter->worker_mask.data,
		       (size_t)filter->worker_mask.width * filter->worker_mask.height);
		filter->published_ready = true;
		filter->published_timestamp = timestamp;
		filter->published_serial++;
		filter->published_colour = colour;
	}
	pthread_mutex_unlock(&filter->mutex);
}

static void *background_worker(void *data)
{
	struct background_data *filter = data;

	if (matting_ort_load(&filter->ort) && filter->ort.sdk_available) {
		blog(LOG_INFO, "[background segmentation: '%s'] ONNX Runtime %s loaded",
		     obs_source_get_name(filter->context), filter->ort.version);
	} else {
		blog(LOG_WARNING, "[background segmentation: '%s'] ONNX Runtime is unavailable, passing through",
		     obs_source_get_name(filter->context));
	}

	for (;;) {
		char *requested_model = NULL;
		enum matting_model_family requested_family = MATTING_MODEL_AUTO;
		const uint8_t *work_rgba = NULL;
		uint64_t timestamp = 0;
		float threshold = 0.5f;
		float softness = 0.1f;
		int feather = 0;
		int expand = 0;
		float smoothing = 1.0f;
		float despill = 0.0f;

		os_event_wait(filter->wake_event);

		pthread_mutex_lock(&filter->mutex);
		if (filter->stop) {
			pthread_mutex_unlock(&filter->mutex);
			break;
		}

		const char *wanted = filter->model ? filter->model : "";
		const char *loaded = filter->loaded_model ? filter->loaded_model : "";
		if (strcmp(wanted, loaded) != 0 || filter->family != filter->loaded_family) {
			requested_model = bstrdup(wanted);
			requested_family = filter->family;
		}

		if (filter->input_pending) {
			/* Take ownership of the filled buffer; the graphics
			 * thread will write into the other one meanwhile. */
			work_rgba = filter->rgba[filter->pending_buffer];
			timestamp = filter->pending_timestamp;
			threshold = (float)filter->threshold;
			softness = (float)filter->softness;
			feather = filter->feather;
			expand = filter->expand;
			smoothing = (float)filter->smoothing;
			despill = (float)filter->despill;
			filter->input_pending = false;
			filter->write_buffer = 1 - filter->pending_buffer;
		}
		pthread_mutex_unlock(&filter->mutex);

		if (requested_model)
			background_reload_model(filter, requested_model, requested_family);

		if (work_rgba)
			background_run_inference(filter, work_rgba, timestamp, threshold, softness, feather, expand,
						 smoothing, despill);
	}

	matting_ort_release_session(&filter->ort, &filter->session);
	matting_ort_unload(&filter->ort);
	bfree(filter->loaded_model);
	bfree(filter->input_tensor);
	bfree(filter->matte_raw);
	background_mask_free(&filter->worker_mask);
	background_mask_free(&filter->history_mask);
	background_mask_free(&filter->scratch_mask);

	return NULL;
}

static void background_update(void *data, obs_data_t *settings)
{
	struct background_data *filter = data;
	char *model = bstrdup(obs_data_get_string(settings, S_MODEL));

	pthread_mutex_lock(&filter->mutex);
	bfree(filter->model);
	filter->model = model;
	filter->family = (enum matting_model_family)obs_data_get_int(settings, S_MODEL_FAMILY);
	filter->inference_fps = (int)obs_data_get_int(settings, S_INFERENCE_FPS);
	filter->threshold = obs_data_get_double(settings, S_THRESHOLD);
	filter->softness = obs_data_get_double(settings, S_SOFTNESS);
	filter->feather = (int)obs_data_get_int(settings, S_FEATHER);
	filter->expand = (int)obs_data_get_int(settings, S_EXPAND);
	filter->smoothing = obs_data_get_double(settings, S_SMOOTHING);
	filter->despill = obs_data_get_double(settings, S_DESPILL);
	filter->reject = obs_data_get_double(settings, S_REJECT);
	filter->show_mask = obs_data_get_bool(settings, S_SHOW_MASK);
	pthread_mutex_unlock(&filter->mutex);

	os_event_signal(filter->wake_event);
}

static void *background_create(obs_data_t *settings, obs_source_t *context)
{
	struct background_data *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	pthread_mutex_init(&filter->mutex, NULL);
	if (os_event_init(&filter->wake_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;

	/* The readback buffers are sized once a model is loaded, since MODNet
	 * and RVM are exported at different resolutions. */

	char *effect_path = obs_module_file("background_alpha.effect");
	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);

	if (!filter->effect) {
		blog(LOG_ERROR, "[background segmentation] could not load background_alpha.effect");
		goto fail;
	}

	background_update(filter, settings);
	if (pthread_create(&filter->worker, NULL, background_worker, filter) != 0)
		goto fail;
	filter->worker_started = true;
	return filter;

fail:
	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}
	if (filter->wake_event)
		os_event_destroy(filter->wake_event);
	bfree(filter->rgba[0]);
	bfree(filter->rgba[1]);
	bfree(filter->model);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
	return NULL;
}

static void background_destroy(void *data)
{
	struct background_data *filter = data;

	if (!filter)
		return;

	pthread_mutex_lock(&filter->mutex);
	filter->stop = true;
	pthread_mutex_unlock(&filter->mutex);

	if (filter->worker_started) {
		os_event_signal(filter->wake_event);
		pthread_join(filter->worker, NULL);
	}

	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	gs_texture_destroy(filter->mask_texture);
	if (filter->readback_render)
		gs_texrender_destroy(filter->readback_render);
	if (filter->readback_stage)
		gs_stagesurface_destroy(filter->readback_stage);
	obs_leave_graphics();

	background_mask_free(&filter->published_mask);
	background_mask_free(&filter->upload_mask);
	bfree(filter->rgba[0]);
	bfree(filter->rgba[1]);
	os_event_destroy(filter->wake_event);
	bfree(filter->model);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

/* Renders the filter target into a small texture and copies it back to CPU
 * memory for the worker.  Graphics thread only.  Returns without doing anything
 * if the inference rate says it is not time yet, which is what keeps the
 * readback cost proportional to the inference rate rather than the frame rate.
 */
static void background_capture_input(struct background_data *filter, obs_source_t *target, obs_source_t *parent)
{
	int inference_fps;
	bool busy;
	uint32_t size;

	pthread_mutex_lock(&filter->mutex);
	inference_fps = filter->inference_fps;
	busy = filter->input_pending;
	size = filter->required_size;
	pthread_mutex_unlock(&filter->mutex);

	/* No model loaded yet, so there is nothing to feed. */
	if (!size)
		return;

	/* The worker has not picked up the previous frame yet; skipping here
	 * keeps the queue at one frame deep instead of building latency. */
	if (busy)
		return;

	const uint64_t now = obs_get_video_frame_time();
	const uint64_t interval = inference_fps > 0 ? 1000000000ULL / (uint64_t)inference_fps : 0;
	if (interval && filter->last_readback_time && now > filter->last_readback_time &&
	    now - filter->last_readback_time < interval)
		return;

	/* Follow the active model's geometry, reallocating only when it
	 * actually changes. */
	if (filter->rgba_size != size) {
		const size_t required = (size_t)size * size * 4;

		if (required > filter->rgba_capacity) {
			uint8_t *first = bmalloc(required);
			uint8_t *second = bmalloc(required);

			if (!first || !second) {
				bfree(first);
				bfree(second);
				return;
			}

			bfree(filter->rgba[0]);
			bfree(filter->rgba[1]);
			filter->rgba[0] = first;
			filter->rgba[1] = second;
			filter->rgba_capacity = required;
		}

		filter->rgba_size = size;
		filter->write_buffer = 0;
	}

	if (filter->readback_stage && filter->readback_size != size) {
		gs_stagesurface_destroy(filter->readback_stage);
		filter->readback_stage = NULL;
	}

	if (!filter->readback_render)
		filter->readback_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!filter->readback_stage) {
		filter->readback_stage = gs_stagesurface_create(size, size, GS_RGBA);
		filter->readback_size = size;
	}
	if (!filter->readback_render || !filter->readback_stage)
		return;

	gs_texrender_reset(filter->readback_render);

	/* Draw the source scaled into the model's square input.  Aspect ratio
	 * is intentionally not preserved: letterboxing would put hard borders
	 * inside the model's field of view, and the matte is stretched back
	 * over the frame the same way, so the distortion cancels out. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->readback_render, size, size)) {
		const uint32_t parent_flags = obs_source_get_output_flags(parent);
		const bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		const uint32_t cx = obs_source_get_base_width(target);
		const uint32_t cy = obs_source_get_base_height(target);
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		if (cx && cy) {
			/* Map the source's own coordinate space onto the square
			 * render target, which performs the downscale. */
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			if (target == parent && !custom_draw && !async)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);
		}

		gs_texrender_end(filter->readback_render);

		gs_texture_t *texture = gs_texrender_get_texture(filter->readback_render);
		if (cx && cy && texture) {
			gs_stage_texture(filter->readback_stage, texture);

			uint8_t *mapped = NULL;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(filter->readback_stage, &mapped, &linesize) && mapped) {
				pthread_mutex_lock(&filter->mutex);
				uint8_t *dst = filter->rgba[filter->write_buffer];
				const uint32_t row = size * 4;

				/* The staging surface may be padded, so copy
				 * row by row rather than in one block. */
				if (linesize == row) {
					memcpy(dst, mapped, (size_t)row * size);
				} else {
					for (uint32_t y = 0; y < size; y++)
						memcpy(dst + (size_t)y * row, mapped + (size_t)y * linesize, row);
				}

				filter->pending_buffer = filter->write_buffer;
				filter->pending_timestamp = now;
				filter->input_pending = true;
				pthread_mutex_unlock(&filter->mutex);

				gs_stagesurface_unmap(filter->readback_stage);
				filter->last_readback_time = now;
				os_event_signal(filter->wake_event);
			} else {
				gs_stagesurface_unmap(filter->readback_stage);
			}
		}
	}

	gs_blend_state_pop();
}

/* Copies the newest matte out from under the lock and uploads it.  Called on
 * the graphics thread only.
 *
 * Returns how strongly the matte should be applied: 1 while it is current,
 * ramping to 0 as it goes stale, so a worker that falls behind degrades into
 * the untouched source gradually instead of in one step. */
static float background_sync_mask_texture(struct background_data *filter)
{
	float strength = 0.0f;
	bool changed = false;

	if (pthread_mutex_trylock(&filter->mutex) != 0) {
		/* Contended: reuse whatever is already uploaded rather than
		 * stalling the graphics thread.  Age is re-evaluated next
		 * frame. */
		return filter->mask_texture ? filter->last_strength : 0.0f;
	}

	const uint64_t now = obs_get_video_frame_time();

	if (filter->published_ready && filter->published_timestamp && now >= filter->published_timestamp) {
		const uint64_t age = now - filter->published_timestamp;

		if (age <= BACKGROUND_MASK_FADE_START_NS) {
			strength = 1.0f;
		} else if (age < BACKGROUND_MASK_TIMEOUT_NS) {
			const uint64_t span = BACKGROUND_MASK_TIMEOUT_NS - BACKGROUND_MASK_FADE_START_NS;
			strength = 1.0f - (float)(age - BACKGROUND_MASK_FADE_START_NS) / (float)span;
		}
	}

	if (strength > 0.0f && filter->published_serial != filter->uploaded_serial) {
		if (background_mask_resize(&filter->upload_mask, filter->published_mask.width,
					   filter->published_mask.height)) {
			memcpy(filter->upload_mask.data, filter->published_mask.data,
			       (size_t)filter->published_mask.width * filter->published_mask.height);
			filter->uploaded_serial = filter->published_serial;
			filter->upload_colour = filter->published_colour;
			changed = true;
		}
	}
	pthread_mutex_unlock(&filter->mutex);

	filter->last_strength = strength;

	if (strength <= 0.0f)
		return 0.0f;

	if (changed) {
		const uint32_t width = filter->upload_mask.width;
		const uint32_t height = filter->upload_mask.height;

		if (!filter->mask_texture || filter->mask_texture_width != width ||
		    filter->mask_texture_height != height) {
			gs_texture_destroy(filter->mask_texture);
			filter->mask_texture = gs_texture_create(width, height, GS_R8, 1, NULL, GS_DYNAMIC);
			filter->mask_texture_width = width;
			filter->mask_texture_height = height;
		}

		if (filter->mask_texture)
			gs_texture_set_image(filter->mask_texture, filter->upload_mask.data, width, false);
	}

	return filter->mask_texture ? strength : 0.0f;
}

static void background_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct background_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!filter->effect || !target || !parent) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	/* Feed the worker before compositing, so the matte is at most one
	 * inference interval behind rather than two. */
	background_capture_input(filter, target, parent);

	const float strength = background_sync_mask_texture(filter);
	if (strength <= 0.0f) {
		/* No matte yet, or inference fell too far behind: show the
		 * source as-is instead of a hole in the scene. */
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;

	bool show_mask;
	float despill;
	float reject;
	pthread_mutex_lock(&filter->mutex);
	show_mask = filter->show_mask;
	despill = (float)filter->despill;
	reject = (float)filter->reject;
	pthread_mutex_unlock(&filter->mutex);

	/* Both corrections are defined relative to the measured wall colour.
	 * Without one there is nothing to unmix or key against, and guessing
	 * would tint the edge or eat the subject rather than clean anything. */
	if (!filter->upload_colour.valid) {
		despill = 0.0f;
		reject = 0.0f;
	}

	struct vec2 mask_scale = {1.0f, 1.0f};
	struct vec2 mask_offset = {0.0f, 0.0f};

	gs_eparam_t *param = gs_effect_get_param_by_name(filter->effect, "mask_tex");
	gs_effect_set_texture(param, filter->mask_texture);

	param = gs_effect_get_param_by_name(filter->effect, "mask_scale");
	gs_effect_set_vec2(param, &mask_scale);

	param = gs_effect_get_param_by_name(filter->effect, "mask_offset");
	gs_effect_set_vec2(param, &mask_offset);

	param = gs_effect_get_param_by_name(filter->effect, "mask_strength");
	gs_effect_set_float(param, strength);

	param = gs_effect_get_param_by_name(filter->effect, "show_mask");
	gs_effect_set_float(param, show_mask ? 1.0f : 0.0f);

	/* The estimate is 0-255 to match the frame bytes it was measured from;
	 * the shader works in normalised colour. */
	struct vec3 colour;
	vec3_set(&colour, filter->upload_colour.rgb[0] / 255.0f, filter->upload_colour.rgb[1] / 255.0f,
		 filter->upload_colour.rgb[2] / 255.0f);

	param = gs_effect_get_param_by_name(filter->effect, "background_colour");
	gs_effect_set_vec3(param, &colour);

	param = gs_effect_get_param_by_name(filter->effect, "despill_strength");
	gs_effect_set_float(param, despill);

	param = gs_effect_get_param_by_name(filter->effect, "despill_floor");
	gs_effect_set_float(param, BACKGROUND_DESPILL_FLOOR);

	/* The tolerance is authored in 0-255 units to match how an operator
	 * reads pixel values; the shader compares normalised colour. */
	param = gs_effect_get_param_by_name(filter->effect, "reject_tolerance");
	gs_effect_set_float(param, reject / 255.0f);

	param = gs_effect_get_param_by_name(filter->effect, "reject_softness");
	gs_effect_set_float(param, BACKGROUND_REJECT_SOFTNESS);

	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
}

/* Lists every .onnx in the segmentation model directory. */
static void background_fill_model_list(obs_property_t *list)
{
	obs_property_list_add_string(list, obs_module_text("BackgroundSegmentation.NoModel"), "");

	/* Created here because it is the location the properties page tells the
	 * user to use, and it sits in the configuration tree, which is
	 * writable in both portable and installed deployments. */
	char *dir_path = background_model_dir();
	if (dir_path) {
		os_mkdirs(dir_path);
		background_add_models_from(list, dir_path);
		bfree(dir_path);
	}

	/* The same folder name directly under obs-studio, which is the other
	 * place someone reasonably puts it on a portable copy. */
	char *nested = background_model_dir_variant(true);
	if (nested) {
		background_add_models_from(list, nested);
		bfree(nested);
	}

	/* Anything shipped with the deployment, listed after the user's own so
	 * the configuration directory takes precedence on a name clash. */
	char *bundled_dir = obs_module_file(BACKGROUND_MODEL_DIR);
	if (bundled_dir) {
		background_add_models_from(list, bundled_dir);
		bfree(bundled_dir);
	}
}

static obs_properties_t *background_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "model_info", obs_module_text("BackgroundSegmentation.ModelInfo"),
				OBS_TEXT_INFO);

	/* Show the resolved path: it differs between portable and installed
	 * deployments, so quoting a fixed one would be wrong for half of
	 * them. */
	char *dir_path = background_model_dir();
	if (dir_path) {
		struct dstr label = {0};
		dstr_printf(&label, "%s\n%s", obs_module_text("BackgroundSegmentation.ModelDir"), dir_path);
		obs_properties_add_text(props, "model_dir", label.array, OBS_TEXT_INFO);
		dstr_free(&label);
		bfree(dir_path);
	}

	obs_property_t *model = obs_properties_add_list(props, S_MODEL, obs_module_text("BackgroundSegmentation.Model"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	background_fill_model_list(model);

	/* The family determines the tensor contract, so it has to be right.
	 * Automatic infers it from the filename, which covers the conventional
	 * release names; the explicit entries are the fallback for renamed or
	 * self-exported models. */
	obs_property_t *family = obs_properties_add_list(props, S_MODEL_FAMILY,
							 obs_module_text("BackgroundSegmentation.Family"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(family, obs_module_text("BackgroundSegmentation.Family.Auto"), MATTING_MODEL_AUTO);
	obs_property_list_add_int(family, "MODNet", MATTING_MODEL_MODNET);
	obs_property_list_add_int(family, "RobustVideoMatting", MATTING_MODEL_RVM);

	obs_property_t *fps = obs_properties_add_list(props, S_INFERENCE_FPS,
						      obs_module_text("BackgroundSegmentation.InferenceFps"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "5", 5);
	obs_property_list_add_int(fps, "10", 10);
	obs_property_list_add_int(fps, "15", 15);
	obs_property_list_add_int(fps, "20", 20);
	obs_property_list_add_int(fps, "30", 30);

	obs_properties_add_float_slider(props, S_THRESHOLD, obs_module_text("BackgroundSegmentation.Threshold"), 0.05,
					0.95, 0.01);
	obs_properties_add_float_slider(props, S_SOFTNESS, obs_module_text("BackgroundSegmentation.Softness"), 0.0, 0.5,
					0.01);
	obs_properties_add_int_slider(props, S_FEATHER, obs_module_text("BackgroundSegmentation.Feather"), 0, 16, 1);
	obs_properties_add_int_slider(props, S_EXPAND, obs_module_text("BackgroundSegmentation.Expand"), -8, 8, 1);
	obs_properties_add_float_slider(props, S_SMOOTHING, obs_module_text("BackgroundSegmentation.Smoothing"), 0.1,
					1.0, 0.05);
	obs_properties_add_float_slider(props, S_DESPILL, obs_module_text("BackgroundSegmentation.Despill"), 0.0, 1.0,
					0.05);
	obs_properties_add_float_slider(props, S_REJECT, obs_module_text("BackgroundSegmentation.Reject"), 0.0, 64.0,
					1.0);
	obs_properties_add_bool(props, S_SHOW_MASK, obs_module_text("BackgroundSegmentation.ShowMask"));

	UNUSED_PARAMETER(data);
	return props;
}

static void background_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_MODEL, "");
	obs_data_set_default_int(settings, S_MODEL_FAMILY, MATTING_MODEL_AUTO);
	/* The inference rate sets how far behind the matte can be: at 15 FPS a
	 * silhouette is up to ~67ms old before compositing even starts, which
	 * is what shows up as lag around a moving subject.
	 *
	 * Measured at 256px with the thread count chosen in matting-ort.c, both
	 * models land near 17-26ms per inference, so 15 FPS leaves ample margin
	 * and keeps the matte current enough for normal movement.  Lower it if
	 * the machine cannot keep up; raise it for fast motion. */
	obs_data_set_default_int(settings, S_INFERENCE_FPS, 15);
	obs_data_set_default_double(settings, S_THRESHOLD, 0.5);
	obs_data_set_default_double(settings, S_SOFTNESS, 0.1);
	obs_data_set_default_int(settings, S_FEATHER, 2);
	obs_data_set_default_int(settings, S_EXPAND, 0);
	obs_data_set_default_double(settings, S_SMOOTHING, 0.6);
	obs_data_set_default_double(settings, S_DESPILL, 0.6);
	obs_data_set_default_double(settings, S_REJECT, 24.0);
	obs_data_set_default_bool(settings, S_SHOW_MASK, false);
}

struct obs_source_info background_segmentation_filter = {
	.id = "background_segmentation_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = background_name,
	.create = background_create,
	.destroy = background_destroy,
	.update = background_update,
	.get_properties = background_properties,
	.get_defaults = background_defaults,
	.video_render = background_render,
};
