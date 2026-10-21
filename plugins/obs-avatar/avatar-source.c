#include "avatar-source.h"
#include "avatar-assets.h"
#include "avatar-audio-tap.h"
#include "avatar-engine-sidecar.h"
#include "avatar-engine-viseme.h"

#include <graphics/image-file.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define S_IMAGE "avatar_image"
#define S_AUDIO_SOURCE "audio_source"
#define S_LOG_LEVEL "log_audio_level"
#define S_MODE "lipsync_mode"
#define S_FPS "generate_fps"

#define S_AUTO_WORKER "auto_worker"
#define S_MOUTH_GAIN "mouth_gain"
#define S_WORKER_PYTHON "worker_python"
#define S_MUSETALK_DIR "musetalk_dir"
#define S_WORKER_SCRIPT "worker_script"

/* Where the MuseTalk checkout (code and weights) lives when nothing else says
 * so; the worker's own --musetalk-dir default is the same place. MUSETALK_DIR
 * in the environment wins, as it does for the worker. */
#define AVATAR_DEFAULT_MUSETALK_DIR "D:\\github\\MuseTalk"

#define S_MODE_OFF "off"
#define S_MODE_SIDECAR "sidecar"
#define S_MODE_VISEME "viseme"

#define S_MOUTH_X "mouth_x"
#define S_MOUTH_Y "mouth_y"
#define S_MOUTH_W "mouth_w"
#define S_MOUTH_H "mouth_h"
#define S_MOUTH_TRAVEL "mouth_travel"
#define S_SENSITIVITY "sensitivity"
#define S_TEETH "mouth_teeth"
#define S_TONGUE "mouth_tongue"
#define S_SHOW_MOUTH "show_mouth"

/* Slider limits, repeated here because the mouse handlers clamp to the same
 * range the sliders do - a wheel that could drive the size outside what the
 * dialog can express would leave a value the user cannot see or undo. */
#define AVATAR_MOUTH_W_MIN 1.0
#define AVATAR_MOUTH_W_MAX 60.0
#define AVATAR_MOUTH_H_MIN 1.0
#define AVATAR_MOUTH_H_MAX 40.0

/* Size change per wheel notch. */
#define AVATAR_WHEEL_STEP 0.08

#define MT_ obs_module_text
#define TEXT_SOURCE_NAME MT_("Avatar.Source")
#define TEXT_IMAGE MT_("Avatar.Image")
#define TEXT_IMAGE_FILTER MT_("Avatar.Image.Filter")
#define TEXT_AUDIO_SOURCE MT_("Avatar.AudioSource")
#define TEXT_AUDIO_SOURCE_NONE MT_("Avatar.AudioSource.None")
#define TEXT_LOG_LEVEL MT_("Avatar.LogAudioLevel")
#define TEXT_STAGE_INFO MT_("Avatar.StageInfo")
#define TEXT_ASSET_DIR MT_("Avatar.AssetDir")
#define TEXT_MODE MT_("Avatar.Mode")
#define TEXT_MODE_OFF MT_("Avatar.Mode.Off")
#define TEXT_MODE_SIDECAR MT_("Avatar.Mode.Sidecar")
#define TEXT_MODE_VISEME MT_("Avatar.Mode.Viseme")
#define TEXT_FPS MT_("Avatar.Fps")
#define TEXT_MOUTH_X MT_("Avatar.Mouth.X")
#define TEXT_MOUTH_Y MT_("Avatar.Mouth.Y")
#define TEXT_MOUTH_W MT_("Avatar.Mouth.Width")
#define TEXT_MOUTH_H MT_("Avatar.Mouth.Height")
#define TEXT_MOUTH_TRAVEL MT_("Avatar.Mouth.Travel")
#define TEXT_SENSITIVITY MT_("Avatar.Sensitivity")
#define TEXT_MOUTH_HINT MT_("Avatar.Mouth.Hint")
#define TEXT_SHOW_MOUTH MT_("Avatar.Mouth.Show")
#define TEXT_TEETH MT_("Avatar.Mouth.Teeth")
#define TEXT_TONGUE MT_("Avatar.Mouth.Tongue")
#define TEXT_AUTO_WORKER MT_("Avatar.Worker.Auto")
#define TEXT_MOUTH_GAIN MT_("Avatar.Worker.MouthGain")
#define TEXT_WORKER_PYTHON MT_("Avatar.Worker.Python")
#define TEXT_MUSETALK_DIR MT_("Avatar.Worker.MuseTalkDir")
#define TEXT_WORKER_SCRIPT MT_("Avatar.Worker.Script")
#define TEXT_PIPE MT_("Avatar.Pipe")
#define TEXT_PIPE_IDLE MT_("Avatar.Pipe.Idle")

/* Placeholder square drawn before an avatar asset has been chosen.  Without it
 * a freshly added source measures 0x0 - invisible in the scene, and so
 * impossible to position before picking the artwork. */
#define AVATAR_PLACEHOLDER_SIZE 320

/* Tap logging cadence: a line a second while there is sound, one every five
 * while there is not.  Silence has to be logged too, or "the tap never
 * attached" and "nobody is talking" look identical in the log. */
#define AVATAR_LOG_ACTIVE_NS 1000000000ULL
#define AVATAR_LOG_IDLE_NS 5000000000ULL

/* How often the asset file's timestamp is checked, as in image-source. */
#define AVATAR_RELOAD_CHECK_S 1.0f

/* Samples handed to the engine per read. */
#define AVATAR_DRAIN_CHUNK 4096

/* How long a generated frame may be the newest one before the source stops
 * showing it.  §7.4 of the design doc: hold the last frame over a brief gap -
 * blinking to something else on a hiccup is far more noticeable than a mouth
 * that pauses - but a worker that has stopped producing must not leave a
 * frozen mouth on stream pretending to speak. */
#define AVATAR_FRAME_STALE_NS 500000000ULL

struct avatar_source {
	obs_source_t *source;

	char *image_path;
	time_t image_timestamp;
	gs_image_file4_t if4;
	bool image_loaded;
	float reload_elapsed;
	uint64_t last_frame_time;

	struct avatar_audio_tap *tap;
	bool log_level;

	/* Levels are taken on a fixed cadence (taking clears them) but not
	 * every take is printed - silence stretches the line out to five
	 * seconds.  So taken values accumulate here; otherwise the peak over a
	 * quiet stretch would be overwritten by the takes in between and the
	 * line would only ever describe its last second. */
	uint64_t level_time;
	uint64_t last_log_time;
	bool pending_audio;
	float pending_peak_db;
	float pending_rms_db;
	uint64_t pending_dropped;
	uint64_t pending_samples;

	float *drain;

	/* Set while the left button is held inside the interact window. */
	bool dragging;

	/* The engine, and the running audio timeline its frames are labelled
	 * with.  Guarded because update() runs on the UI thread while the
	 * graphics and video threads are reading it. */
	pthread_mutex_t engine_mutex;
	const struct avatar_engine_ops *ops;
	void *engine;
	char *mode;
	uint32_t fps;
	int64_t audio_pts;

	/* The worker launch settings joined into one string, so a change to any
	 * of them is a single comparison rather than four. */
	char *worker_key;

	/* The generated frame.  The texture and its dimensions belong to the
	 * graphics thread (video_render); the two flags and the arrival time are
	 * also read by the video thread's staleness check, so they are atomic
	 * rather than plain bools.  Nothing here is a pointer the video thread
	 * dereferences - it only ever asks "is there a recent frame". */
	gs_texture_t *texture;
	uint32_t tex_width;
	uint32_t tex_height;
	int64_t frame_pts;
	uint64_t frame_arrived;
	volatile bool have_frame;
	volatile bool stale_logged;
};

/* Defined with the rest of the geometry below; the mouse handlers need them to
 * turn source coordinates into fractions of the artwork. */
static uint32_t avatar_source_get_width(void *data);
static uint32_t avatar_source_get_height(void *data);

/* Clears the accumulated summary back to "nothing heard". */
static void avatar_reset_pending(struct avatar_source *ctx)
{
	ctx->pending_audio = false;
	ctx->pending_peak_db = AVATAR_TAP_SILENCE_FLOOR_DB;
	ctx->pending_rms_db = AVATAR_TAP_SILENCE_FLOOR_DB;
	ctx->pending_dropped = 0;
	ctx->pending_samples = 0;
}

static const char *avatar_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_SOURCE_NAME;
}

static time_t avatar_file_timestamp(const char *path)
{
	struct stat stats;
	if (!path || !*path || os_stat(path, &stats) != 0)
		return -1;
	return stats.st_mtime;
}

static void avatar_unload_image(struct avatar_source *ctx)
{
	if (!ctx->image_loaded)
		return;

	obs_enter_graphics();
	gs_image_file4_free(&ctx->if4);
	obs_leave_graphics();

	ctx->image_loaded = false;
}

static void avatar_load_image(struct avatar_source *ctx)
{
	avatar_unload_image(ctx);

	if (!ctx->image_path || !*ctx->image_path)
		return;

	ctx->image_timestamp = avatar_file_timestamp(ctx->image_path);

	/* Premultiplied alpha: avatar artwork almost always has transparent
	 * edges - a cut-out portrait, a cartoon head on no background - and
	 * compositing straight alpha over the scene leaves a dark fringe. */
	gs_image_file4_init(&ctx->if4, ctx->image_path, GS_IMAGE_ALPHA_PREMULTIPLY);

	obs_enter_graphics();
	gs_image_file4_init_texture(&ctx->if4);
	obs_leave_graphics();

	ctx->image_loaded = true;
	ctx->reload_elapsed = 0.0f;
	ctx->last_frame_time = 0;

	if (!ctx->if4.image3.image2.image.loaded)
		blog(LOG_WARNING, "[obs-avatar: '%s'] failed to load avatar asset '%s'",
		     obs_source_get_name(ctx->source), ctx->image_path);
}

/* ---------------------------------------------------------------------------
 * Engine
 * ------------------------------------------------------------------------- */

static const struct avatar_engine_ops *avatar_ops_for_mode(const char *mode)
{
	if (!mode)
		return NULL;
	if (strcmp(mode, S_MODE_SIDECAR) == 0)
		return &avatar_engine_sidecar_ops;
	if (strcmp(mode, S_MODE_VISEME) == 0)
		return &avatar_engine_viseme_ops;
	return NULL;
}

/* Mode B's mouth region, read straight from the settings.  Percentages in the
 * UI, fractions of the image in the engine: the numbers then survive the asset
 * being re-exported at a different resolution. */
static void avatar_apply_mouth(struct avatar_source *ctx, obs_data_t *settings)
{
	pthread_mutex_lock(&ctx->engine_mutex);
	const bool viseme = ctx->ops == &avatar_engine_viseme_ops && ctx->engine;
	void *engine = ctx->engine;
	pthread_mutex_unlock(&ctx->engine_mutex);

	if (!viseme)
		return;

	const float sensitivity = (float)obs_data_get_double(settings, S_SENSITIVITY) / 100.0f;

	struct avatar_viseme_mouth mouth = {
		.center_x = (float)obs_data_get_double(settings, S_MOUTH_X) / 100.0f,
		.center_y = (float)obs_data_get_double(settings, S_MOUTH_Y) / 100.0f,
		.radius_x = (float)obs_data_get_double(settings, S_MOUTH_W) / 100.0f,
		.radius_y = (float)obs_data_get_double(settings, S_MOUTH_H) / 100.0f,
		.travel = (float)obs_data_get_double(settings, S_MOUTH_TRAVEL) / 100.0f,
	};

	/* Sensitivity moves the gate rather than scaling the openness: turning
	 * it up should make a quiet voice open the mouth fully, not make a loud
	 * one overshoot.  The span above the gate is kept fixed so the mouth's
	 * dynamic range does not change shape as the slider moves. */
	mouth.gate_db = -35.0f - 25.0f * sensitivity;
	mouth.full_db = mouth.gate_db + 30.0f;
	mouth.teeth = (float)obs_data_get_double(settings, S_TEETH) / 100.0f;
	mouth.tongue = (float)obs_data_get_double(settings, S_TONGUE) / 100.0f;
	mouth.show_region = obs_data_get_bool(settings, S_SHOW_MOUTH);

	avatar_engine_viseme_set_mouth(engine, &mouth);
}

/* How far the real-person worker lets the mouth travel.  Pushed to the running
 * engine, not carried in its config: the worker takes it as a message, so
 * dragging the slider is not a minute-long restart per notch. */
static void avatar_apply_gain(struct avatar_source *ctx, obs_data_t *settings)
{
	pthread_mutex_lock(&ctx->engine_mutex);
	if (ctx->ops == &avatar_engine_sidecar_ops && ctx->engine)
		avatar_engine_sidecar_set_mouth_gain(ctx->engine,
						     (float)obs_data_get_double(settings, S_MOUTH_GAIN) / 100.0f);
	pthread_mutex_unlock(&ctx->engine_mutex);
}

/* ---------------------------------------------------------------------------
 * Placing the mouth with the mouse
 *
 * Six sliders can express where a mouth is, but nobody finds a mouth by
 * dragging numbers. With OBS_SOURCE_INTERACTION the source gets mouse events
 * in its own coordinate space, so the region can be dragged onto the mouth
 * directly - right-click the source and choose Interact.
 *
 * The values are written back into the source's settings rather than kept
 * beside them, so a drag persists across restarts and the sliders show where
 * the mouse put things.
 * ------------------------------------------------------------------------- */

static void avatar_set_mouth_center(struct avatar_source *ctx, int32_t x, int32_t y)
{
	const uint32_t cx = avatar_source_get_width(ctx);
	const uint32_t cy = avatar_source_get_height(ctx);
	if (!cx || !cy)
		return;

	double px = 100.0 * (double)x / (double)cx;
	double py = 100.0 * (double)y / (double)cy;

	/* A drag that leaves the source still has to land somewhere usable. */
	px = (px < 0.0) ? 0.0 : (px > 100.0 ? 100.0 : px);
	py = (py < 0.0) ? 0.0 : (py > 100.0 ? 100.0 : py);

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	obs_data_set_double(settings, S_MOUTH_X, px);
	obs_data_set_double(settings, S_MOUTH_Y, py);
	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
}

static void avatar_scale_mouth(struct avatar_source *ctx, int notches)
{
	if (!notches)
		return;

	const double factor = 1.0 + AVATAR_WHEEL_STEP * (double)notches;

	obs_data_t *settings = obs_source_get_settings(ctx->source);

	double w = obs_data_get_double(settings, S_MOUTH_W) * factor;
	double h = obs_data_get_double(settings, S_MOUTH_H) * factor;

	w = (w < AVATAR_MOUTH_W_MIN) ? AVATAR_MOUTH_W_MIN : (w > AVATAR_MOUTH_W_MAX ? AVATAR_MOUTH_W_MAX : w);
	h = (h < AVATAR_MOUTH_H_MIN) ? AVATAR_MOUTH_H_MIN : (h > AVATAR_MOUTH_H_MAX ? AVATAR_MOUTH_H_MAX : h);

	obs_data_set_double(settings, S_MOUTH_W, w);
	obs_data_set_double(settings, S_MOUTH_H, h);
	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
}

/* Mode B only: a worker does its own face detection, so dragging a region at
 * it would do nothing. */
static bool avatar_mouse_applies(struct avatar_source *ctx)
{
	pthread_mutex_lock(&ctx->engine_mutex);
	const bool viseme = ctx->ops == &avatar_engine_viseme_ops && ctx->engine;
	pthread_mutex_unlock(&ctx->engine_mutex);
	return viseme;
}

static void avatar_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			       uint32_t click_count)
{
	struct avatar_source *ctx = data;

	UNUSED_PARAMETER(click_count);

	if (type != MOUSE_LEFT || !avatar_mouse_applies(ctx))
		return;

	ctx->dragging = !mouse_up;
	if (!mouse_up)
		avatar_set_mouth_center(ctx, event->x, event->y);
}

static void avatar_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct avatar_source *ctx = data;

	if (mouse_leave) {
		/* The button-up will never arrive if the pointer left while
		 * held, so the drag has to end here or it would resume on the
		 * next move. */
		ctx->dragging = false;
		return;
	}

	if (ctx->dragging && avatar_mouse_applies(ctx))
		avatar_set_mouth_center(ctx, event->x, event->y);
}

static void avatar_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	struct avatar_source *ctx = data;

	UNUSED_PARAMETER(event);
	UNUSED_PARAMETER(x_delta);

	if (!avatar_mouse_applies(ctx))
		return;

	/* A notch is 120 units by convention. */
	avatar_scale_mouth(ctx, y_delta / 120);
}

/* Installs a new engine and returns the old one for the caller to destroy.
 *
 * The swap happens under the lock but the destroy must not: tearing an engine
 * down joins its reader thread, and blocking the graphics thread on that for
 * the sake of a property edit is how a UI stall becomes a dropped frame. */
static void *avatar_swap_engine(struct avatar_source *ctx, const struct avatar_engine_ops *ops, void *engine,
				const struct avatar_engine_ops **old_ops)
{
	pthread_mutex_lock(&ctx->engine_mutex);

	*old_ops = ctx->ops;
	void *old = ctx->engine;

	ctx->ops = ops;
	ctx->engine = engine;
	ctx->audio_pts = 0;

	pthread_mutex_unlock(&ctx->engine_mutex);
	return old;
}

static const char *avatar_default_musetalk_dir(void)
{
	const char *env = getenv("MUSETALK_DIR");
	return (env && *env) ? env : AVATAR_DEFAULT_MUSETALK_DIR;
}

/* The worker's launch settings as they will be used, with the blanks filled
 * in: an empty interpreter means the virtualenv MuseTalk was installed into
 * (it needs that environment's torch and CUDA, which a system Python will not
 * have), and an empty script means the one in the source tree this build came
 * from. Empty results stay empty, and the engine then says what is missing. */
struct avatar_worker_launch {
	struct dstr python;
	struct dstr musetalk_dir;
	struct dstr script;
	bool enabled;
};

static void avatar_resolve_worker(obs_data_t *settings, struct avatar_worker_launch *out)
{
	memset(out, 0, sizeof(*out));

	out->enabled = obs_data_get_bool(settings, S_AUTO_WORKER);

	dstr_copy(&out->musetalk_dir, obs_data_get_string(settings, S_MUSETALK_DIR));
	dstr_replace(&out->musetalk_dir, "/", "\\");

	dstr_copy(&out->python, obs_data_get_string(settings, S_WORKER_PYTHON));
	if (dstr_is_empty(&out->python) && !dstr_is_empty(&out->musetalk_dir)) {
		struct dstr venv = {0};
		dstr_printf(&venv, "%s\\venv\\Scripts\\python.exe", out->musetalk_dir.array);
		if (os_file_exists(venv.array))
			dstr_copy_dstr(&out->python, &venv);
		dstr_free(&venv);
	}

	dstr_copy(&out->script, obs_data_get_string(settings, S_WORKER_SCRIPT));
#ifdef AVATAR_DEFAULT_WORKER_SCRIPT
	if (dstr_is_empty(&out->script))
		dstr_copy(&out->script, AVATAR_DEFAULT_WORKER_SCRIPT);
#endif
	dstr_replace(&out->script, "/", "\\");
}

static void avatar_free_worker(struct avatar_worker_launch *launch)
{
	dstr_free(&launch->python);
	dstr_free(&launch->musetalk_dir);
	dstr_free(&launch->script);
}

static void avatar_apply_engine(struct avatar_source *ctx, obs_data_t *settings)
{
	const struct avatar_engine_ops *ops = avatar_ops_for_mode(ctx->mode);
	void *engine = NULL;

	if (ops) {
		struct avatar_worker_launch worker;
		avatar_resolve_worker(settings, &worker);

		/* Only the worker engine has a process to start; the sprite engine
		 * would ignore these, but handing them over would suggest it
		 * might. */
		const bool launch = ops == &avatar_engine_sidecar_ops && worker.enabled;
		char *log_path = launch ? avatar_worker_log_path() : NULL;

		const struct avatar_engine_config cfg = {
			.asset_path = ctx->image_path ? ctx->image_path : "",
			.worker_script = launch ? worker.script.array : NULL,
			.worker_python = launch ? worker.python.array : NULL,
			.musetalk_dir = launch ? worker.musetalk_dir.array : NULL,
			.worker_log = log_path,
			.fps = ctx->fps,
			.sample_rate = AVATAR_TAP_SAMPLE_RATE,
			.mouth_gain = (float)obs_data_get_double(settings, S_MOUTH_GAIN) / 100.0f,
		};

		engine = ops->create(&cfg, ctx->source);
		bfree(log_path);
		avatar_free_worker(&worker);
		if (!engine) {
			blog(LOG_ERROR, "[obs-avatar: '%s'] could not start the '%s' engine",
			     obs_source_get_name(ctx->source), ops->id);
			ops = NULL;
		}
	}

	const struct avatar_engine_ops *old_ops = NULL;
	void *old = avatar_swap_engine(ctx, ops, engine, &old_ops);

	if (old_ops && old)
		old_ops->destroy(old);

	/* Whatever is on screen came from the engine that just went away. */
	os_atomic_set_bool(&ctx->have_frame, false);
	os_atomic_set_bool(&ctx->stale_logged, false);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

static void avatar_source_update(void *data, obs_data_t *settings)
{
	struct avatar_source *ctx = data;

	const char *path = obs_data_get_string(settings, S_IMAGE);
	const char *mode = obs_data_get_string(settings, S_MODE);
	const uint32_t fps = (uint32_t)obs_data_get_int(settings, S_FPS);

	const bool image_changed = !ctx->image_path || strcmp(ctx->image_path, path) != 0;
	const bool mode_changed = !ctx->mode || strcmp(ctx->mode, mode) != 0;
	const bool fps_changed = ctx->fps != fps;

	struct dstr worker_key = {0};
	dstr_printf(&worker_key, "%d|%s|%s|%s", (int)obs_data_get_bool(settings, S_AUTO_WORKER),
		    obs_data_get_string(settings, S_WORKER_PYTHON), obs_data_get_string(settings, S_MUSETALK_DIR),
		    obs_data_get_string(settings, S_WORKER_SCRIPT));
	const bool worker_changed = !ctx->worker_key || strcmp(ctx->worker_key, worker_key.array) != 0;
	if (worker_changed) {
		bfree(ctx->worker_key);
		ctx->worker_key = bstrdup(worker_key.array);
	}
	dstr_free(&worker_key);

	if (image_changed) {
		bfree(ctx->image_path);
		ctx->image_path = bstrdup(path);
		avatar_load_image(ctx);
	}

	if (mode_changed) {
		bfree(ctx->mode);
		ctx->mode = bstrdup(mode);
	}

	ctx->fps = fps;
	ctx->log_level = obs_data_get_bool(settings, S_LOG_LEVEL);

	avatar_audio_tap_set_source(ctx->tap, obs_data_get_string(settings, S_AUDIO_SOURCE));

	/* The asset path and frame rate are negotiated in the handshake, so a
	 * change to either has to be renegotiated.  That drops the worker and
	 * waits for it to reconnect, which is the predictable behaviour even if
	 * it is not the cheapest.  How the worker is launched is only read when
	 * it is started, so changing that restarts it too. */
	if (mode_changed || fps_changed || image_changed || worker_changed)
		avatar_apply_engine(ctx, settings);

	/* After the engine exists, and on every edit: the mouth sliders are
	 * dragged live, so they are pushed onto the running engine rather than
	 * carried in the config and costing a restart per pixel moved. */
	avatar_apply_mouth(ctx, settings);
	avatar_apply_gain(ctx, settings);
}

static void *avatar_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct avatar_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->drain = bmalloc(AVATAR_DRAIN_CHUNK * sizeof(float));
	avatar_reset_pending(ctx);

	if (pthread_mutex_init(&ctx->engine_mutex, NULL) != 0) {
		blog(LOG_ERROR, "[obs-avatar] could not create the engine mutex");
		bfree(ctx->drain);
		bfree(ctx);
		return NULL;
	}

	ctx->tap = avatar_audio_tap_create(source);
	if (!ctx->tap) {
		blog(LOG_ERROR, "[obs-avatar] failed to create the audio tap");
		pthread_mutex_destroy(&ctx->engine_mutex);
		bfree(ctx->drain);
		bfree(ctx);
		return NULL;
	}

	avatar_source_update(ctx, settings);
	return ctx;
}

static void avatar_source_destroy(void *data)
{
	struct avatar_source *ctx = data;

	const struct avatar_engine_ops *old_ops = NULL;
	void *old = avatar_swap_engine(ctx, NULL, NULL, &old_ops);
	if (old_ops && old)
		old_ops->destroy(old);

	avatar_audio_tap_destroy(ctx->tap);
	avatar_unload_image(ctx);

	obs_enter_graphics();
	gs_texture_destroy(ctx->texture);
	obs_leave_graphics();

	pthread_mutex_destroy(&ctx->engine_mutex);

	bfree(ctx->drain);
	bfree(ctx->mode);
	bfree(ctx->worker_key);
	bfree(ctx->image_path);
	bfree(ctx);
}

/* ---------------------------------------------------------------------------
 * Geometry
 *
 * The asset's size wins whenever there is an asset, and a generated frame is
 * scaled into that box.  Reporting the frame's size instead would resize the
 * scene item the moment a worker connected, and again when it dropped - the
 * user would position the avatar, then watch it jump.  A stable box is worth
 * more than matching the generator's pixel grid, and once the real engine
 * blends its output back into the full avatar the two sizes agree anyway.
 * ------------------------------------------------------------------------- */

static uint32_t avatar_source_get_width(void *data)
{
	struct avatar_source *ctx = data;

	if (ctx->image_loaded && ctx->if4.image3.image2.image.cx)
		return ctx->if4.image3.image2.image.cx;
	if (ctx->tex_width)
		return ctx->tex_width;
	return AVATAR_PLACEHOLDER_SIZE;
}

static uint32_t avatar_source_get_height(void *data)
{
	struct avatar_source *ctx = data;

	if (ctx->image_loaded && ctx->if4.image3.image2.image.cy)
		return ctx->if4.image3.image2.image.cy;
	if (ctx->tex_height)
		return ctx->tex_height;
	return AVATAR_PLACEHOLDER_SIZE;
}

/* ---------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

/* Deliberately does not look at the texture: the video thread calls this too,
 * and the texture pointer belongs to the graphics thread. */
static bool avatar_frame_fresh(struct avatar_source *ctx)
{
	if (!os_atomic_load_bool(&ctx->have_frame))
		return false;
	return (os_gettime_ns() - ctx->frame_arrived) < AVATAR_FRAME_STALE_NS;
}

/* Pulls the engine's newest frame into a texture.  Called from video_render,
 * so the graphics context is already current.
 *
 * Only a frame with a timestamp we have not seen is uploaded: video_render
 * runs once per output - preview, program, every projector - and re-uploading
 * the same pixels for each of them would be pure waste. */
static void avatar_upload_frame(struct avatar_source *ctx)
{
	pthread_mutex_lock(&ctx->engine_mutex);

	if (!ctx->ops || !ctx->ops->lock_frame) {
		pthread_mutex_unlock(&ctx->engine_mutex);
		return;
	}

	struct avatar_engine_frame frame;
	if (!ctx->ops->lock_frame(ctx->engine, &frame)) {
		pthread_mutex_unlock(&ctx->engine_mutex);
		return;
	}

	if (!os_atomic_load_bool(&ctx->have_frame) || frame.pts_ns != ctx->frame_pts) {
		if (!ctx->texture || ctx->tex_width != frame.width || ctx->tex_height != frame.height) {
			gs_texture_destroy(ctx->texture);
			ctx->texture = gs_texture_create(frame.width, frame.height, GS_BGRA, 1, NULL, GS_DYNAMIC);
			ctx->tex_width = frame.width;
			ctx->tex_height = frame.height;
		}

		if (ctx->texture) {
			/* The engine's frame lock is held across this upload to
			 * avoid copying the frame twice.  A quarter of a
			 * megabyte is microseconds; anything slower would have
			 * to copy out first, because the engine's reader thread
			 * is waiting on this lock. */
			gs_texture_set_image(ctx->texture, frame.bgra, frame.width * 4, false);
			ctx->frame_pts = frame.pts_ns;
			/* Arrival time before the flag: the video thread reads
			 * the flag first and must not then read a stale time. */
			ctx->frame_arrived = os_gettime_ns();
			os_atomic_set_bool(&ctx->have_frame, true);

			if (os_atomic_load_bool(&ctx->stale_logged)) {
				blog(LOG_INFO, "[obs-avatar: '%s'] generated frames resumed",
				     obs_source_get_name(ctx->source));
				os_atomic_set_bool(&ctx->stale_logged, false);
			}
		}
	}

	ctx->ops->unlock_frame(ctx->engine);
	pthread_mutex_unlock(&ctx->engine_mutex);
}

static void avatar_draw_placeholder(void)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");

	struct vec4 fill;
	vec4_set(&fill, 0.07f, 0.08f, 0.10f, 0.65f);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_effect_set_vec4(color, &fill);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(NULL, 0, AVATAR_PLACEHOLDER_SIZE, AVATAR_PLACEHOLDER_SIZE);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

/* `premultiplied` selects the blend for the source factor: the avatar asset is
 * loaded premultiplied, while an engine's frame arrives as straight alpha. */
static void avatar_draw_texture(gs_texture_t *texture, uint32_t cx, uint32_t cy, bool premultiplied)
{
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(premultiplied ? GS_BLEND_ONE : GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

/* Gives a GPU-compositing engine (mode B) its turn.  It needs the artwork, so
 * without one there is nothing for it to deform and the caller falls through to
 * the placeholder. */
static bool avatar_render_engine(struct avatar_source *ctx, gs_texture_t *base, uint32_t cx, uint32_t cy)
{
	if (!base)
		return false;

	pthread_mutex_lock(&ctx->engine_mutex);
	const bool drawn = ctx->ops && ctx->ops->render && ctx->ops->render(ctx->engine, base, cx, cy);
	pthread_mutex_unlock(&ctx->engine_mutex);

	return drawn;
}

static void avatar_source_render(void *data, gs_effect_t *unused)
{
	struct avatar_source *ctx = data;

	UNUSED_PARAMETER(unused);

	const uint32_t cx = avatar_source_get_width(ctx);
	const uint32_t cy = avatar_source_get_height(ctx);

	struct gs_image_file *const image = ctx->image_loaded ? &ctx->if4.image3.image2.image : NULL;
	gs_texture_t *const base = image ? image->texture : NULL;

	/* Mode B draws the artwork itself, so it goes first and there is no
	 * staleness to consider: it is synchronous and always has an answer. */
	if (avatar_render_engine(ctx, base, cx, cy))
		return;

	avatar_upload_frame(ctx);

	/* A fresh generated frame wins.  When it goes stale the still artwork
	 * comes back - once both modes can run, this is where mode B would take
	 * over from a dropped worker instead. */
	if (avatar_frame_fresh(ctx) && ctx->texture) {
		avatar_draw_texture(ctx->texture, cx, cy, false);
		return;
	}

	if (base)
		avatar_draw_texture(base, image->cx, image->cy, true);
	else
		avatar_draw_placeholder();
}

/* ---------------------------------------------------------------------------
 * Tick
 * ------------------------------------------------------------------------- */

/* Hands the tapped audio to the engine, advancing the audio timeline its
 * frames are labelled with.  That timeline - not wall clock - is what makes
 * the lip-sync offset measurable later. */
static size_t avatar_drain_audio(struct avatar_source *ctx)
{
	size_t total = 0;

	for (;;) {
		const size_t read = avatar_audio_tap_read(ctx->tap, ctx->drain, AVATAR_DRAIN_CHUNK);
		if (!read)
			break;

		pthread_mutex_lock(&ctx->engine_mutex);
		if (ctx->ops)
			ctx->ops->push_audio(ctx->engine, ctx->drain, read, ctx->audio_pts);
		ctx->audio_pts += (int64_t)read * 1000000000LL / AVATAR_TAP_SAMPLE_RATE;
		pthread_mutex_unlock(&ctx->engine_mutex);

		total += read;
		if (read < AVATAR_DRAIN_CHUNK)
			break;
	}

	return total;
}

/* Logs the tapped level, and with it the end-to-end lag of the generated
 * frame.  Both timestamps are on the audio timeline, so their difference is
 * exactly the offset stage D has to compensate - no clock comparison needed. */
static void avatar_report_audio(struct avatar_source *ctx, size_t drained)
{
	ctx->pending_samples += drained;

	/* While unattached the level accumulators are left alone, so the first
	 * line after attaching describes audio and not an empty interval. */
	if (!avatar_audio_tap_attached(ctx->tap))
		return;

	const uint64_t now = os_gettime_ns();
	if (!ctx->level_time) {
		ctx->level_time = now;
		ctx->last_log_time = now;
		return;
	}

	if (now - ctx->level_time < AVATAR_LOG_ACTIVE_NS)
		return;
	ctx->level_time = now;

	float peak_db = 0.0f;
	float rms_db = 0.0f;
	uint64_t dropped = 0;

	if (avatar_audio_tap_take_level(ctx->tap, &peak_db, &rms_db, &dropped)) {
		ctx->pending_audio = true;
		if (peak_db > ctx->pending_peak_db)
			ctx->pending_peak_db = peak_db;
		if (rms_db > ctx->pending_rms_db)
			ctx->pending_rms_db = rms_db;
	}
	ctx->pending_dropped += dropped;

	const bool speaking = ctx->pending_audio && ctx->pending_peak_db > AVATAR_TAP_SILENCE_DB;
	if (!speaking && now - ctx->last_log_time < AVATAR_LOG_IDLE_NS)
		return;

	ctx->last_log_time = now;

	if (ctx->log_level) {
		if (!ctx->pending_audio) {
			blog(LOG_INFO, "[obs-avatar: '%s'] tap attached but no audio arrived",
			     obs_source_get_name(ctx->source));
		} else {
			/* Mode A reports how far the picture is behind the voice;
			 * mode B has no lag worth naming (it renders from the
			 * audio it was just handed), so it reports how far open
			 * the mouth got instead. */
			struct dstr lag = {0};

			pthread_mutex_lock(&ctx->engine_mutex);
			const bool viseme = ctx->ops == &avatar_engine_viseme_ops && ctx->engine;
			const float openness = viseme ? avatar_engine_viseme_openness(ctx->engine) : 0.0f;
			pthread_mutex_unlock(&ctx->engine_mutex);

			if (viseme) {
				/* The parameters come back from the engine, not
				 * from the settings: that is the half of the
				 * chain worth proving when the opening is
				 * visible but its contents are not. */
				struct avatar_viseme_mouth m = {0};
				pthread_mutex_lock(&ctx->engine_mutex);
				if (ctx->ops == &avatar_engine_viseme_ops && ctx->engine)
					avatar_engine_viseme_get_mouth(ctx->engine, &m);
				pthread_mutex_unlock(&ctx->engine_mutex);

				dstr_printf(&lag,
					    ", mouth %.0f%% [centre %.2f,%.2f radius %.2f,%.2f "
					    "travel %.2f teeth %.2f tongue %.2f]",
					    (double)openness * 100.0, (double)m.center_x, (double)m.center_y,
					    (double)m.radius_x, (double)m.radius_y, (double)m.travel,
					    (double)m.teeth, (double)m.tongue);
			}
			else if (os_atomic_load_bool(&ctx->have_frame))
				dstr_printf(&lag, ", frame lag %.0f ms",
					    (double)(ctx->audio_pts - ctx->frame_pts) / 1000000.0);

			blog(LOG_INFO,
			     "[obs-avatar: '%s'] tap %s: peak %.1f dBFS, rms %.1f dBFS, "
			     "%llu samples at 16 kHz, %llu dropped%s",
			     obs_source_get_name(ctx->source), speaking ? "speaking" : "silent",
			     ctx->pending_peak_db, ctx->pending_rms_db, (unsigned long long)ctx->pending_samples,
			     (unsigned long long)ctx->pending_dropped, lag.array ? lag.array : "");
			dstr_free(&lag);
		}
	}

	avatar_reset_pending(ctx);
}

/* One line when generated frames stop arriving, one when they come back.  A
 * frozen mouth on stream is worse than a still avatar, so this is the moment
 * the source stops trusting the engine. */
static void avatar_check_stale(struct avatar_source *ctx)
{
	pthread_mutex_lock(&ctx->engine_mutex);
	const bool running = ctx->ops != NULL;
	pthread_mutex_unlock(&ctx->engine_mutex);

	if (!running || !os_atomic_load_bool(&ctx->have_frame) || os_atomic_load_bool(&ctx->stale_logged))
		return;

	if (avatar_frame_fresh(ctx))
		return;

	os_atomic_set_bool(&ctx->stale_logged, true);
	blog(LOG_WARNING, "[obs-avatar: '%s'] no generated frame for %llu ms; falling back to the still avatar",
	     obs_source_get_name(ctx->source), (unsigned long long)(AVATAR_FRAME_STALE_NS / 1000000ULL));
}

static void avatar_source_tick(void *data, float seconds)
{
	struct avatar_source *ctx = data;

	avatar_audio_tap_tick(ctx->tap);
	avatar_report_audio(ctx, avatar_drain_audio(ctx));
	avatar_check_stale(ctx);

	if (!obs_source_showing(ctx->source))
		return;

	/* Reload when the asset is rewritten underneath us - a different image
	 * saved over the same name, a re-export. */
	ctx->reload_elapsed += seconds;
	if (ctx->reload_elapsed >= AVATAR_RELOAD_CHECK_S) {
		ctx->reload_elapsed = 0.0f;
		if (ctx->image_loaded && avatar_file_timestamp(ctx->image_path) != ctx->image_timestamp)
			avatar_load_image(ctx);
	}

	if (!ctx->image_loaded)
		return;

	/* Animated assets advance here, which is all the idle animation needs -
	 * an animated GIF/WebP avatar blinks with no extra code. */
	const uint64_t frame_time = obs_get_video_frame_time();
	if (ctx->last_frame_time && gs_image_file4_tick(&ctx->if4, frame_time - ctx->last_frame_time)) {
		obs_enter_graphics();
		gs_image_file4_update_texture(&ctx->if4);
		obs_leave_graphics();
	}
	ctx->last_frame_time = frame_time;
}

/* ---------------------------------------------------------------------------
 * Properties
 * ------------------------------------------------------------------------- */

static void avatar_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_AUDIO_SOURCE, "none");
	obs_data_set_default_string(settings, S_MODE, S_MODE_OFF);
	/* MuseTalk costs about 40 ms a frame on an RTX 4060 with nothing else on
	 * the GPU, so 25 fps is already no headroom and 30 is slower than real
	 * time.  A worker that cannot keep up drops audio and the mouth lags and
	 * skips; 20 leaves room for OBS's own encoders to share the card. */
	obs_data_set_default_int(settings, S_FPS, 20);
	/* Measured on this model: at 1.0 the mouth barely opens on most faces, 2.0
	 * is clearly visible, and by 3.0 the lip edges start to show artefacts. */
	obs_data_set_default_double(settings, S_MOUTH_GAIN, 200.0);
	obs_data_set_default_bool(settings, S_AUTO_WORKER, true);
	obs_data_set_default_string(settings, S_MUSETALK_DIR, avatar_default_musetalk_dir());

	/* Lower-middle of the frame, which is where a mouth is on most
	 * head-and-shoulders artwork.  It will not be right for any particular
	 * avatar - that is what the sliders are for - but it puts the
	 * deformation somewhere plausible so the first drag is an adjustment
	 * rather than a search. */
	obs_data_set_default_double(settings, S_MOUTH_X, 50.0);
	obs_data_set_default_double(settings, S_MOUTH_Y, 68.0);
	obs_data_set_default_double(settings, S_MOUTH_W, 16.0);
	obs_data_set_default_double(settings, S_MOUTH_H, 9.0);
	obs_data_set_default_double(settings, S_MOUTH_TRAVEL, 80.0);
	obs_data_set_default_double(settings, S_SENSITIVITY, 50.0);
	obs_data_set_default_double(settings, S_TEETH, 70.0);
	obs_data_set_default_double(settings, S_TONGUE, 60.0);
	/* On by default: the first thing anyone has to do with this mode is put
	 * the region on the mouth, and that is far easier when it is visible. */
	obs_data_set_default_bool(settings, S_SHOW_MOUTH, true);
	/* On by default while the log is still the clearest evidence that the
	 * tap and the engine are working. */
	obs_data_set_default_bool(settings, S_LOG_LEVEL, true);
}

static bool avatar_add_audio_source(void *data, obs_source_t *source)
{
	obs_property_t *list = data;

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(list, name, name);
	return true;
}

/* Each mode's controls follow that mode.  A visible control that does nothing
 * is the same lie as shipping one that is not wired up: the frame rate is
 * negotiated with a worker and means nothing to the in-process mouth, and the
 * mouth region means nothing to a worker that does its own face detection. */
static bool avatar_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const char *mode = obs_data_get_string(settings, S_MODE);
	const bool sidecar = mode && strcmp(mode, S_MODE_SIDECAR) == 0;
	const bool viseme = mode && strcmp(mode, S_MODE_VISEME) == 0;

	obs_property_set_visible(obs_properties_get(props, S_FPS), sidecar);

	static const char *const worker_props[] = {S_AUTO_WORKER,    S_MOUTH_GAIN,    S_MUSETALK_DIR,
						   S_WORKER_PYTHON, S_WORKER_SCRIPT};
	for (size_t i = 0; i < sizeof(worker_props) / sizeof(worker_props[0]); i++)
		obs_property_set_visible(obs_properties_get(props, worker_props[i]), sidecar);

	static const char *const mouth_props[] = {"mouth_hint", S_MOUTH_X,      S_MOUTH_Y,     S_MOUTH_W,
						  S_MOUTH_H,    S_MOUTH_TRAVEL, S_SENSITIVITY, S_TEETH,
						  S_TONGUE,     S_SHOW_MOUTH};
	for (size_t i = 0; i < sizeof(mouth_props) / sizeof(mouth_props[0]); i++)
		obs_property_set_visible(obs_properties_get(props, mouth_props[i]), viseme);

	return true;
}

static obs_properties_t *avatar_source_properties(void *data)
{
	struct avatar_source *ctx = data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "stage_info", TEXT_STAGE_INFO, OBS_TEXT_INFO);

	obs_properties_add_path(props, S_IMAGE, TEXT_IMAGE, OBS_PATH_FILE, TEXT_IMAGE_FILTER, NULL);

	obs_property_t *sources = obs_properties_add_list(props, S_AUDIO_SOURCE, TEXT_AUDIO_SOURCE,
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources, TEXT_AUDIO_SOURCE_NONE, "none");
	obs_enum_sources(avatar_add_audio_source, sources);

	obs_property_t *mode = obs_properties_add_list(props, S_MODE, TEXT_MODE, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, TEXT_MODE_OFF, S_MODE_OFF);
	obs_property_list_add_string(mode, TEXT_MODE_VISEME, S_MODE_VISEME);
	obs_property_list_add_string(mode, TEXT_MODE_SIDECAR, S_MODE_SIDECAR);
	obs_property_set_modified_callback(mode, avatar_mode_modified);

	obs_property_t *fps = obs_properties_add_list(props, S_FPS, TEXT_FPS, OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "15", 15);
	obs_property_list_add_int(fps, "20", 20);
	obs_property_list_add_int(fps, "25", 25);
	obs_property_list_add_int(fps, "30", 30);

	/* Mode A's worker.  Off means "I start it myself", the way to watch its
	 * console; the pipe to point it at is shown further down.  An empty
	 * interpreter or script is not an error - each has a default, resolved
	 * when the worker is launched. */
	obs_properties_add_bool(props, S_AUTO_WORKER, TEXT_AUTO_WORKER);
	obs_properties_add_float_slider(props, S_MOUTH_GAIN, TEXT_MOUTH_GAIN, 50.0, 300.0, 5.0);
	obs_properties_add_path(props, S_MUSETALK_DIR, TEXT_MUSETALK_DIR, OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_path(props, S_WORKER_PYTHON, TEXT_WORKER_PYTHON, OBS_PATH_FILE, "python.exe (*.exe)", NULL);
	obs_properties_add_path(props, S_WORKER_SCRIPT, TEXT_WORKER_SCRIPT, OBS_PATH_FILE, "Python (*.py)", NULL);

	/* Mode B's mouth.  There is no detecting where an animal's mouth is -
	 * that is exactly what MuseTalk's face parsing cannot do either - so
	 * the user points at it, in percentages of the artwork. */
	obs_properties_add_text(props, "mouth_hint", TEXT_MOUTH_HINT, OBS_TEXT_INFO);
	obs_properties_add_float_slider(props, S_MOUTH_X, TEXT_MOUTH_X, 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, S_MOUTH_Y, TEXT_MOUTH_Y, 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, S_MOUTH_W, TEXT_MOUTH_W, 1.0, 60.0, 0.5);
	obs_properties_add_float_slider(props, S_MOUTH_H, TEXT_MOUTH_H, 1.0, 40.0, 0.5);
	obs_properties_add_float_slider(props, S_MOUTH_TRAVEL, TEXT_MOUTH_TRAVEL, 0.0, 200.0, 5.0);
	obs_properties_add_float_slider(props, S_SENSITIVITY, TEXT_SENSITIVITY, 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, S_TEETH, TEXT_TEETH, 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, S_TONGUE, TEXT_TONGUE, 0.0, 100.0, 1.0);
	obs_properties_add_bool(props, S_SHOW_MOUTH, TEXT_SHOW_MOUTH);

	obs_properties_add_bool(props, S_LOG_LEVEL, TEXT_LOG_LEVEL);

	/* The pipe a hand-started worker has to be pointed at.  Only the
	 * sidecar engine has one, and only once it exists - reopening the
	 * dialog after switching the mode on is what fills this in. */
	if (ctx) {
		pthread_mutex_lock(&ctx->engine_mutex);
		const char *pipe = (ctx->ops == &avatar_engine_sidecar_ops && ctx->engine)
					   ? avatar_engine_sidecar_pipe_name(ctx->engine)
					   : NULL;
		struct dstr label = {0};
		dstr_printf(&label, "%s%s", TEXT_PIPE, pipe ? pipe : TEXT_PIPE_IDLE);
		pthread_mutex_unlock(&ctx->engine_mutex);

		obs_properties_add_text(props, "pipe_name", label.array, OBS_TEXT_INFO);
		dstr_free(&label);
	}

	/* Where weights and avatar bundles go.  The path has to show even
	 * before the directory exists, or there is nowhere to start. */
	char *root = avatar_asset_root();
	if (root) {
		struct dstr label = {0};
		dstr_printf(&label, "%s%s", TEXT_ASSET_DIR, root);
		obs_properties_add_text(props, "asset_dir", label.array, OBS_TEXT_INFO);
		dstr_free(&label);
		bfree(root);
	}

	return props;
}

struct obs_source_info avatar_source_info = {
	.id = "ppobs_avatar_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* CUSTOM_DRAW because the placeholder, the still asset and a generated
	 * frame use different effects and blend modes: outside custom-draw mode
	 * libobs has already opened a technique pass, and a second one cannot
	 * be started inside it. */
	/* INTERACTION is what puts "Interact" on the source's context menu and
	 * delivers mouse events in source coordinates, which is how the mouth
	 * region gets dragged onto the mouth instead of hunted for with
	 * sliders. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_INTERACTION,
	.get_name = avatar_source_get_name,
	.create = avatar_source_create,
	.destroy = avatar_source_destroy,
	.update = avatar_source_update,
	.get_defaults = avatar_source_defaults,
	.get_properties = avatar_source_properties,
	.get_width = avatar_source_get_width,
	.get_height = avatar_source_get_height,
	.video_render = avatar_source_render,
	.video_tick = avatar_source_tick,
	.mouse_click = avatar_mouse_click,
	.mouse_move = avatar_mouse_move,
	.mouse_wheel = avatar_mouse_wheel,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};
