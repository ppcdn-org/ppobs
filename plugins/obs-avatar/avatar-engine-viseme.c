#include "avatar-engine-viseme.h"
#include "avatar-audio-tap.h"

#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

struct viseme {
	obs_source_t *owner;
	uint32_t sample_rate;

	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_center;
	gs_eparam_t *param_radius;
	gs_eparam_t *param_openness;
	gs_eparam_t *param_travel;
	gs_eparam_t *param_interior;
	gs_eparam_t *param_teeth;
	gs_eparam_t *param_tongue;
	gs_eparam_t *param_show_region;
	bool effect_failed;

	/* Written by the video thread's push_audio, read by the graphics
	 * thread's render. A float torn between the two would at worst show one
	 * frame of a wrong mouth, but the mouth parameters are a struct, so
	 * those get the lock. */
	pthread_mutex_t mutex;
	struct avatar_viseme_mouth mouth;
	float openness;

	/* Samples left over from the last push that did not fill a window. */
	float window[AVATAR_TAP_SAMPLE_RATE / 50];
	size_t window_fill;
	size_t window_size;

	uint64_t frames;
};

static void viseme_destroy(void *data);

static void *viseme_create(const struct avatar_engine_config *cfg, obs_source_t *owner)
{
	struct viseme *v = bzalloc(sizeof(*v));
	v->owner = owner;
	v->sample_rate = cfg->sample_rate ? cfg->sample_rate : AVATAR_TAP_SAMPLE_RATE;

	if (pthread_mutex_init(&v->mutex, NULL) != 0) {
		bfree(v);
		return NULL;
	}

	v->window_size = (size_t)v->sample_rate * AVATAR_VISEME_WINDOW_MS / 1000;
	if (v->window_size > sizeof(v->window) / sizeof(v->window[0]))
		v->window_size = sizeof(v->window) / sizeof(v->window[0]);
	if (!v->window_size)
		v->window_size = 1;

	/* Sensible defaults so the mouth is somewhere plausible before anyone
	 * touches a slider: lower-middle of the face, which is where it is on
	 * most head-and-shoulders framing. */
	v->mouth.center_x = 0.5f;
	v->mouth.center_y = 0.68f;
	v->mouth.radius_x = 0.16f;
	v->mouth.radius_y = 0.09f;
	v->mouth.travel = 0.8f;
	v->mouth.gate_db = AVATAR_TAP_SILENCE_DB;
	v->mouth.full_db = -12.0f;
	v->mouth.teeth = 0.7f;
	v->mouth.tongue = 0.6f;

	return v;
}

static void viseme_destroy(void *data)
{
	struct viseme *v = data;
	if (!v)
		return;

	if (v->effect) {
		obs_enter_graphics();
		gs_effect_destroy(v->effect);
		obs_leave_graphics();
	}

	pthread_mutex_destroy(&v->mutex);
	bfree(v);
}

static bool viseme_ready(void *data)
{
	/* Nothing to wait for: no model, no worker, no GPU requirement beyond
	 * the one OBS already has. That is the whole point of this mode. */
	UNUSED_PARAMETER(data);
	return true;
}

/* Folds one window of audio into the current openness. */
static void viseme_consume_window(struct viseme *v)
{
	const float peak = avatar_block_peak(v->window, v->window_fill);
	const float peak_db = avatar_amplitude_to_db(peak);
	const float seconds = (float)v->window_fill / (float)v->sample_rate;

	pthread_mutex_lock(&v->mutex);
	const float target = avatar_viseme_target(peak_db, v->mouth.gate_db, v->mouth.full_db);
	v->openness = avatar_viseme_smooth(v->openness, target, seconds);
	pthread_mutex_unlock(&v->mutex);

	v->window_fill = 0;
}

static void viseme_push_audio(void *data, const float *samples, size_t count, int64_t pts_ns)
{
	struct viseme *v = data;

	UNUSED_PARAMETER(pts_ns);

	/* Walked in 20 ms windows rather than taken as one block: the level of
	 * a whole tick averaged together would flatten the syllables that make
	 * the mouth look like it is speaking. */
	while (count) {
		const size_t room = v->window_size - v->window_fill;
		const size_t take = (count < room) ? count : room;

		memcpy(v->window + v->window_fill, samples, take * sizeof(float));
		v->window_fill += take;
		samples += take;
		count -= take;

		if (v->window_fill == v->window_size)
			viseme_consume_window(v);
	}
}

/* Loaded on first use, on the graphics thread, so creation does not need the
 * context and a failure can be reported once rather than per frame. */
static bool viseme_load_effect(struct viseme *v)
{
	if (v->effect)
		return true;
	if (v->effect_failed)
		return false;

	char *path = obs_module_file("avatar-mouth.effect");
	if (!path) {
		v->effect_failed = true;
		blog(LOG_ERROR, "[obs-avatar viseme] avatar-mouth.effect is missing from the plugin's data");
		return false;
	}

	char *error = NULL;
	v->effect = gs_effect_create_from_file(path, &error);
	bfree(path);

	if (!v->effect) {
		v->effect_failed = true;
		blog(LOG_ERROR, "[obs-avatar viseme] could not compile avatar-mouth.effect: %s",
		     error ? error : "(no detail)");
		bfree(error);
		return false;
	}
	bfree(error);

	v->param_image = gs_effect_get_param_by_name(v->effect, "image");
	v->param_center = gs_effect_get_param_by_name(v->effect, "mouth_center");
	v->param_radius = gs_effect_get_param_by_name(v->effect, "mouth_radius");
	v->param_openness = gs_effect_get_param_by_name(v->effect, "openness");
	v->param_travel = gs_effect_get_param_by_name(v->effect, "travel");
	v->param_interior = gs_effect_get_param_by_name(v->effect, "interior");
	v->param_teeth = gs_effect_get_param_by_name(v->effect, "teeth");
	v->param_tongue = gs_effect_get_param_by_name(v->effect, "tongue");
	v->param_show_region = gs_effect_get_param_by_name(v->effect, "show_region");

	return true;
}

static bool viseme_render(void *data, gs_texture_t *base, uint32_t cx, uint32_t cy)
{
	struct viseme *v = data;

	if (!base || !viseme_load_effect(v))
		return false;

	pthread_mutex_lock(&v->mutex);
	const struct avatar_viseme_mouth mouth = v->mouth;
	const float openness = v->openness;
	pthread_mutex_unlock(&v->mutex);

	struct vec2 center;
	struct vec2 radius;
	vec2_set(&center, mouth.center_x, mouth.center_y);
	vec2_set(&radius, mouth.radius_x, mouth.radius_y);

	/* The cavity's base colour. Brighter than a photographic mouth cavity
	 * on purpose: the framebuffer is in linear space and the avatar is
	 * usually small on screen, so a physically dark value disappears. */
	struct vec3 interior;
	vec3_set(&interior, 0.16f, 0.05f, 0.07f);

	gs_effect_set_texture_srgb(v->param_image, base);
	gs_effect_set_vec2(v->param_center, &center);
	gs_effect_set_vec2(v->param_radius, &radius);
	gs_effect_set_float(v->param_openness, openness);
	gs_effect_set_float(v->param_travel, mouth.travel);
	gs_effect_set_vec3(v->param_interior, &interior);
	gs_effect_set_float(v->param_teeth, mouth.teeth);
	gs_effect_set_float(v->param_tongue, mouth.tongue);
	gs_effect_set_float(v->param_show_region, mouth.show_region ? 1.0f : 0.0f);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	/* The artwork is premultiplied and so is the shader's output. */
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(v->effect, "Draw"))
		gs_draw_sprite(base, 0, cx, cy);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	v->frames++;
	return true;
}

static void viseme_get_stats(void *data, struct avatar_engine_stats *out)
{
	struct viseme *v = data;

	memset(out, 0, sizeof(*out));
	out->connected = true;
	out->real_model = false;
	out->frames = v->frames;
	strncpy(out->engine, "viseme (mouth deformation)", sizeof(out->engine) - 1);
}

void avatar_engine_viseme_set_mouth(void *data, const struct avatar_viseme_mouth *mouth)
{
	struct viseme *v = data;
	if (!v || !mouth)
		return;

	pthread_mutex_lock(&v->mutex);
	v->mouth = *mouth;
	pthread_mutex_unlock(&v->mutex);
}

void avatar_engine_viseme_get_mouth(void *data, struct avatar_viseme_mouth *out)
{
	struct viseme *v = data;
	if (!v || !out)
		return;

	pthread_mutex_lock(&v->mutex);
	*out = v->mouth;
	pthread_mutex_unlock(&v->mutex);
}

float avatar_engine_viseme_openness(void *data)
{
	struct viseme *v = data;
	if (!v)
		return 0.0f;

	pthread_mutex_lock(&v->mutex);
	const float openness = v->openness;
	pthread_mutex_unlock(&v->mutex);
	return openness;
}

const struct avatar_engine_ops avatar_engine_viseme_ops = {
	.id = "viseme",
	.create = viseme_create,
	.destroy = viseme_destroy,
	.ready = viseme_ready,
	.push_audio = viseme_push_audio,
	/* No lock_frame/unlock_frame: this engine composites the source's own
	 * artwork rather than producing pixels of its own. */
	.render = viseme_render,
	.get_stats = viseme_get_stats,
};
