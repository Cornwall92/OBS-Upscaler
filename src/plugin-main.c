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
	obs_register_source(&upscale_filter_info);
	blog(LOG_INFO, "[obs-upscaler] %s loaded (built for OBS 31.1.1; runtime %s)", PLUGIN_VERSION,
	     obs_get_version_string());
	return true;
}
