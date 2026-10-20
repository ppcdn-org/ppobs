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
 * Frames are borrowed rather than copied: lock_frame holds the engine's frame
 * lock so the renderer can upload straight from the engine's buffer, and
 * unlock_frame releases it. The renderer must not hold the lock across
 * anything slow - the engine's reader thread is blocked on it meanwhile.
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

	/* Command the plugin uses to start the worker, if it is to start one at
	 * all. Empty means "a worker is being started by hand", which is how
	 * the skeleton is driven: the pipe waits, the developer runs the worker
	 * in a terminal and sees its output directly. */
	const char *worker_command;

	uint32_t fps;
	uint32_t sample_rate;
};

struct avatar_engine_frame {
	const uint8_t *bgra;
	uint32_t width;
	uint32_t height;
	/* The audio timestamp this frame was generated for, which is what the
	 * sync offset has to be measured against - not the time it arrived. */
	int64_t pts_ns;
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
};

struct avatar_engine_ops {
	const char *id;

	void *(*create)(const struct avatar_engine_config *cfg, obs_source_t *owner);
	void (*destroy)(void *data);

	/* Whether the engine can currently produce frames. False is a normal
	 * state, not an error: the worker may not have connected yet. */
	bool (*ready)(void *data);

	void (*push_audio)(void *data, const float *samples, size_t count, int64_t pts_ns);

	/* True when a frame is available; the caller must then call
	 * unlock_frame. False leaves nothing locked. */
	bool (*lock_frame)(void *data, struct avatar_engine_frame *out);
	void (*unlock_frame)(void *data);

	void (*get_stats)(void *data, struct avatar_engine_stats *out);
};

/* Each engine exports its own instance from its own header:
 * avatar-engine-sidecar.h for mode A, and mode B's when stage B lands. */

#ifdef __cplusplus
}
#endif
