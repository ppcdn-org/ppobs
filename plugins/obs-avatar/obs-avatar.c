#include <obs-module.h>

#include "avatar-assets.h"
#include "avatar-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-avatar", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Avatar livestream source with audio-driven lip sync";
}

bool obs_module_load(void)
{
	obs_register_source(&avatar_source_info);

	/* Logged once at startup so the directory to drop weights and avatar
	 * bundles into is in every log, whether or not anyone opens the
	 * properties page. */
	char *root = avatar_asset_root();
	blog(LOG_INFO, "[obs-avatar] asset and model directory: %s", root ? root : "(could not be resolved)");
	bfree(root);

	return true;
}
