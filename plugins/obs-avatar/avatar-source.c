#include "avatar-source.h"
#include "avatar-assets.h"
#include "avatar-audio-tap.h"

#include <graphics/image-file.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <sys/stat.h>
#include <string.h>

#define S_IMAGE "avatar_image"
#define S_AUDIO_SOURCE "audio_source"
#define S_LOG_LEVEL "log_audio_level"

#define MT_ obs_module_text
#define TEXT_SOURCE_NAME MT_("Avatar.Source")
#define TEXT_IMAGE MT_("Avatar.Image")
#define TEXT_IMAGE_FILTER MT_("Avatar.Image.Filter")
#define TEXT_AUDIO_SOURCE MT_("Avatar.AudioSource")
#define TEXT_AUDIO_SOURCE_NONE MT_("Avatar.AudioSource.None")
#define TEXT_LOG_LEVEL MT_("Avatar.LogAudioLevel")
#define TEXT_STAGE_INFO MT_("Avatar.StageInfo")
#define TEXT_ASSET_DIR MT_("Avatar.AssetDir")

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

/* Samples taken from the ring each tick.  Stage A discards them: leaving the
 * buffer permanently full would keep the drop counter non-zero and hide
 * whether a real backlog is forming. */
#define AVATAR_DRAIN_CHUNK 4096

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
};

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

static void avatar_source_update(void *data, obs_data_t *settings)
{
	struct avatar_source *ctx = data;

	const char *path = obs_data_get_string(settings, S_IMAGE);
	if (!ctx->image_path || strcmp(ctx->image_path, path) != 0) {
		bfree(ctx->image_path);
		ctx->image_path = bstrdup(path);
		avatar_load_image(ctx);
	}

	ctx->log_level = obs_data_get_bool(settings, S_LOG_LEVEL);

	avatar_audio_tap_set_source(ctx->tap, obs_data_get_string(settings, S_AUDIO_SOURCE));
}

static void *avatar_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct avatar_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->drain = bmalloc(AVATAR_DRAIN_CHUNK * sizeof(float));
	avatar_reset_pending(ctx);

	ctx->tap = avatar_audio_tap_create(source);
	if (!ctx->tap) {
		blog(LOG_ERROR, "[obs-avatar] failed to create the audio tap");
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

	avatar_audio_tap_destroy(ctx->tap);
	avatar_unload_image(ctx);

	bfree(ctx->drain);
	bfree(ctx->image_path);
	bfree(ctx);
}

static uint32_t avatar_source_get_width(void *data)
{
	struct avatar_source *ctx = data;
	const uint32_t cx = ctx->image_loaded ? ctx->if4.image3.image2.image.cx : 0;
	return cx ? cx : AVATAR_PLACEHOLDER_SIZE;
}

static uint32_t avatar_source_get_height(void *data)
{
	struct avatar_source *ctx = data;
	const uint32_t cy = ctx->image_loaded ? ctx->if4.image3.image2.image.cy : 0;
	return cy ? cy : AVATAR_PLACEHOLDER_SIZE;
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

static void avatar_source_render(void *data, gs_effect_t *unused)
{
	struct avatar_source *ctx = data;

	UNUSED_PARAMETER(unused);

	struct gs_image_file *const image = ctx->image_loaded ? &ctx->if4.image3.image2.image : NULL;
	gs_texture_t *const texture = image ? image->texture : NULL;

	if (!texture) {
		avatar_draw_placeholder();
		return;
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	/* The asset is premultiplied, hence ONE rather than SRCALPHA. */
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, image->cx, image->cy);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

/* Empties the ring.  Stage A throws the audio away; from stage B on this is
 * where the engine is fed. */
static size_t avatar_drain_audio(struct avatar_source *ctx)
{
	size_t total = 0;
	for (;;) {
		const size_t read = avatar_audio_tap_read(ctx->tap, ctx->drain, AVATAR_DRAIN_CHUNK);
		total += read;
		if (read < AVATAR_DRAIN_CHUNK)
			break;
	}
	return total;
}

/* Stage A's acceptance test: the logged level tracks speech. */
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
			blog(LOG_INFO,
			     "[obs-avatar: '%s'] tap %s: peak %.1f dBFS, rms %.1f dBFS, "
			     "%llu samples at 16 kHz, %llu dropped",
			     obs_source_get_name(ctx->source), speaking ? "speaking" : "silent",
			     ctx->pending_peak_db, ctx->pending_rms_db, (unsigned long long)ctx->pending_samples,
			     (unsigned long long)ctx->pending_dropped);
		}
	}

	avatar_reset_pending(ctx);
}

static void avatar_source_tick(void *data, float seconds)
{
	struct avatar_source *ctx = data;

	avatar_audio_tap_tick(ctx->tap);
	avatar_report_audio(ctx, avatar_drain_audio(ctx));

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

	/* Animated assets advance here, which is all stage A's idle animation
	 * needs - an animated GIF/WebP avatar blinks with no extra code. */
	const uint64_t frame_time = obs_get_video_frame_time();
	if (ctx->last_frame_time && gs_image_file4_tick(&ctx->if4, frame_time - ctx->last_frame_time)) {
		obs_enter_graphics();
		gs_image_file4_update_texture(&ctx->if4);
		obs_leave_graphics();
	}
	ctx->last_frame_time = frame_time;
}

static void avatar_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_AUDIO_SOURCE, "none");
	/* On by default in stage A: the log is the only observable evidence
	 * that the tap works.  Once the mouth moves on screen this becomes off
	 * by default. */
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

static obs_properties_t *avatar_source_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "stage_info", TEXT_STAGE_INFO, OBS_TEXT_INFO);

	obs_properties_add_path(props, S_IMAGE, TEXT_IMAGE, OBS_PATH_FILE, TEXT_IMAGE_FILTER, NULL);

	obs_property_t *sources = obs_properties_add_list(props, S_AUDIO_SOURCE, TEXT_AUDIO_SOURCE,
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources, TEXT_AUDIO_SOURCE_NONE, "none");
	obs_enum_sources(avatar_add_audio_source, sources);

	obs_properties_add_bool(props, S_LOG_LEVEL, TEXT_LOG_LEVEL);

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
	/* CUSTOM_DRAW because the placeholder and the avatar texture use
	 * different effects: outside custom-draw mode libobs has already opened
	 * a technique pass, and a second one cannot be started inside it. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
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
	.icon_type = OBS_ICON_TYPE_IMAGE,
};
