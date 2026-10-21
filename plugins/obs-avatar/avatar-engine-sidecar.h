/* Mode A's engine instance. avatar-engine.h defines the contract every mode
 * implements; each engine exports its own instance from its own header, so the
 * generic contract does not accumulate per-engine declarations. */

#pragma once

#include "avatar-engine.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct avatar_engine_ops avatar_engine_sidecar_ops;

/* The named pipe this engine is listening on, valid for the engine's lifetime.
 *
 * Mode-A specific on purpose, which is why it is not in avatar_engine_ops: a
 * sprite engine has no endpoint. Two callers need it - the properties page,
 * which shows the developer what to point a hand-started worker at, and the
 * loopback test, which starts a real worker against it. */
const char *avatar_engine_sidecar_pipe_name(void *data);

/* How far the worker lets the mouth travel, as a multiple of what the model
 * produces. Safe to call from any thread and never blocks: the value is kept
 * and handed to the worker by the engine's own writer, now if one is connected
 * and again after any reconnect. */
void avatar_engine_sidecar_set_mouth_gain(void *data, float gain);

#ifdef __cplusplus
}
#endif
