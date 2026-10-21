#include "avatar-assets.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

/* Shared with the virtual background on purpose; see avatar-assets.h. */
#define AVATAR_ASSET_DIR "background-models"

/* obs_module_config_path yields "<root>/obs-studio/plugin_config/obs-avatar",
 * where <root> is <obs>/config when portable and %APPDATA% when installed.
 * The path is cut at this marker rather than by trimming a fixed number of
 * components: the two layouts do not nest to the same depth, so a fixed count
 * is wrong for one of them. */
#define AVATAR_CONFIG_MARKER "/obs-studio/"

/* `below_obs_studio` selects one of the two sensible readings of "the config
 * directory", which differ by whether "obs-studio" is kept:
 *
 *   false -> <obs>/config/background-models   (portable)
 *            %APPDATA%/background-models      (installed)
 *   true  -> <obs>/config/obs-studio/background-models   (portable)
 *            %APPDATA%/obs-studio/background-models      (installed)
 *
 * Both are searched, matching the virtual background: either is a reasonable
 * place for someone to have put the folder. */
static char *avatar_config_leaf(bool below_obs_studio, const char *leaf)
{
	/* Asking for "" gives the module's own config directory, which sits
	 * under "<root>/obs-studio/plugin_config". */
	char *module_config = obs_module_config_path("");
	if (!module_config)
		return NULL;

	struct dstr path = {0};
	dstr_copy(&path, module_config);
	bfree(module_config);

	dstr_replace(&path, "\\", "/");

	const char *marker = strstr(path.array, AVATAR_CONFIG_MARKER);
	if (!marker) {
		dstr_free(&path);
		return NULL;
	}

	size_t keep = (size_t)(marker - path.array);
	if (below_obs_studio)
		keep += strlen(AVATAR_CONFIG_MARKER) - 1;
	dstr_resize(&path, keep);

	dstr_cat(&path, "/");
	dstr_cat(&path, leaf);
	return path.array;
}

static char *avatar_asset_root_variant(bool below_obs_studio)
{
	return avatar_config_leaf(below_obs_studio, AVATAR_ASSET_DIR);
}

char *avatar_worker_log_path(void)
{
	/* OBS keeps its own logs in <config>/obs-studio/logs, and the worker's
	 * belongs beside them. The directory exists whenever OBS is running, but
	 * is created anyway: a missing one would silently cost the log. */
	char *dir = avatar_config_leaf(true, "logs");
	if (!dir)
		return NULL;

	os_mkdirs(dir);

	struct dstr path = {0};
	dstr_printf(&path, "%s/avatar-worker.log", dir);
	bfree(dir);
	return path.array;
}

char *avatar_asset_root(void)
{
	/* Whichever variant already exists wins, so a folder created in either
	 * place is found.  When neither exists the primary location is still
	 * returned: the properties page has to show somewhere to put things. */
	for (int variant = 0; variant < 2; variant++) {
		char *dir = avatar_asset_root_variant(variant != 0);
		if (!dir)
			continue;
		if (os_file_exists(dir))
			return dir;
		bfree(dir);
	}

	return avatar_asset_root_variant(false);
}

char *avatar_asset_subdir(const char *leaf)
{
	char *root = avatar_asset_root();
	if (!root)
		return NULL;

	if (!leaf || !*leaf)
		return root;

	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", root, leaf);
	bfree(root);
	return path.array;
}
