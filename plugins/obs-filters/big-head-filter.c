/*
 * Big-head / head-circle filter.
 *
 * Masks a video source to a feathered circle around the streamer's head and
 * follows the head as it moves.  It is a synchronous GPU filter, so it attaches
 * to camera, media, image and window-capture sources alike; the source is
 * rendered into a small readback once every detection interval, the face is
 * located with the light-weight skin tracker in face-track.c, and the circle is
 * drawn in the pixel shader with two uniforms.
 *
 * Detection runs on the graphics thread after the staging map.  Unlike the
 * portrait matting filter there is no worker thread: the tracker is a coarse
 * connected-component scan over a 64x64 grid, far cheaper than the readback it
 * follows, and keeping it synchronous means the circle never trails the frame
 * by more than the configured detection interval.
 *
 * When no face has been found yet, or automatic tracking is disabled, the
 * filter uses the manual centre/radius so the effect is still useful as a
 * plain circular crop.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "face-track.h"

#define S_AUTO_TRACK "auto_track"
#define S_DETECT_FPS "detect_fps"
#define S_RADIUS_SCALE "radius_scale"
#define S_OFFSET_X "offset_x"
#define S_OFFSET_Y "offset_y"
#define S_FEATHER "feather"
#define S_MANUAL_CENTER_X "manual_center_x"
#define S_MANUAL_CENTER_Y "manual_center_y"
#define S_MANUAL_RADIUS "manual_radius"
#define S_SHOW_MASK "show_mask"

#define TEXT_INFO obs_module_text("BigHeadFilter.Info")
#define TEXT_AUTO_TRACK obs_module_text("BigHeadFilter.AutoTrack")
#define TEXT_DETECT_FPS obs_module_text("BigHeadFilter.DetectFps")
#define TEXT_RADIUS_SCALE obs_module_text("BigHeadFilter.RadiusScale")
#define TEXT_OFFSET_X obs_module_text("BigHeadFilter.OffsetX")
#define TEXT_OFFSET_Y obs_module_text("BigHeadFilter.OffsetY")
#define TEXT_FEATHER obs_module_text("BigHeadFilter.Feather")
#define TEXT_MANUAL_CENTER_X obs_module_text("BigHeadFilter.ManualCenterX")
#define TEXT_MANUAL_CENTER_Y obs_module_text("BigHeadFilter.ManualCenterY")
#define TEXT_MANUAL_RADIUS obs_module_text("BigHeadFilter.ManualRadius")
#define TEXT_SHOW_MASK obs_module_text("BigHeadFilter.ShowMask")

/* Longest side of the detection readback.  The tracker only needs a face box,
 * so this is deliberately much smaller than the video. */
#define BIG_HEAD_CAPTURE_MAX 256
#define BIG_HEAD_CAPTURE_MIN 64

/* A skin blob usually runs down the neck, which would put the circle too low.
 * The beauty filter caps the usable face height at 1.45x its width for the same
 * reason; the head circle uses the top of that capped box. */
#define BIG_HEAD_FACE_HEIGHT_RATIO 1.45f

/* Head geometry relative to the face box.  The radius is a fraction of the face
 * width; the centre sits a little above the middle of the capped face box so
 * hair is included above the forehead. */
#define BIG_HEAD_RADIUS_PER_FACE_WIDTH 0.72f
#define BIG_HEAD_CENTER_Y_RATIO 0.42f

struct big_head_data {
	obs_source_t *context;
	gs_effect_t *effect;

	/* Graphics-thread readback resources. */
	gs_texrender_t *capture_render;
	gs_stagesurf_t *capture_stage;
	uint32_t capture_width;
	uint32_t capture_height;
	uint64_t last_detect_time;

	struct face_track track;

	/* UI thread writes these under the mutex; the graphics thread copies
	 * them once per render. */
	pthread_mutex_t mutex;
	bool auto_track;
	int detect_fps;
	double radius_scale;
	double offset_x;
	double offset_y;
	double feather;
	double manual_center_x;
	double manual_center_y;
	double manual_radius;
	bool show_mask;
};

static const char *big_head_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BigHeadFilter");
}

static void big_head_update(void *data, obs_data_t *settings)
{
	struct big_head_data *filter = data;

	pthread_mutex_lock(&filter->mutex);
	filter->auto_track = obs_data_get_bool(settings, S_AUTO_TRACK);
	filter->detect_fps = (int)obs_data_get_int(settings, S_DETECT_FPS);
	filter->radius_scale = obs_data_get_double(settings, S_RADIUS_SCALE);
	filter->offset_x = obs_data_get_double(settings, S_OFFSET_X);
	filter->offset_y = obs_data_get_double(settings, S_OFFSET_Y);
	filter->feather = obs_data_get_double(settings, S_FEATHER);
	filter->manual_center_x = obs_data_get_double(settings, S_MANUAL_CENTER_X);
	filter->manual_center_y = obs_data_get_double(settings, S_MANUAL_CENTER_Y);
	filter->manual_radius = obs_data_get_double(settings, S_MANUAL_RADIUS);
	filter->show_mask = obs_data_get_bool(settings, S_SHOW_MASK);
	pthread_mutex_unlock(&filter->mutex);
}

static void *big_head_create(obs_data_t *settings, obs_source_t *context)
{
	struct big_head_data *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	pthread_mutex_init(&filter->mutex, NULL);
	face_track_init(&filter->track);

	char *effect_path = obs_module_file("big_head.effect");
	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);

	if (!filter->effect) {
		blog(LOG_ERROR, "[big head] could not load big_head.effect");
		pthread_mutex_destroy(&filter->mutex);
		bfree(filter);
		return NULL;
	}

	big_head_update(filter, settings);
	return filter;
}

static void big_head_destroy(void *data)
{
	struct big_head_data *filter = data;

	if (!filter)
		return;

	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	if (filter->capture_render)
		gs_texrender_destroy(filter->capture_render);
	if (filter->capture_stage)
		gs_stagesurface_destroy(filter->capture_stage);
	obs_leave_graphics();

	face_track_free(&filter->track);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

static void big_head_capture(struct big_head_data *filter, obs_source_t *target, obs_source_t *parent, int detect_fps)
{
	const uint32_t source_width = obs_source_get_base_width(target);
	const uint32_t source_height = obs_source_get_base_height(target);

	if (!source_width || !source_height)
		return;

	const uint64_t now = obs_get_video_frame_time();
	const uint64_t interval = detect_fps > 0 ? 1000000000ULL / (uint64_t)detect_fps : 0;

	if (interval && filter->last_detect_time && now > filter->last_detect_time &&
	    now - filter->last_detect_time < interval)
		return;

	uint32_t capture_width;
	uint32_t capture_height;

	if (source_width >= source_height) {
		capture_width = BIG_HEAD_CAPTURE_MAX;
		capture_height = (uint32_t)lround((double)BIG_HEAD_CAPTURE_MAX * source_height / source_width);
		if (capture_height < BIG_HEAD_CAPTURE_MIN)
			capture_height = BIG_HEAD_CAPTURE_MIN;
	} else {
		capture_height = BIG_HEAD_CAPTURE_MAX;
		capture_width = (uint32_t)lround((double)BIG_HEAD_CAPTURE_MAX * source_width / source_height);
		if (capture_width < BIG_HEAD_CAPTURE_MIN)
			capture_width = BIG_HEAD_CAPTURE_MIN;
	}

	if (capture_width != filter->capture_width || capture_height != filter->capture_height) {
		if (filter->capture_stage) {
			gs_stagesurface_destroy(filter->capture_stage);
			filter->capture_stage = NULL;
		}
		filter->capture_stage = gs_stagesurface_create(capture_width, capture_height, GS_RGBA);
		filter->capture_width = capture_width;
		filter->capture_height = capture_height;
	}

	if (!filter->capture_render)
		filter->capture_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (!filter->capture_render || !filter->capture_stage)
		return;

	gs_texrender_reset(filter->capture_render);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->capture_render, capture_width, capture_height)) {
		const uint32_t parent_flags = obs_source_get_output_flags(parent);
		const bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		const bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)source_width, 0.0f, (float)source_height, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(filter->capture_render);

		gs_texture_t *texture = gs_texrender_get_texture(filter->capture_render);
		if (texture) {
			gs_stage_texture(filter->capture_stage, texture);

			uint8_t *mapped = NULL;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(filter->capture_stage, &mapped, &linesize) && mapped) {
				face_track_update_rgba(&filter->track, mapped, linesize, capture_width, capture_height);
				gs_stagesurface_unmap(filter->capture_stage);
				filter->last_detect_time = now;
			} else {
				gs_stagesurface_unmap(filter->capture_stage);
			}
		}
	}

	gs_blend_state_pop();
}

static void big_head_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct big_head_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);

	if (!filter->effect || !target || !parent) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space =
		obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	if (source_space == GS_CS_709_EXTENDED) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	bool auto_track;
	int detect_fps;
	double radius_scale;
	double offset_x;
	double offset_y;
	double feather;
	double manual_center_x;
	double manual_center_y;
	double manual_radius;
	bool show_mask;

	pthread_mutex_lock(&filter->mutex);
	auto_track = filter->auto_track;
	detect_fps = filter->detect_fps;
	radius_scale = filter->radius_scale;
	offset_x = filter->offset_x;
	offset_y = filter->offset_y;
	feather = filter->feather;
	manual_center_x = filter->manual_center_x;
	manual_center_y = filter->manual_center_y;
	manual_radius = filter->manual_radius;
	show_mask = filter->show_mask;
	pthread_mutex_unlock(&filter->mutex);

	if (auto_track)
		big_head_capture(filter, target, parent, detect_fps);

	const float source_width = (float)obs_source_get_base_width(target);
	const float source_height = (float)obs_source_get_base_height(target);

	if (source_width <= 0.0f || source_height <= 0.0f) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const float aspect = source_width / source_height;
	struct vec2 center;
	float radius;

	if (auto_track && filter->track.width > 0.0f) {
		const float face_width = filter->track.width * source_width;
		const float face_height =
			fminf(filter->track.height * source_height, BIG_HEAD_FACE_HEIGHT_RATIO * face_width);
		const float face_top = filter->track.y * source_height;
		const float center_x =
			(filter->track.x + filter->track.width * 0.5f) * source_width + (float)offset_x * source_width;
		const float center_y =
			face_top + BIG_HEAD_CENTER_Y_RATIO * face_height + (float)offset_y * source_height;
		const float head_radius = BIG_HEAD_RADIUS_PER_FACE_WIDTH * face_width * (float)radius_scale;

		center.x = center_x / source_width;
		center.y = center_y / source_height;
		radius = head_radius / source_height;
	} else {
		center.x = (float)manual_center_x;
		center.y = (float)manual_center_y;
		radius = (float)manual_radius;
	}

	if (radius < 0.01f)
		radius = 0.01f;

	const enum gs_color_format format = gs_get_format_from_space(source_space);
	if (!obs_source_process_filter_begin_with_color_space(filter->context, format, source_space,
							      OBS_ALLOW_DIRECT_RENDERING))
		return;

	gs_effect_set_vec2(gs_effect_get_param_by_name(filter->effect, "circle_center"), &center);
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "circle_radius"), radius);
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "aspect"), aspect);
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "feather"), (float)feather);
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "show_mask"), show_mask ? 1.0f : 0.0f);

	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
}

static enum gs_color_space big_head_get_color_space(void *data, size_t count,
						    const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct big_head_data *filter = data;

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	return obs_source_get_color_space(obs_filter_get_target(filter->context), OBS_COUNTOF(potential_spaces),
					  potential_spaces);
}

static obs_properties_t *big_head_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "info", TEXT_INFO, OBS_TEXT_INFO);
	obs_properties_add_bool(props, S_AUTO_TRACK, TEXT_AUTO_TRACK);

	obs_property_t *fps = obs_properties_add_list(props, S_DETECT_FPS, TEXT_DETECT_FPS, OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "10", 10);
	obs_property_list_add_int(fps, "15", 15);
	obs_property_list_add_int(fps, "20", 20);
	obs_property_list_add_int(fps, "30", 30);

	obs_properties_add_float_slider(props, S_RADIUS_SCALE, TEXT_RADIUS_SCALE, 0.4, 1.6, 0.01);
	obs_properties_add_float_slider(props, S_OFFSET_X, TEXT_OFFSET_X, -0.5, 0.5, 0.01);
	obs_properties_add_float_slider(props, S_OFFSET_Y, TEXT_OFFSET_Y, -0.5, 0.5, 0.01);
	obs_properties_add_float_slider(props, S_FEATHER, TEXT_FEATHER, 0.0, 0.2, 0.005);

	obs_properties_add_float_slider(props, S_MANUAL_CENTER_X, TEXT_MANUAL_CENTER_X, 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_MANUAL_CENTER_Y, TEXT_MANUAL_CENTER_Y, 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_MANUAL_RADIUS, TEXT_MANUAL_RADIUS, 0.02, 0.75, 0.01);
	obs_properties_add_bool(props, S_SHOW_MASK, TEXT_SHOW_MASK);

	return props;
}

static void big_head_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_AUTO_TRACK, true);
	obs_data_set_default_int(settings, S_DETECT_FPS, 20);
	obs_data_set_default_double(settings, S_RADIUS_SCALE, 1.0);
	obs_data_set_default_double(settings, S_OFFSET_X, 0.0);
	obs_data_set_default_double(settings, S_OFFSET_Y, 0.0);
	obs_data_set_default_double(settings, S_FEATHER, 0.02);
	obs_data_set_default_double(settings, S_MANUAL_CENTER_X, 0.5);
	obs_data_set_default_double(settings, S_MANUAL_CENTER_Y, 0.35);
	obs_data_set_default_double(settings, S_MANUAL_RADIUS, 0.25);
	obs_data_set_default_bool(settings, S_SHOW_MASK, false);
}

struct obs_source_info big_head_filter = {
	.id = "big_head_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = big_head_name,
	.create = big_head_create,
	.destroy = big_head_destroy,
	.update = big_head_update,
	.get_properties = big_head_properties,
	.get_defaults = big_head_defaults,
	.video_render = big_head_render,
	.video_get_color_space = big_head_get_color_space,
};
