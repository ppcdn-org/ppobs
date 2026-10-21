/* The contract both lip-sync modes implement.
 *
 * Mode A (MuseTalk, in a worker process - avatar-engine-sidecar.c) and mode B
 * (viseme mouth sprites, in-process - stage B) have almost nothing in common
 * internally: one streams audio to another process and gets BGRA frames back,
 * the other composites sprites on the GPU. What they share is this: audio goes
 * in, and the most recent frame can be handed to the renderer.
 *
 * Keeping that behind one vtable is what lets the source fall back from A to B
 * without knowing anything about either, and what lets mode A's worker be
 * replaced later (TensorRT, or in-process ONNX Runtime if the export ever
 * lands) without the source changing at all.
 *
 * There are two ways an engine can affect the picture, and an engine provides
 * exactly one of them:
 *
 *   lock_frame / unlock_frame - the engine produces pixels of its own. Frames
 *       are borrowed rather than copied: lock_frame holds the engine's frame
 *       lock so the renderer can upload straight from the engine's buffer. The
 *       renderer must not hold that lock across anything slow, since the
 *       engine's reader thread is blocked on it meanwhile.
 *
 *   render - the engine composites the source's own artwork on the GPU. It
 *       produces no pixels to hand over; it needs the base texture instead.
 *
 * Mode A is the first kind: its pixels come from another process. Mode B is
 * the second: it deforms the avatar the user already chose, and routing that
 * through a CPU frame buffer would mean reading a texture back, warping it on
 * the CPU and uploading it again - three steps to avoid one shader.
 */

#pragma once

#include <obs-module.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct avatar_engine_config {
	/* Prepared avatar bundle (mode A) or sprite directory (mode B). May be
	 * empty, which both modes must survive - the user can enable the source
	 * before choosing an asset. */
	const char *asset_path;

	/* How the plugin starts the worker, if it is to start one at all. An empty
	 * or NULL worker_script means "a worker is being started by hand": the
	 * pipe waits and the developer runs the worker in a terminal, which is
	 * also how the loopback test drives the engine.
	 *
	 * With a script, the engine runs `worker_python worker_script` itself,
	 * hands it the pipe and the MuseTalk checkout, and relaunches it with a
	 * growing delay when it dies. worker_python may be empty (then "python"
	 * from PATH). worker_log, when set, is where the worker's stdout and
	 * stderr go: the interpreter's import errors and tracebacks happen
	 * before the handshake, so nothing else would ever show them. */
	const char *worker_script;
	const char *worker_engine; /* the worker's --engine; NULL means "musetalk" */
	const char *worker_python;
	const char *musetalk_dir;
	const char *worker_log;

	uint32_t fps;
	uint32_t sample_rate;

	/* Mode A: how far the mouth may travel, as a multiple of what the model
	 * produces. 0 means "not set" and is treated as 1.0. Changed at run time
	 * through avatar_engine_sidecar_set_mouth_gain. */
	float mouth_gain;
};

struct avatar_engine_frame {
	const uint8_t *bgra;
	uint32_t width;
	uint32_t height;
	/* The audio timestamp this frame was generated for, which is what the
	 * sync offset has to be measured against - not the time it arrived. */
	int64_t pts_ns;

	/* When set, this is only the repainted region and its alpha is the
	 * blend mask: it has to be composited over the artwork at (x, y) in a
	 * space `ref_width` by `ref_height`, not shown on its own. */
	bool is_region;
	uint32_t x;
	uint32_t y;
	uint32_t ref_width;
	uint32_t ref_height;
};

struct avatar_engine_stats {
	bool connected;
	bool real_model;
	char engine[64]; /* what the worker calls itself, for the UI and log */

	uint32_t whisper_us;
	uint32_t unet_us;
	uint32_t vae_us;
	uint32_t queue_depth;

	uint64_t frames;
	uint64_t dropped;

	/* The frame rate the worker is being asked for now; it can come down from
	 * the configured one when the worker cannot keep up. */
	uint32_t fps;

	/* The audio timestamp of the frame most recently drawn, which is how far
	 * behind the voice the picture is. Engines that draw for themselves are
	 * the only place that can say; the source cannot see what they showed. */
	bool shown;
	int64_t shown_pts_ns;
};

struct avatar_engine_ops {
	const char *id;

	void *(*create)(const struct avatar_engine_config *cfg, obs_source_t *owner);
	void (*destroy)(void *data);

	/* Whether the engine can currently produce frames. False is a normal
	 * state, not an error: the worker may not have connected yet. */
	bool (*ready)(void *data);

	void (*push_audio)(void *data, const float *samples, size_t count, int64_t pts_ns);

	/* Pixel-producing engines: true when a frame is available, in which case
	 * the caller must call unlock_frame. False leaves nothing locked.
	 * NULL on engines that render instead. */
	bool (*lock_frame)(void *data, struct avatar_engine_frame *out);
	void (*unlock_frame)(void *data);

	/* GPU-compositing engines: draw the avatar at cx by cy from `base`.
	 * Called on the graphics thread from inside the source's own render, so
	 * the context is current and no effect loop is open. Returns false when
	 * it could not draw, which makes the source fall back to the plain
	 * artwork rather than leaving a hole. NULL on engines that produce
	 * frames instead. */
	bool (*render)(void *data, gs_texture_t *base, uint32_t cx, uint32_t cy);

	void (*get_stats)(void *data, struct avatar_engine_stats *out);
};

/* Each engine exports its own instance from its own header:
 * avatar-engine-sidecar.h for mode A, and mode B's when stage B lands. */

#ifdef __cplusplus
}
#endif
