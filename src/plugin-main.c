/*
 * Grouwdi-OBS-FrameGen - module entry point.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info capture_smoother_filter;
#ifdef _WIN32
extern struct obs_source_info motion_smoother_filter;
#endif

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

bool obs_module_load(void)
{
	obs_register_source(&capture_smoother_filter);
#ifdef _WIN32
	obs_register_source(&motion_smoother_filter);
#endif
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "unloaded");
}
