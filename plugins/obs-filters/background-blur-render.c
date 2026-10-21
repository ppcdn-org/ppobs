#include "background-blur-render.h"

#include <graphics/vec4.h>

void background_blur_set_matte(gs_effect_t *effect, const struct background_blur_matte *matte)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "mask_tex"), matte->texture);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "mask_scale"), &matte->scale);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "mask_offset"), &matte->offset);
}

/* One direction of the blur: draws `source` through the BlurPass technique into
 * `dest`.  The first pass (horizontal) reads plain colour and writes weighted
 * colour plus weight; the second reads that and resolves to colour. */
static bool background_blur_pass(gs_effect_t *effect, gs_texture_t *source, gs_texrender_t *dest, uint32_t width,
				 uint32_t height, const struct background_blur_matte *matte, struct vec2 step,
				 float inv2s2, bool second)
{
	gs_texrender_reset(dest);
	if (!gs_texrender_begin(dest, width, height))
		return false;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	/* Overwrite rather than blend: the alpha channel carries the weight in
	 * the first pass, and blending it would corrupt that. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), source);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "blur_step"), &step);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "blur_inv2s2"), inv2s2);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "blur_second_pass"), second ? 1.0f : 0.0f);
	background_blur_set_matte(effect, matte);

	while (gs_effect_loop(effect, "BlurPass"))
		gs_draw_sprite(source, 0, width, height);

	gs_blend_state_pop();
	gs_texrender_end(dest);
	return true;
}

gs_texture_t *background_blur_render(gs_effect_t *effect, struct background_blur_targets *targets,
				     const struct background_blur_plan *plan, const struct background_blur_matte *matte,
				     background_blur_draw_fn draw, void *draw_data)
{
	if (!effect || !plan->valid || !matte->texture)
		return NULL;

	/* The middle target holds weighted colour and a weight that sums past 1,
	 * which an 8-bit format would clip. */
	static const enum gs_color_format formats[BLUR_TARGET_COUNT] = {GS_RGBA, GS_RGBA16F, GS_RGBA};
	for (int i = 0; i < BLUR_TARGET_COUNT; i++) {
		if (!targets->render[i])
			targets->render[i] = gs_texrender_create(formats[i], GS_ZS_NONE);
		if (!targets->render[i])
			return NULL;
	}

	gs_texrender_t *reduced = targets->render[BLUR_TARGET_REDUCED];
	gs_texrender_reset(reduced);
	if (!gs_texrender_begin(reduced, plan->width, plan->height))
		return NULL;
	draw(draw_data, plan->width, plan->height);
	gs_texrender_end(reduced);

	gs_texture_t *reduced_texture = gs_texrender_get_texture(reduced);
	if (!reduced_texture)
		return NULL;

	const float inv2s2 = 1.0f / (2.0f * plan->sigma * plan->sigma);
	const struct vec2 horizontal = {1.0f / (float)plan->width, 0.0f};
	const struct vec2 vertical = {0.0f, 1.0f / (float)plan->height};

	gs_texrender_t *middle = targets->render[BLUR_TARGET_MIDDLE];
	if (!background_blur_pass(effect, reduced_texture, middle, plan->width, plan->height, matte, horizontal, inv2s2,
				  false))
		return NULL;

	gs_texture_t *middle_texture = gs_texrender_get_texture(middle);
	if (!middle_texture)
		return NULL;

	gs_texrender_t *result = targets->render[BLUR_TARGET_RESULT];
	if (!background_blur_pass(effect, middle_texture, result, plan->width, plan->height, matte, vertical, inv2s2,
				  true))
		return NULL;

	return gs_texrender_get_texture(result);
}

void background_blur_targets_destroy(struct background_blur_targets *targets)
{
	for (int i = 0; i < BLUR_TARGET_COUNT; i++) {
		if (targets->render[i])
			gs_texrender_destroy(targets->render[i]);
		targets->render[i] = NULL;
	}
}
