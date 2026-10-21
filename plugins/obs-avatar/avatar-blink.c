#include "avatar-blink.h"

#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/base.h>
#include <util/bmem.h>

#include <math.h>
#include <string.h>

/* The lid closes in 80 ms and opens in 140: slower to open is how a real one
 * looks, and a symmetric blink looks like a shutter. */
#define BLINK_CLOSE_S 0.08
#define BLINK_OPEN_S 0.14

#define BLINK_MIN_INTERVAL_S 2.0
#define BLINK_SPREAD_S 4.0

/* One blink in this many is followed straight away by another. */
#define BLINK_DOUBLE_ODDS 0.15
#define BLINK_DOUBLE_GAP_S 0.45

/* The same drawing as the prototype this was worked out in, written for the
 * GPU: per pixel, how far the lid's patch covers it, the skin to cover it with
 * (sampled from just above, 1.9 eye heights up), and the lash line. Everything
 * is in the reference frame's pixels so the thresholds mean the same thing at
 * any size the avatar is drawn. The two eyes are separate parameter sets because
 * the effect language has no arrays to loop over. */
static const char avatar_blink_effect[] =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float2 size;\n"
	"uniform float blink;\n"
	"uniform float has1;\n"
	"uniform float4 e0;\n"
	"uniform float3 e0u;\n"
	"uniform float3 e0l;\n"
	"uniform float4 e1;\n"
	"uniform float3 e1u;\n"
	"uniform float3 e1l;\n"
	"sampler_state def_sampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
	"struct VertInOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"VertInOut VSDefault(VertInOut vert_in) {\n"
	"	VertInOut vert_out;\n"
	"	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);\n"
	"	vert_out.uv = vert_in.uv;\n"
	"	return vert_out;\n"
	"}\n"
	"float sm(float x) { float t = saturate(x); return t * t * (3.0 - 2.0 * t); }\n"
	"float lid(float3 c, float x) { return c.x * x * x + c.y * x + c.z; }\n"
	"float edge(float3 u, float3 l, float x, float b, float eh) {\n"
	"	float uy = lid(u, x);\n"
	"	float ly = lid(l, x);\n"
	"	return uy + b * (ly - uy + 0.25 * eh * b);\n"
	"}\n"
	"float eyew(float2 p, float4 g, float3 u, float3 l, float b) {\n"
	"	float xr = p.x - g.x;\n"
	"	float width = g.z - g.y;\n"
	"	float mid = 0.5 * (g.y + g.z) - g.x;\n"
	"	float wx = sm(1.0 - (abs(xr - mid) - 0.5 * width) / (0.25 * width + 0.001));\n"
	"	float uy = lid(u, xr);\n"
	"	float ed = edge(u, l, xr, b, g.w);\n"
	"	float above = sm((ed - p.y) / (0.45 * g.w) + 0.5);\n"
	"	float topf = sm((p.y - (uy - 1.4 * g.w)) / g.w);\n"
	"	return wx * above * topf;\n"
	"}\n"
	"float eyeline(float2 p, float4 g, float3 u, float3 l, float b) {\n"
	"	float ed = edge(u, l, p.x - g.x, b, g.w);\n"
	"	float inside = step(g.y, p.x) * step(p.x, g.z);\n"
	"	return inside * (1.0 - sm(abs(p.y - ed) / 1.2));\n"
	"}\n"
	"float4 PSBlink(VertInOut vert_in) : TARGET {\n"
	"	float2 p = vert_in.uv * size;\n"
	"	float w0 = eyew(p, e0, e0u, e0l, blink);\n"
	"	float w1 = eyew(p, e1, e1u, e1l, blink) * has1;\n"
	"	float3 c0 = image.Sample(def_sampler, float2(vert_in.uv.x, vert_in.uv.y - 1.9 * e0.w / size.y)).rgb;\n"
	"	float3 c1 = image.Sample(def_sampler, float2(vert_in.uv.x, vert_in.uv.y - 1.9 * e1.w / size.y)).rgb;\n"
	"	float w = w0 + w1;\n"
	"	float3 dark = float3(0.019, 0.014, 0.014);\n"
	"	float3 skin = lerp(dark, (c0 * w0 + c1 * w1) / max(w, 0.0001), saturate(w * 1000.0));\n"
	"	float ln = max(eyeline(p, e0, e0u, e0l, blink), eyeline(p, e1, e1u, e1l, blink) * has1);\n"
	"	ln = ln * min(1.0, 0.25 + blink) * 0.8;\n"
	"	return float4(lerp(skin, dark, ln), max(saturate(w), ln));\n"
	"}\n"
	"technique Draw { pass { vertex_shader = VSDefault(vert_in); pixel_shader = PSBlink(vert_in); } }\n";

static double blink_rand(struct avatar_blink *blink)
{
	/* xorshift64: not for anything but when to blink. */
	uint64_t x = blink->rng ? blink->rng : 0x9E3779B97F4A7C15ULL;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	blink->rng = x;
	return (double)(x >> 11) / (double)(1ULL << 53);
}

static float blink_smooth(double x)
{
	x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
	return (float)(x * x * (3.0 - 2.0 * x));
}

void avatar_blink_init(struct avatar_blink *blink, uint64_t now_ns)
{
	memset(blink, 0, sizeof(*blink));
	blink->rng = now_ns ^ 0xA5A5A5A55A5A5A5AULL;
	blink->next_ns = now_ns + (uint64_t)(1.5e9);
}

float avatar_blink_amount(struct avatar_blink *blink, uint64_t now_ns)
{
	const double total = BLINK_CLOSE_S + BLINK_OPEN_S;

	if (blink->start_ns) {
		const double t = (double)(now_ns - blink->start_ns) / 1e9;
		if (t < total)
			return t < BLINK_CLOSE_S ? blink_smooth(t / BLINK_CLOSE_S)
						  : 1.0f - blink_smooth((t - BLINK_CLOSE_S) / BLINK_OPEN_S);

		blink->start_ns = 0;
	}

	if (now_ns >= blink->next_ns) {
		blink->start_ns = now_ns;

		const double gap = blink_rand(blink) < BLINK_DOUBLE_ODDS
					   ? BLINK_DOUBLE_GAP_S
					   : BLINK_MIN_INTERVAL_S + BLINK_SPREAD_S * blink_rand(blink);
		blink->next_ns = now_ns + (uint64_t)(gap * 1e9);
	}

	return 0.0f;
}

void avatar_blink_draw(struct avatar_blink *blink, gs_texture_t *base, uint32_t cx, uint32_t cy,
		       const struct avatar_eyes *eyes, float amount)
{
	if (!base || !eyes || !eyes->valid || eyes->count == 0 || !eyes->ref_width || !eyes->ref_height ||
	    amount <= 0.0f)
		return;

	if (!blink->effect && !blink->failed) {
		char *error = NULL;
		blink->effect = gs_effect_create(avatar_blink_effect, "avatar-blink", &error);
		if (!blink->effect) {
			blink->failed = true;
			blog(LOG_WARNING, "[obs-avatar] could not build the blink effect: %s", error ? error : "no detail");
		}
		bfree(error);
	}

	if (!blink->effect)
		return;

	gs_effect_t *effect = blink->effect;

	/* A single eye is drawn twice rather than leaving the second set unset. */
	const struct avatar_eye *first = &eyes->eye[0];
	const struct avatar_eye *second = eyes->count > 1 ? &eyes->eye[1] : first;

	struct vec2 size;
	vec2_set(&size, (float)eyes->ref_width, (float)eyes->ref_height);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), base);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "size"), &size);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "blink"), amount);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "has1"), eyes->count > 1 ? 1.0f : 0.0f);

	const struct avatar_eye *set[2] = {first, second};
	static const char *const geometry[2] = {"e0", "e1"};
	static const char *const upper[2] = {"e0u", "e1u"};
	static const char *const lower[2] = {"e0l", "e1l"};

	for (int i = 0; i < 2; i++) {
		struct vec4 g;
		struct vec3 u, l;
		vec4_set(&g, set[i]->cx, set[i]->left, set[i]->right, set[i]->eh);
		vec3_set(&u, set[i]->up[0], set[i]->up[1], set[i]->up[2]);
		vec3_set(&l, set[i]->lo[0], set[i]->lo[1], set[i]->lo[2]);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, geometry[i]), &g);
		gs_effect_set_vec3(gs_effect_get_param_by_name(effect, upper[i]), &u);
		gs_effect_set_vec3(gs_effect_get_param_by_name(effect, lower[i]), &l);
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(base, 0, cx, cy);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

void avatar_blink_free(struct avatar_blink *blink)
{
	if (blink->effect)
		gs_effect_destroy(blink->effect);
	blink->effect = NULL;
}
