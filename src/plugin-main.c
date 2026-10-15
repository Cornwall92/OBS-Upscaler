// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-upscaler", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Portable GPU upscaling with quality presets and compatibility diagnostics.";
}
MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Upscaler";
}
MODULE_EXPORT const char *obs_module_author(void)
{
	return "James C";
}

extern struct obs_source_info upscale_filter_info;

bool obs_module_load(void)
{
	/* No upper-version restriction: use the stable public libobs API. */
	if (obs_get_version() < PLUGIN_MIN_OBS_API)
		blog(LOG_WARNING,
		     "[obs-upscaler] OBS %s is below the supported minimum %s. "
		     "This older runtime is best-effort only; please upgrade.",
		     obs_get_version_string(), PLUGIN_MIN_OBS_VERSION);
	obs_register_source(&upscale_filter_info);
	blog(LOG_INFO, "[obs-upscaler] %s loaded (minimum OBS %s; runtime %s)", PLUGIN_VERSION, PLUGIN_MIN_OBS_VERSION,
	     obs_get_version_string());
	return true;
}
