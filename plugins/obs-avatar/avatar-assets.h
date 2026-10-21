/* Resolution of the directory holding avatar assets and model weights.
 *
 * Everything lives under "background-models" inside the OBS configuration
 * directory:
 *
 *   portable:  <obs>/config/background-models
 *   installed: %APPDATA%/background-models
 *
 * The directory name is shared with obs-filters' virtual background on
 * purpose: this deployment keeps every AI weight in one place, which makes it
 * one thing to ship and one thing to look at when something is missing.  Each
 * feature owns a subdirectory, so no filenames collide:
 *
 *   background-models/*.onnx          matting models (obs-filters')
 *   background-models/musetalk/       MuseTalk weights (stage C)
 *   background-models/avatars/<id>/   prepared avatar bundles (stage C)
 *
 * Moving avatar assets to their own directory later is a one-constant change.
 *
 * The path comes from obs_module_config_path rather than os_get_config_path:
 * the latter is hard-wired to %APPDATA% on Windows and ignores portable mode.
 * See background-segmentation-filter.c for the longer version of the same
 * reasoning.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The asset root, whether or not it exists yet.  Caller owns the result. */
char *avatar_asset_root(void);

/* Where the plugin-started worker's stdout and stderr go, next to OBS's own
 * logs. The directory is created if needed. Caller owns the result. */
char *avatar_worker_log_path(void);

/* A subdirectory of the root ("musetalk", say), existing or not.  Caller owns
 * the result. */
char *avatar_asset_subdir(const char *leaf);

#ifdef __cplusplus
}
#endif
