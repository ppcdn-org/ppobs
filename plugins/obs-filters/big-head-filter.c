/*
 * Big-head / head-circle filter with reference-face tracking.
 *
 * The source is rendered into a small detection canvas at a configurable rate
 * and handed to a worker thread.  YuNet finds every face in the canvas, SFace
 * embeds them, and the face whose embedding matches the enrolled reference is
 * selected.  Without a reference the largest face (or the one nearest the
 * manual circle) is used.
 *
 * The circle itself is drawn by the GPU shader; only the centre and radius are
 * updated per frame, so the compositing cost is negligible.  Detection and
 * recognition are the only meaningful CPU work and run at 10-20 Hz, not at the
 * video frame rate.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>

#include <float.h>
#include <math.h>
#include <string.h>

#include "face-detect.h"
#include "face-ort.h"

#define S_AUTO_TRACK "auto_track"
#define S_DETECT_FPS "detect_fps"
#define S_RADIUS_SCALE "radius_scale"
#define S_FIXED_SIZE "fixed_size"
#define S_FIXED_RADIUS "fixed_radius"
#define S_OFFSET_X "offset_x"
#define S_OFFSET_Y "offset_y"
#define S_FEATHER "feather"
#define S_BORDER_THICKNESS "border_thickness"
#define S_BORDER_COLOR "border_color"
#define S_MANUAL_CENTER_X "manual_center_x"
#define S_MANUAL_CENTER_Y "manual_center_y"
#define S_MANUAL_RADIUS "manual_radius"
#define S_SIMILARITY "similarity"
#define S_REFERENCE "reference"
#define S_SHOW_MASK "show_mask"

#define TEXT_INFO obs_module_text("BigHeadFilter.Info")
#define TEXT_AUTO_TRACK obs_module_text("BigHeadFilter.AutoTrack")
#define TEXT_DETECT_FPS obs_module_text("BigHeadFilter.DetectFps")
#define TEXT_RADIUS_SCALE obs_module_text("BigHeadFilter.RadiusScale")
#define TEXT_FIXED_SIZE obs_module_text("BigHeadFilter.FixedSize")
#define TEXT_FIXED_RADIUS obs_module_text("BigHeadFilter.FixedRadius")
#define TEXT_OFFSET_X obs_module_text("BigHeadFilter.OffsetX")
#define TEXT_OFFSET_Y obs_module_text("BigHeadFilter.OffsetY")
#define TEXT_FEATHER obs_module_text("BigHeadFilter.Feather")
#define TEXT_BORDER_THICKNESS obs_module_text("BigHeadFilter.BorderThickness")
#define TEXT_BORDER_COLOR obs_module_text("BigHeadFilter.BorderColor")
#define TEXT_MANUAL_CENTER_X obs_module_text("BigHeadFilter.ManualCenterX")
#define TEXT_MANUAL_CENTER_Y obs_module_text("BigHeadFilter.ManualCenterY")
#define TEXT_MANUAL_RADIUS obs_module_text("BigHeadFilter.ManualRadius")
#define TEXT_SIMILARITY obs_module_text("BigHeadFilter.Similarity")
#define TEXT_REFERENCE_INFO obs_module_text("BigHeadFilter.ReferenceInfo")
#define TEXT_CAPTURE_REFERENCE obs_module_text("BigHeadFilter.CaptureReference")
#define TEXT_CLEAR_REFERENCE obs_module_text("BigHeadFilter.ClearReference")
#define TEXT_REFERENCE_SET obs_module_text("BigHeadFilter.ReferenceSet")
#define TEXT_REFERENCE_NOT_SET obs_module_text("BigHeadFilter.ReferenceNotSet")
#define TEXT_SHOW_MASK obs_module_text("BigHeadFilter.ShowMask")

/* Longest side of the detection canvas.  The actual image keeps its aspect
 * ratio and occupies the top-left of the padded network input, so faces are
 * never distorted before embedding. */
#define BIG_HEAD_CANVAS 320
#define BIG_HEAD_CANVAS_MIN 64

/* Detection and recognition rates.  Recognition is only run when a reference
 * exists; without one the filter just needs face boxes. */
#define BIG_HEAD_SCORE_THRESHOLD 0.75f
#define BIG_HEAD_NMS_THRESHOLD 0.30f

/* How long the last trusted target is kept after the worker stops seeing a
 * face, before the manual circle takes over. */
#define BIG_HEAD_TARGET_TIMEOUT_NS 3000000000ULL

/* Consecutive detections required before switching to a different face. */
#define BIG_HEAD_SWITCH_FRAMES 3

#define BIG_HEAD_SIMILARITY_MARGIN 0.08f

struct big_head_target {
	bool valid;
	struct face_detection face;
	float similarity;
	uint64_t timestamp;
	uint64_t serial;
};

struct big_head_data {
	obs_source_t *context;
	gs_effect_t *effect;

	/* Graphics-thread readback. */
	gs_texrender_t *capture_render;
	gs_stagesurf_t *capture_stage;
	uint32_t capture_width;
	uint32_t capture_height;
	uint8_t *rgba[2];
	size_t rgba_capacity;
	int write_buffer;
	int pending_buffer;
	bool input_pending;
	uint32_t pending_image_width;
	uint32_t pending_image_height;
	uint64_t pending_timestamp;
	uint64_t last_capture_time;

	/* Worker state. */
	pthread_mutex_t mutex;
	os_event_t *wake_event;
	pthread_t worker;
	bool worker_started;
	bool stop;
	bool models_ready;
	struct face_ort ort;

	/* Settings, written by the UI thread under the mutex. */
	bool auto_track;
	int detect_fps;
	double radius_scale;
	bool fixed_size;
	double fixed_radius;
	double offset_x;
	double offset_y;
	double feather;
	int border_thickness;
	struct vec4 border_color;
	double manual_center_x;
	double manual_center_y;
	double manual_radius;
	double similarity_threshold;
	bool show_mask;

	bool has_reference;
	float reference[FACE_EMBED_DIM];
	bool capture_requested;
	int switch_frames;

	struct big_head_target published;

	/* Graphics-thread copy of the published target. */
	struct big_head_target current;
	uint64_t uploaded_serial;
	struct vec2 smooth_center;
	float smooth_radius;
	bool smooth_valid;
};

static const char *big_head_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BigHeadFilter");
}

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float stabilize(float current, float target, float dead, float alpha)
{
	const float delta = target - current;

	if (fabsf(delta) <= dead)
		return current;

	return current + alpha * (delta - (delta > 0.0f ? dead : -dead));
}

static float face_iou(const struct face_detection *a, const struct face_detection *b)
{
	const float ax2 = a->x + a->width;
	const float ay2 = a->y + a->height;
	const float bx2 = b->x + b->width;
	const float by2 = b->y + b->height;
	const float ix1 = a->x > b->x ? a->x : b->x;
	const float iy1 = a->y > b->y ? a->y : b->y;
	const float ix2 = ax2 < bx2 ? ax2 : bx2;
	const float iy2 = ay2 < by2 ? ay2 : by2;
	const float iw = ix2 - ix1;
	const float ih = iy2 - iy1;

	if (iw <= 0.0f || ih <= 0.0f)
		return 0.0f;

	const float intersection = iw * ih;
	const float union_area = a->width * a->height + b->width * b->height - intersection;
	return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static int find_previous_face(const struct face_detection *faces, size_t count, const struct face_detection *prev)
{
	int best = -1;
	float best_iou = 0.05f;

	for (size_t i = 0; i < count; i++) {
		const float iou = face_iou(&faces[i], prev);
		if (iou > best_iou) {
			best_iou = iou;
			best = (int)i;
		}
	}

	return best;
}

static void embedding_to_hex(const float *embedding, char *out)
{
	static const char digits[] = "0123456789abcdef";
	const uint8_t *bytes = (const uint8_t *)embedding;
	const size_t size = FACE_EMBED_DIM * sizeof(float);

	for (size_t i = 0; i < size; i++) {
		out[i * 2] = digits[bytes[i] >> 4];
		out[i * 2 + 1] = digits[bytes[i] & 0x0f];
	}
	out[size * 2] = '\0';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool embedding_from_hex(const char *hex, float *embedding)
{
	const size_t size = FACE_EMBED_DIM * sizeof(float);
	uint8_t *bytes = (uint8_t *)embedding;

	if (!hex || strlen(hex) != size * 2)
		return false;

	for (size_t i = 0; i < size; i++) {
		const int hi = hex_value(hex[i * 2]);
		const int lo = hex_value(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;
		bytes[i] = (uint8_t)((hi << 4) | lo);
	}

	return true;
}

static void big_head_persist_reference(void *param)
{
	obs_source_t *source = param;
	struct big_head_data *filter = obs_obj_get_data(source);

	if (!filter) {
		obs_source_release(source);
		return;
	}

	char hex[FACE_EMBED_DIM * sizeof(float) * 2 + 1];
	pthread_mutex_lock(&filter->mutex);
	if (filter->has_reference)
		embedding_to_hex(filter->reference, hex);
	else
		hex[0] = '\0';
	pthread_mutex_unlock(&filter->mutex);

	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, S_REFERENCE, hex);
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);
}

static void big_head_update(void *data, obs_data_t *settings)
{
	struct big_head_data *filter = data;

	float reference[FACE_EMBED_DIM];
	const char *hex = obs_data_get_string(settings, S_REFERENCE);
	const bool has_reference = embedding_from_hex(hex, reference);

	pthread_mutex_lock(&filter->mutex);
	filter->auto_track = obs_data_get_bool(settings, S_AUTO_TRACK);
	filter->detect_fps = (int)obs_data_get_int(settings, S_DETECT_FPS);
	filter->radius_scale = obs_data_get_double(settings, S_RADIUS_SCALE);
	filter->fixed_size = obs_data_get_bool(settings, S_FIXED_SIZE);
	filter->fixed_radius = obs_data_get_double(settings, S_FIXED_RADIUS);
	filter->offset_x = obs_data_get_double(settings, S_OFFSET_X);
	filter->offset_y = obs_data_get_double(settings, S_OFFSET_Y);
	filter->feather = obs_data_get_double(settings, S_FEATHER);
	filter->border_thickness = (int)obs_data_get_int(settings, S_BORDER_THICKNESS);
	vec4_from_rgba_srgb(&filter->border_color, (uint32_t)obs_data_get_int(settings, S_BORDER_COLOR));
	filter->manual_center_x = obs_data_get_double(settings, S_MANUAL_CENTER_X);
	filter->manual_center_y = obs_data_get_double(settings, S_MANUAL_CENTER_Y);
	filter->manual_radius = obs_data_get_double(settings, S_MANUAL_RADIUS);
	filter->similarity_threshold = obs_data_get_double(settings, S_SIMILARITY);
	filter->show_mask = obs_data_get_bool(settings, S_SHOW_MASK);
	filter->has_reference = has_reference;
	if (has_reference)
		memcpy(filter->reference, reference, sizeof(filter->reference));
	else
		memset(filter->reference, 0, sizeof(filter->reference));
	pthread_mutex_unlock(&filter->mutex);
}

static void *big_head_worker(void *data)
{
	struct big_head_data *filter = data;

	char *detect_path = obs_module_file("face-models/face_detection_yunet_2026may.onnx");
	char *embed_path = obs_module_file("face-models/face_recognition_sface_2021dec.onnx");
	bool loaded = false;

	if (detect_path && embed_path) {
		wchar_t *detect_wide = NULL;
		wchar_t *embed_wide = NULL;

		if (os_utf8_to_wcs_ptr(detect_path, 0, &detect_wide) && os_utf8_to_wcs_ptr(embed_path, 0, &embed_wide))
			loaded = face_ort_load(&filter->ort, detect_wide, embed_wide);

		bfree(detect_wide);
		bfree(embed_wide);
	}

	bfree(detect_path);
	bfree(embed_path);

	pthread_mutex_lock(&filter->mutex);
	filter->models_ready = loaded;
	pthread_mutex_unlock(&filter->mutex);

	if (loaded) {
		blog(LOG_INFO, "[big head: '%s'] face models ready (ONNX Runtime %s)",
		     obs_source_get_name(filter->context), filter->ort.version);
	} else {
		blog(LOG_WARNING, "[big head: '%s'] face models unavailable, using the manual circle",
		     obs_source_get_name(filter->context));
	}

	for (;;) {
		os_event_wait(filter->wake_event);

		const uint8_t *rgba = NULL;
		uint32_t image_width = 0;
		uint32_t image_height = 0;
		uint64_t timestamp = 0;
		bool capture_requested = false;
		bool has_reference = false;
		bool models_ready = false;
		double manual_center_x = 0.5;
		double manual_center_y = 0.35;
		double similarity_threshold = 0.36;
		float reference[FACE_EMBED_DIM];
		struct big_head_target previous;

		pthread_mutex_lock(&filter->mutex);
		if (filter->stop) {
			pthread_mutex_unlock(&filter->mutex);
			break;
		}

		previous = filter->published;
		models_ready = filter->models_ready;

		if (filter->input_pending) {
			rgba = filter->rgba[filter->pending_buffer];
			image_width = filter->pending_image_width;
			image_height = filter->pending_image_height;
			timestamp = filter->pending_timestamp;
			capture_requested = filter->capture_requested;
			has_reference = filter->has_reference;
			manual_center_x = filter->manual_center_x;
			manual_center_y = filter->manual_center_y;
			similarity_threshold = filter->similarity_threshold;
			memcpy(reference, filter->reference, sizeof(reference));

			filter->capture_requested = false;
			filter->input_pending = false;
			filter->write_buffer = 1 - filter->pending_buffer;
		}
		pthread_mutex_unlock(&filter->mutex);

		if (!rgba || !models_ready)
			continue;

		struct face_detection faces[FACE_DETECT_MAX_FACES];
		const size_t face_count = face_ort_detect(&filter->ort, rgba, (size_t)image_width * 4, image_width,
							  image_height, BIG_HEAD_SCORE_THRESHOLD,
							  BIG_HEAD_NMS_THRESHOLD, faces, FACE_DETECT_MAX_FACES);

		struct face_detection selected;
		bool selected_valid = false;
		float selected_similarity = 0.0f;

		if (capture_requested) {
			if (face_count) {
				size_t chosen = 0;
				float best = FLT_MAX;

				for (size_t i = 0; i < face_count; i++) {
					const float cx = faces[i].x + faces[i].width * 0.5f;
					const float cy = faces[i].y + faces[i].height * 0.5f;
					const float dx = cx - (float)manual_center_x;
					const float dy = cy - (float)manual_center_y;
					const float distance = dx * dx + dy * dy;

					if (distance < best) {
						best = distance;
						chosen = i;
					}
				}

				float embedding[FACE_EMBED_DIM];
				if (face_ort_embed(&filter->ort, rgba, (size_t)image_width * 4, image_width,
						   image_height, &faces[chosen], embedding)) {
					pthread_mutex_lock(&filter->mutex);
					memcpy(filter->reference, embedding, sizeof(filter->reference));
					filter->has_reference = true;
					filter->published.valid = true;
					filter->published.face = faces[chosen];
					filter->published.similarity = 1.0f;
					filter->published.timestamp = timestamp;
					filter->published.serial++;
					pthread_mutex_unlock(&filter->mutex);

					blog(LOG_INFO, "[big head: '%s'] reference face captured",
					     obs_source_get_name(filter->context));

					obs_source_get_ref(filter->context);
					obs_queue_task(OBS_TASK_UI, big_head_persist_reference, filter->context, false);
				}
			} else {
				blog(LOG_WARNING, "[big head: '%s'] no face found for reference capture",
				     obs_source_get_name(filter->context));
			}

			continue;
		}

		if (face_count) {
			if (has_reference) {
				float similarities[FACE_DETECT_MAX_FACES];
				bool embedded[FACE_DETECT_MAX_FACES] = {0};
				size_t best = 0;
				float best_similarity = -1.0f;

				for (size_t i = 0; i < face_count; i++) {
					float embedding[FACE_EMBED_DIM];

					if (!face_ort_embed(&filter->ort, rgba, (size_t)image_width * 4, image_width,
							    image_height, &faces[i], embedding)) {
						similarities[i] = -1.0f;
						continue;
					}

					embedded[i] = true;
					similarities[i] =
						face_embedding_similarity(embedding, reference, FACE_EMBED_DIM);

					if (similarities[i] > best_similarity) {
						best_similarity = similarities[i];
						best = i;
					}
				}

				if (best_similarity >= (float)similarity_threshold) {
					const int previous_match =
						previous.valid ? find_previous_face(faces, face_count, &previous.face)
							       : -1;

					if (previous_match >= 0 && embedded[previous_match] &&
					    similarities[previous_match] >= (float)similarity_threshold &&
					    best_similarity - similarities[previous_match] <
						    BIG_HEAD_SIMILARITY_MARGIN) {
						best = (size_t)previous_match;
						filter->switch_frames = 0;
					} else if (previous_match >= 0 && embedded[previous_match] &&
						   (int)best != previous_match) {
						if (++filter->switch_frames < BIG_HEAD_SWITCH_FRAMES)
							best = (size_t)previous_match;
						else
							filter->switch_frames = 0;
					} else {
						filter->switch_frames = 0;
					}

					selected = faces[best];
					selected_valid = true;
					selected_similarity = similarities[best];
				}
			} else {
				int previous_match =
					previous.valid ? find_previous_face(faces, face_count, &previous.face) : -1;

				if (previous_match >= 0) {
					selected = faces[previous_match];
				} else {
					size_t chosen = 0;
					float best = FLT_MAX;

					for (size_t i = 0; i < face_count; i++) {
						const float cx = faces[i].x + faces[i].width * 0.5f;
						const float cy = faces[i].y + faces[i].height * 0.5f;
						const float dx = cx - (float)manual_center_x;
						const float dy = cy - (float)manual_center_y;
						const float distance = dx * dx + dy * dy;

						if (distance < best) {
							best = distance;
							chosen = i;
						}
					}

					selected = faces[chosen];
				}

				selected_valid = true;
			}
		}

		pthread_mutex_lock(&filter->mutex);
		filter->published.valid = selected_valid;
		if (selected_valid) {
			filter->published.face = selected;
			filter->published.similarity = selected_similarity;
		}
		filter->published.timestamp = timestamp;
		filter->published.serial++;
		pthread_mutex_unlock(&filter->mutex);
	}

	face_ort_unload(&filter->ort);
	return NULL;
}

static void *big_head_create(obs_data_t *settings, obs_source_t *context)
{
	struct big_head_data *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	pthread_mutex_init(&filter->mutex, NULL);

	if (os_event_init(&filter->wake_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;

	char *effect_path = obs_module_file("big_head.effect");
	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);

	if (!filter->effect) {
		blog(LOG_ERROR, "[big head] could not load big_head.effect");
		goto fail;
	}

	big_head_update(filter, settings);

	if (pthread_create(&filter->worker, NULL, big_head_worker, filter) != 0)
		goto fail;
	filter->worker_started = true;

	return filter;

fail:
	if (filter->worker_started) {
		pthread_mutex_lock(&filter->mutex);
		filter->stop = true;
		pthread_mutex_unlock(&filter->mutex);
		os_event_signal(filter->wake_event);
		pthread_join(filter->worker, NULL);
	}
	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}
	if (filter->wake_event)
		os_event_destroy(filter->wake_event);
	bfree(filter->rgba[0]);
	bfree(filter->rgba[1]);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
	return NULL;
}

static void big_head_destroy(void *data)
{
	struct big_head_data *filter = data;

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
	if (filter->capture_render)
		gs_texrender_destroy(filter->capture_render);
	if (filter->capture_stage)
		gs_stagesurface_destroy(filter->capture_stage);
	obs_leave_graphics();

	bfree(filter->rgba[0]);
	bfree(filter->rgba[1]);
	os_event_destroy(filter->wake_event);
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

static void big_head_capture(struct big_head_data *filter, obs_source_t *target, obs_source_t *parent, bool auto_track,
			     int detect_fps)
{
	pthread_mutex_lock(&filter->mutex);
	const bool ready = filter->models_ready;
	const bool busy = filter->input_pending;
	const bool capture_requested = filter->capture_requested;
	pthread_mutex_unlock(&filter->mutex);

	if (!ready || busy)
		return;

	/* Reference enrolment still needs a frame when automatic tracking is
	 * off, so the user can capture once and enable tracking afterwards. */
	if (!auto_track && !capture_requested)
		return;

	const uint32_t source_width = obs_source_get_base_width(target);
	const uint32_t source_height = obs_source_get_base_height(target);

	if (!source_width || !source_height)
		return;

	const uint64_t now = obs_get_video_frame_time();
	const uint64_t interval = detect_fps > 0 ? 1000000000ULL / (uint64_t)detect_fps : 0;

	if (interval && filter->last_capture_time && now > filter->last_capture_time &&
	    now - filter->last_capture_time < interval)
		return;

	uint32_t image_width;
	uint32_t image_height;

	if (source_width >= source_height) {
		image_width = BIG_HEAD_CANVAS;
		image_height = (uint32_t)lround((double)BIG_HEAD_CANVAS * source_height / source_width);
		if (image_height < BIG_HEAD_CANVAS_MIN)
			image_height = BIG_HEAD_CANVAS_MIN;
	} else {
		image_height = BIG_HEAD_CANVAS;
		image_width = (uint32_t)lround((double)BIG_HEAD_CANVAS * source_width / source_height);
		if (image_width < BIG_HEAD_CANVAS_MIN)
			image_width = BIG_HEAD_CANVAS_MIN;
	}

	const size_t required = (size_t)image_width * image_height * 4;
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

	if (image_width != filter->capture_width || image_height != filter->capture_height) {
		if (filter->capture_stage) {
			gs_stagesurface_destroy(filter->capture_stage);
			filter->capture_stage = NULL;
		}
		filter->capture_stage = gs_stagesurface_create(image_width, image_height, GS_RGBA);
		filter->capture_width = image_width;
		filter->capture_height = image_height;
	}

	if (!filter->capture_render)
		filter->capture_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (!filter->capture_render || !filter->capture_stage)
		return;

	gs_texrender_reset(filter->capture_render);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(filter->capture_render, image_width, image_height)) {
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
				uint8_t *dst = filter->rgba[filter->write_buffer];
				const uint32_t row = image_width * 4;

				if (linesize == row) {
					memcpy(dst, mapped, (size_t)row * image_height);
				} else {
					for (uint32_t y = 0; y < image_height; y++)
						memcpy(dst + (size_t)y * row, mapped + (size_t)y * linesize, row);
				}

				pthread_mutex_lock(&filter->mutex);
				filter->pending_buffer = filter->write_buffer;
				filter->pending_image_width = image_width;
				filter->pending_image_height = image_height;
				filter->pending_timestamp = now;
				filter->input_pending = true;
				pthread_mutex_unlock(&filter->mutex);

				gs_stagesurface_unmap(filter->capture_stage);
				filter->last_capture_time = now;
				os_event_signal(filter->wake_event);
			} else {
				gs_stagesurface_unmap(filter->capture_stage);
			}
		}
	}

	gs_blend_state_pop();
}

static bool head_circle_from_face(const struct face_detection *face, float source_width, float source_height,
				  float radius_scale, float offset_x, float offset_y, struct vec2 *center,
				  float *radius)
{
	const float eye_dx = (face->landmarks[1][0] - face->landmarks[0][0]) * source_width;
	const float eye_dy = (face->landmarks[1][1] - face->landmarks[0][1]) * source_height;
	const float eye_distance = sqrtf(eye_dx * eye_dx + eye_dy * eye_dy);

	float center_x;
	float center_y;
	float head_radius;

	if (eye_distance > 1.0f) {
		center_x = (face->landmarks[0][0] + face->landmarks[1][0]) * 0.5f * source_width;
		center_y =
			(face->landmarks[0][1] + face->landmarks[1][1]) * 0.5f * source_height + 0.175f * eye_distance;
		head_radius = 1.75f * eye_distance;
	} else {
		center_x = (face->x + face->width * 0.5f) * source_width;
		center_y = (face->y + face->height * 0.5f) * source_height;
		head_radius = 0.72f * face->width * source_width;
	}

	head_radius *= radius_scale;
	center_x += offset_x * source_width;
	center_y += offset_y * source_height;

	center->x = center_x / source_width;
	center->y = center_y / source_height;
	*radius = head_radius / source_height;
	return true;
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
	bool fixed_size;
	double fixed_radius;
	double offset_x;
	double offset_y;
	double feather;
	int border_thickness;
	struct vec4 border_color;
	double manual_center_x;
	double manual_center_y;
	double manual_radius;
	bool show_mask;

	pthread_mutex_lock(&filter->mutex);
	auto_track = filter->auto_track;
	detect_fps = filter->detect_fps;
	radius_scale = filter->radius_scale;
	fixed_size = filter->fixed_size;
	fixed_radius = filter->fixed_radius;
	offset_x = filter->offset_x;
	offset_y = filter->offset_y;
	feather = filter->feather;
	border_thickness = filter->border_thickness;
	border_color = filter->border_color;
	manual_center_x = filter->manual_center_x;
	manual_center_y = filter->manual_center_y;
	manual_radius = filter->manual_radius;
	show_mask = filter->show_mask;
	pthread_mutex_unlock(&filter->mutex);

	big_head_capture(filter, target, parent, auto_track, detect_fps);

	if (pthread_mutex_trylock(&filter->mutex) == 0) {
		if (filter->published.serial != filter->uploaded_serial) {
			/* Keep the last valid target while the worker reports
			 * "no face"; its timestamp drives the timeout that
			 * eventually falls back to the manual circle. */
			if (filter->published.valid)
				filter->current = filter->published;
			filter->uploaded_serial = filter->published.serial;
		}
		pthread_mutex_unlock(&filter->mutex);
	}

	const float source_width = (float)obs_source_get_base_width(target);
	const float source_height = (float)obs_source_get_base_height(target);

	if (source_width <= 0.0f || source_height <= 0.0f) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const float aspect = source_width / source_height;
	const uint64_t now = obs_get_video_frame_time();

	struct vec2 center;
	float radius;
	bool use_target = false;

	if (auto_track && filter->current.valid && filter->current.timestamp && now >= filter->current.timestamp &&
	    now - filter->current.timestamp <= BIG_HEAD_TARGET_TIMEOUT_NS) {
		struct vec2 target_center;
		float target_radius;

		if (head_circle_from_face(&filter->current.face, source_width, source_height, (float)radius_scale,
					  (float)offset_x, (float)offset_y, &target_center, &target_radius)) {
			if (fixed_size)
				target_radius = (float)fixed_radius;

			if (!filter->smooth_valid) {
				filter->smooth_center = target_center;
				filter->smooth_radius = target_radius;
				filter->smooth_valid = true;
			} else {
				const float dx = target_center.x - filter->smooth_center.x;
				const float dy = target_center.y - filter->smooth_center.y;

				if (dx * dx + dy * dy > 0.015f) {
					filter->smooth_center = target_center;
					filter->smooth_radius = target_radius;
				} else {
					filter->smooth_center.x =
						stabilize(filter->smooth_center.x, target_center.x, 0.002f, 0.35f);
					filter->smooth_center.y =
						stabilize(filter->smooth_center.y, target_center.y, 0.002f, 0.35f);
					filter->smooth_radius =
						stabilize(filter->smooth_radius, target_radius, 0.0015f, 0.35f);
				}
			}

			/* A fixed radius snaps exactly to the selected size while the
			 * centre keeps the normal tracking smoothing. */
			if (fixed_size)
				filter->smooth_radius = target_radius;

			center = filter->smooth_center;
			radius = filter->smooth_radius;
			use_target = true;
		}
	}

	if (!use_target) {
		filter->smooth_valid = false;
		center.x = (float)manual_center_x;
		center.y = (float)manual_center_y;
		radius = fixed_size ? (float)fixed_radius : (float)manual_radius;
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
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "border_width"),
			    (float)border_thickness / source_height);
	gs_effect_set_float(gs_effect_get_param_by_name(filter->effect, "border_aa"), 1.0f / source_height);
	gs_effect_set_vec4(gs_effect_get_param_by_name(filter->effect, "border_color"), &border_color);
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

static bool big_head_capture_reference_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct big_head_data *filter = data;

	pthread_mutex_lock(&filter->mutex);
	if (filter->models_ready)
		filter->capture_requested = true;
	else
		blog(LOG_WARNING, "[big head: '%s'] face models are not ready", obs_source_get_name(filter->context));
	pthread_mutex_unlock(&filter->mutex);

	return true;
}

static bool big_head_clear_reference_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct big_head_data *filter = data;

	pthread_mutex_lock(&filter->mutex);
	filter->has_reference = false;
	memset(filter->reference, 0, sizeof(filter->reference));
	pthread_mutex_unlock(&filter->mutex);

	obs_data_t *settings = obs_source_get_settings(filter->context);
	obs_data_set_string(settings, S_REFERENCE, "");
	obs_source_update(filter->context, settings);
	obs_data_release(settings);

	return true;
}

static obs_properties_t *big_head_properties(void *data)
{
	struct big_head_data *filter = data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "info", TEXT_INFO, OBS_TEXT_INFO);
	obs_properties_add_bool(props, S_AUTO_TRACK, TEXT_AUTO_TRACK);

	obs_property_t *fps = obs_properties_add_list(props, S_DETECT_FPS, TEXT_DETECT_FPS, OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "10", 10);
	obs_property_list_add_int(fps, "15", 15);
	obs_property_list_add_int(fps, "20", 20);
	obs_property_list_add_int(fps, "30", 30);

	obs_properties_add_float_slider(props, S_SIMILARITY, TEXT_SIMILARITY, 0.10, 0.90, 0.01);
	obs_properties_add_float_slider(props, S_RADIUS_SCALE, TEXT_RADIUS_SCALE, 0.4, 1.6, 0.01);
	obs_properties_add_bool(props, S_FIXED_SIZE, TEXT_FIXED_SIZE);
	obs_properties_add_float_slider(props, S_FIXED_RADIUS, TEXT_FIXED_RADIUS, 0.02, 0.75, 0.01);
	obs_properties_add_float_slider(props, S_OFFSET_X, TEXT_OFFSET_X, -0.5, 0.5, 0.01);
	obs_properties_add_float_slider(props, S_OFFSET_Y, TEXT_OFFSET_Y, -0.5, 0.5, 0.01);
	obs_properties_add_float_slider(props, S_FEATHER, TEXT_FEATHER, 0.0, 0.2, 0.005);
	obs_properties_add_int_slider(props, S_BORDER_THICKNESS, TEXT_BORDER_THICKNESS, 0, 64, 1);
	obs_properties_add_color_alpha(props, S_BORDER_COLOR, TEXT_BORDER_COLOR);

	obs_properties_add_text(props, "reference_info", TEXT_REFERENCE_INFO, OBS_TEXT_INFO);

	bool has_reference;
	pthread_mutex_lock(&filter->mutex);
	has_reference = filter->has_reference;
	pthread_mutex_unlock(&filter->mutex);

	obs_properties_add_text(props, "reference_status", has_reference ? TEXT_REFERENCE_SET : TEXT_REFERENCE_NOT_SET,
				OBS_TEXT_INFO);
	obs_properties_add_button(props, "capture_reference", TEXT_CAPTURE_REFERENCE,
				  big_head_capture_reference_clicked);
	obs_properties_add_button(props, "clear_reference", TEXT_CLEAR_REFERENCE, big_head_clear_reference_clicked);

	obs_properties_add_float_slider(props, S_MANUAL_CENTER_X, TEXT_MANUAL_CENTER_X, 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_MANUAL_CENTER_Y, TEXT_MANUAL_CENTER_Y, 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_MANUAL_RADIUS, TEXT_MANUAL_RADIUS, 0.02, 0.75, 0.01);
	obs_properties_add_bool(props, S_SHOW_MASK, TEXT_SHOW_MASK);

	return props;
}

static void big_head_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_AUTO_TRACK, true);
	obs_data_set_default_int(settings, S_DETECT_FPS, 15);
	obs_data_set_default_double(settings, S_SIMILARITY, 0.36);
	obs_data_set_default_double(settings, S_RADIUS_SCALE, 1.0);
	obs_data_set_default_bool(settings, S_FIXED_SIZE, false);
	obs_data_set_default_double(settings, S_FIXED_RADIUS, 0.25);
	obs_data_set_default_double(settings, S_OFFSET_X, 0.0);
	obs_data_set_default_double(settings, S_OFFSET_Y, 0.0);
	obs_data_set_default_double(settings, S_FEATHER, 0.02);
	obs_data_set_default_int(settings, S_BORDER_THICKNESS, 4);
	obs_data_set_default_int(settings, S_BORDER_COLOR, 0xFFFFFFFF);
	obs_data_set_default_double(settings, S_MANUAL_CENTER_X, 0.5);
	obs_data_set_default_double(settings, S_MANUAL_CENTER_Y, 0.35);
	obs_data_set_default_double(settings, S_MANUAL_RADIUS, 0.25);
	obs_data_set_default_string(settings, S_REFERENCE, "");
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
