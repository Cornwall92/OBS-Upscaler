// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <graphics/vec2.h>
#include <util/threading.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "upscale-policy.h"

enum warning {
	WARN_BACKEND = 1 << 0,
	WARN_SHADER = 1 << 1,
	WARN_HDR = 1 << 2,
	WARN_SIZE = 1 << 3,
	WARN_CANVAS = 1 << 4,
	WARN_OUTPUT = 1 << 5,
	WARN_LAG = 1 << 6,
};

struct upscale_filter {
	obs_source_t *source;
	pthread_mutex_t mutex;
	gs_effect_t *effect;
	gs_eparam_t *source_size, *texel_size, *sharpness_param;
	gs_eparam_t *split_param, *position_param, *ringing_param;
	bool supported, adaptive, split, anti_ringing, rendered;
	int mode, size;
	uint32_t custom_width, custom_height;
	float sharpness, split_position;
	struct upscale_adaptation adaptation;
	uint32_t input_width, input_height, output_width, output_height;
	enum upscale_result dimension_result;
	bool bypass;
	unsigned warnings, last_logged_warnings;
	struct obs_video_info video;
	uint64_t last_poll;
	uint32_t last_frames, last_lagged;
	double average_ms, lag_percent;
	char backend[64];
};

static const char *mode_name(int mode)
{
	static const char *names[] = {"Performance", "Balanced", "Quality"};
	return names[upscale_clamp_mode(mode)];
}

static const char *filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Filter.Name");
}

static enum gs_color_space target_space(obs_source_t *target)
{
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED, GS_CS_709_SCRGB};
	return target ? obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces) : GS_CS_SRGB;
}

/* Query upstream callbacks before taking our lock: some filters ask for the
 * parent's final dimensions, which re-enters our width/height callbacks. */
static void refresh_dimensions(struct upscale_filter *filter)
{
	obs_source_t *target = obs_filter_get_target(filter->source);
	const uint32_t input_width = target ? obs_source_get_base_width(target) : 0;
	const uint32_t input_height = target ? obs_source_get_base_height(target) : 0;
	const enum gs_color_space space = target_space(target);
	struct obs_video_info video = {0};
	obs_get_video_info(&video);

	pthread_mutex_lock(&filter->mutex);
	filter->input_width = input_width;
	filter->input_height = input_height;
	filter->video = video;
	const struct upscale_dimensions dimensions =
	    upscale_dimensions(input_width, input_height, filter->size, filter->custom_width, filter->custom_height);
	filter->dimension_result = dimensions.result;
	unsigned warnings = filter->warnings & WARN_LAG;
	if (!filter->supported)
		warnings |= WARN_BACKEND;
	if (!filter->effect)
		warnings |= WARN_SHADER;
	if (dimensions.result == UPSCALE_TOO_LARGE)
		warnings |= WARN_SIZE;
	if ((space != GS_CS_SRGB && space != GS_CS_SRGB_16F) || video.colorspace == VIDEO_CS_2100_PQ ||
	    video.colorspace == VIDEO_CS_2100_HLG)
		warnings |= WARN_HDR;
	filter->bypass =
	    dimensions.result != UPSCALE_READY || (warnings & (WARN_BACKEND | WARN_SHADER | WARN_SIZE | WARN_HDR));
	filter->output_width = filter->bypass ? input_width : dimensions.width;
	filter->output_height = filter->bypass ? input_height : dimensions.height;
	if (!filter->bypass) {
		if (video.base_width < dimensions.width || video.base_height < dimensions.height)
			warnings |= WARN_CANVAS;
		if (video.output_width < dimensions.width || video.output_height < dimensions.height)
			warnings |= WARN_OUTPUT;
	}
	filter->warnings = warnings;
	const bool log_warning = warnings && warnings != filter->last_logged_warnings;
	filter->last_logged_warnings = warnings;
	const bool bypass = filter->bypass;
	const uint32_t output_width = filter->output_width;
	const uint32_t output_height = filter->output_height;
	pthread_mutex_unlock(&filter->mutex);

	/* Report transitions only. Logging can also invoke external callbacks. */
	if (log_warning)
		blog(LOG_WARNING,
		     "[obs-upscaler] Compatibility/performance warning flags=0x%x; open filter diagnostics. "
		     "backend=%s input=%ux%u output=%ux%u bypass=%s",
		     warnings, filter->backend, input_width, input_height, output_width, output_height,
		     bypass ? "yes" : "no");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct upscale_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	const long long raw_mode = obs_data_get_int(settings, "mode");
	const int mode = raw_mode >= 0 && raw_mode <= 2 ? (int)raw_mode : UPSCALE_BALANCED;
	const bool adaptive = obs_data_get_bool(settings, "adaptive");
	if (filter->mode != mode || filter->adaptive != adaptive)
		upscale_adaptation_reset(&filter->adaptation, mode);
	filter->mode = mode;
	filter->adaptive = adaptive;
	const long long size = obs_data_get_int(settings, "size");
	filter->size = size >= 0 && size <= 3 ? (int)size : UPSCALE_4K;
	const long long width = obs_data_get_int(settings, "width");
	const long long height = obs_data_get_int(settings, "height");
	filter->custom_width = width >= 16 && width <= UPSCALE_MAX_DIMENSION ? (uint32_t)width : 0;
	filter->custom_height = height >= 16 && height <= UPSCALE_MAX_DIMENSION ? (uint32_t)height : 0;
	filter->sharpness = upscale_clamp_sharpness(obs_data_get_double(settings, "sharpness"));
	filter->split = obs_data_get_bool(settings, "split");
	filter->split_position = upscale_clamp_sharpness(obs_data_get_double(settings, "split_position"));
	filter->anti_ringing = obs_data_get_bool(settings, "anti_ringing");
	pthread_mutex_unlock(&filter->mutex);
}

static void filter_destroy(void *data)
{
	struct upscale_filter *filter = data;
	if (!filter)
		return;
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();
	pthread_mutex_destroy(&filter->mutex);
	bfree(filter);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct upscale_filter *filter = bzalloc(sizeof(*filter));
	filter->source = source;
	filter->mode = -1;
	filter->bypass = true;
	if (pthread_mutex_init(&filter->mutex, NULL) != 0) {
		bfree(filter);
		return NULL;
	}
	char *path = obs_module_file("upscale.effect");
	char *errors = NULL;
	obs_enter_graphics();
	const int device = gs_get_device_type();
	filter->supported = device == GS_DEVICE_DIRECT3D_11 || device == GS_DEVICE_OPENGL;
	const char *name = gs_get_device_name();
	snprintf(filter->backend, sizeof(filter->backend), "%s", name ? name : "Unknown");
	if (filter->supported && path)
		filter->effect = gs_effect_create_from_file(path, &errors);
	if (filter->effect) {
		filter->source_size = gs_effect_get_param_by_name(filter->effect, "source_size");
		filter->texel_size = gs_effect_get_param_by_name(filter->effect, "texel_size");
		filter->sharpness_param = gs_effect_get_param_by_name(filter->effect, "sharpness");
		filter->split_param = gs_effect_get_param_by_name(filter->effect, "split_enabled");
		filter->position_param = gs_effect_get_param_by_name(filter->effect, "split_position");
		filter->ringing_param = gs_effect_get_param_by_name(filter->effect, "anti_ringing");
		if (!filter->source_size || !filter->texel_size || !filter->sharpness_param || !filter->split_param ||
		    !filter->position_param || !filter->ringing_param ||
		    !gs_effect_get_technique(filter->effect, "Performance") ||
		    !gs_effect_get_technique(filter->effect, "Balanced") ||
		    !gs_effect_get_technique(filter->effect, "Quality")) {
			gs_effect_destroy(filter->effect);
			filter->effect = NULL;
		}
	}
	obs_leave_graphics();
	if (!filter->effect)
		blog(LOG_WARNING, "[obs-upscaler] Shader unavailable; passing source through. %s",
		     errors ? errors : "Check plugin data files and graphics driver.");
	bfree(errors);
	bfree(path);
	filter_update(filter, settings);
	return filter;
}

static void filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct upscale_filter *filter = data;
	refresh_dimensions(filter);
	const uint64_t now = os_gettime_ns();
	const uint32_t frames = obs_get_total_frames();
	const uint32_t lagged = obs_get_lagged_frames();
	const double render_ns = (double)obs_get_average_frame_time_ns();
	bool changed = false;
	int requested_mode = UPSCALE_BALANCED;
	int effective_mode = UPSCALE_BALANCED;

	pthread_mutex_lock(&filter->mutex);
	if (!filter->last_poll || now - filter->last_poll >= 2000000000ull) {
		const uint32_t elapsed = frames - filter->last_frames;
		const uint32_t missed = lagged - filter->last_lagged;
		const double budget_ns =
		    filter->video.fps_num ? 1.e9 * filter->video.fps_den / filter->video.fps_num : 0.0;
		filter->average_ms = render_ns / 1.e6;
		/* A reset of OBS counters is not an enormous burst of dropped frames. */
		const bool valid = filter->last_poll && filter->rendered && !filter->bypass &&
				   frames >= filter->last_frames && lagged >= filter->last_lagged && elapsed > 0 &&
				   missed <= elapsed;
		filter->lag_percent = valid ? 100.0 * missed / elapsed : 0.0;
		if (valid && (filter->lag_percent >= 1.0 || (budget_ns > 0 && render_ns >= budget_ns * 0.9)))
			filter->warnings |= WARN_LAG;
		else
			filter->warnings &= ~WARN_LAG;
		changed = upscale_adaptation_sample(&filter->adaptation, filter->adaptive, filter->mode,
						    valid ? elapsed : 0, valid ? missed : 0, render_ns, budget_ns);
		requested_mode = filter->mode;
		effective_mode = filter->adaptation.effective_mode;
		filter->last_poll = now;
		filter->last_frames = frames;
		filter->last_lagged = lagged;
		filter->rendered = false;
	}
	pthread_mutex_unlock(&filter->mutex);

	if (changed)
		blog(LOG_WARNING,
		     "[obs-upscaler] OBS-wide render pressure: selected %s, now using %s. "
		     "Resolution/FPS unchanged. Reset in filter properties to retry.",
		     mode_name(requested_mode), mode_name(effective_mode));
}

static void filter_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct upscale_filter *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->source);
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (!target || !parent)
		return;

	/* Source sizes can change between ticks (capture resize or filter reorder). */
	refresh_dimensions(filter);
	const enum gs_color_space current = gs_get_color_space();
	const enum gs_color_space space = target_space(target);

	pthread_mutex_lock(&filter->mutex);
	const bool bypass = filter->bypass;
	const uint32_t input_width = filter->input_width;
	const uint32_t input_height = filter->input_height;
	const uint32_t output_width = filter->output_width;
	const uint32_t output_height = filter->output_height;
	const float sharpness = filter->sharpness;
	const bool split = filter->split;
	const float split_position = filter->split_position;
	const bool anti_ringing = filter->anti_ringing;
	const int effective_mode = filter->adaptation.effective_mode;
	pthread_mutex_unlock(&filter->mutex);

	/* Never hold our mutex while rendering upstream: a source or earlier
	 * filter may query the parent's output size and re-enter our getters. */
	if (bypass || (current != GS_CS_SRGB && current != GS_CS_SRGB_16F)) {
		obs_source_skip_video_filter(filter->source);
		return;
	}
	if (obs_source_process_filter_begin_with_color_space(filter->source, gs_get_format_from_space(space), space,
							     OBS_NO_DIRECT_RENDERING)) {
		struct vec2 size, texel;
		vec2_set(&size, (float)input_width, (float)input_height);
		vec2_set(&texel, 1.0f / size.x, 1.0f / size.y);
		/* Effect and parameter pointers remain immutable for this instance. */
		gs_effect_set_vec2(filter->source_size, &size);
		gs_effect_set_vec2(filter->texel_size, &texel);
		gs_effect_set_float(filter->sharpness_param, sharpness);
		gs_effect_set_bool(filter->split_param, split);
		gs_effect_set_float(filter->position_param, split_position);
		gs_effect_set_bool(filter->ringing_param, anti_ringing);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_process_filter_tech_end(filter->source, filter->effect, output_width, output_height,
						   mode_name(effective_mode));
		gs_blend_state_pop();
		pthread_mutex_lock(&filter->mutex);
		filter->rendered = true;
		pthread_mutex_unlock(&filter->mutex);
	}
}

static uint32_t filter_width(void *data)
{
	struct upscale_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	const uint32_t width = filter->output_width;
	pthread_mutex_unlock(&filter->mutex);
	return width;
}

static uint32_t filter_height(void *data)
{
	struct upscale_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	const uint32_t height = filter->output_height;
	pthread_mutex_unlock(&filter->mutex);
	return height;
}

static enum gs_color_space filter_space(void *data, size_t count, const enum gs_color_space *spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(spaces);
	struct upscale_filter *filter = data;
	return target_space(obs_filter_get_target(filter->source));
}

static void set_diagnostics(obs_properties_t *props, struct upscale_filter *filter)
{
	if (!filter)
		return;
	char text[4096];
	pthread_mutex_lock(&filter->mutex);
	const double fps = filter->video.fps_den ? (double)filter->video.fps_num / filter->video.fps_den : 0.0;
	snprintf(
	    text, sizeof(text),
	    "Backend: %s | OBS %s\nInput: %ux%u -> filter: %ux%u | OBS: %.2f FPS\n"
	    "Selected: %s | Effective: %s%s\n"
	    "OBS average render time: %.2f ms | render lag in last sample: %.2f%%\n"
	    "These are OBS-wide statistics, not this filter's GPU time. Click Refresh to update.\n%s%s%s%s%s%s%s%s%s%s",
	    filter->backend, obs_get_version_string(), filter->input_width, filter->input_height, filter->output_width,
	    filter->output_height, fps, mode_name(filter->mode), mode_name(filter->adaptation.effective_mode),
	    filter->bypass ? " (bypassed)" : "", filter->average_ms, filter->lag_percent,
	    filter->warnings & WARN_BACKEND ? "WARNING: Unsupported graphics backend. Source passed through.\n" : "",
	    filter->warnings & WARN_SHADER ? "WARNING: Shader could not load/compile. Check data files, driver and OBS "
					     "log. Source passed through.\n"
					   : "",
	    filter->warnings & WARN_HDR
		? "WARNING: HDR/extended colour is unsupported in this version. Source passed through unchanged.\n"
		: "",
	    filter->warnings & WARN_SIZE ? "WARNING: Invalid or excessive dimensions. Limit: 8192 per axis and 33.55 "
					   "megapixels. Source passed through.\n"
					 : "",
	    filter->warnings & WARN_CANVAS ? "WARNING: Canvas is smaller than filter output. Set the canvas to the "
					     "intended output size and fit the source.\n"
					   : "",
	    filter->warnings & WARN_OUTPUT
		? "WARNING: OBS output resolution is smaller than filter output; detail will be reduced again.\n"
		: "",
	    filter->warnings & WARN_LAG
		? "WARNING: OBS reports rendering pressure. Lower the preset or game GPU load; check View > Stats.\n"
		: "",
	    filter->dimension_result == UPSCALE_NO_SOURCE ? "Waiting for source video.\n" : "",
	    filter->dimension_result == UPSCALE_NO_ENLARGEMENT
		? "Source already meets/exceeds the selected size. Upscaling bypassed.\n"
		: "",
	    filter->adaptation.effective_mode != filter->mode
		? "Automatic fallback is active; Reset retries the selected quality.\n"
		: "");
	const bool warning = filter->warnings != 0;
	pthread_mutex_unlock(&filter->mutex);
	obs_property_t *info = obs_properties_get(props, "diagnostics");
	obs_property_set_description(info, text);
	obs_property_text_set_info_type(info, warning ? OBS_TEXT_INFO_WARNING : OBS_TEXT_INFO_NORMAL);
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	set_diagnostics(props, data);
	return true;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	struct upscale_filter *filter = data;
	if (filter) {
		pthread_mutex_lock(&filter->mutex);
		upscale_adaptation_reset(&filter->adaptation, filter->mode);
		filter->last_poll = 0;
		pthread_mutex_unlock(&filter->mutex);
	}
	set_diagnostics(props, filter);
	return true;
}

static bool controls_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool custom = obs_data_get_int(settings, "size") == UPSCALE_CUSTOM;
	obs_property_set_visible(obs_properties_get(props, "width"), custom);
	obs_property_set_visible(obs_properties_get(props, "height"), custom);
	obs_property_set_visible(obs_properties_get(props, "split_position"), obs_data_get_bool(settings, "split"));
	return true;
}

static obs_properties_t *filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "about", obs_module_text("Filter.About"), OBS_TEXT_INFO);
	obs_property_t *p =
	    obs_properties_add_list(props, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Mode.Performance"), UPSCALE_PERFORMANCE);
	obs_property_list_add_int(p, obs_module_text("Mode.Balanced"), UPSCALE_BALANCED);
	obs_property_list_add_int(p, obs_module_text("Mode.Quality"), UPSCALE_QUALITY);
	p = obs_properties_add_list(props, "size", obs_module_text("Size"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "3840 x 2160 (4K UHD)", UPSCALE_4K);
	obs_property_list_add_int(p, "2560 x 1440 (1440p)", UPSCALE_1440P);
	obs_property_list_add_int(p, obs_module_text("Size.Double"), UPSCALE_DOUBLE);
	obs_property_list_add_int(p, obs_module_text("Size.Custom"), UPSCALE_CUSTOM);
	obs_property_set_modified_callback(p, controls_modified);
	obs_properties_add_int(props, "width", obs_module_text("Width"), 16, UPSCALE_MAX_DIMENSION, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 16, UPSCALE_MAX_DIMENSION, 1);
	obs_properties_add_float_slider(props, "sharpness", obs_module_text("Sharpness"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(props, "anti_ringing", obs_module_text("AntiRinging"));
	obs_properties_add_bool(props, "adaptive", obs_module_text("Adaptive"));
	p = obs_properties_add_bool(props, "split", obs_module_text("Split"));
	obs_property_set_modified_callback(p, controls_modified);
	obs_properties_add_float_slider(props, "split_position", obs_module_text("Split.Position"), 0.0, 1.0, 0.01);
	obs_properties_add_text(props, "diagnostics", obs_module_text("Diagnostics"), OBS_TEXT_INFO);
	obs_properties_add_button(props, "refresh", obs_module_text("Refresh"), refresh_clicked);
	obs_properties_add_button(props, "reset", obs_module_text("Reset"), reset_clicked);
	set_diagnostics(props, data);
	return props;
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", UPSCALE_BALANCED);
	obs_data_set_default_int(settings, "size", UPSCALE_4K);
	obs_data_set_default_int(settings, "width", 3840);
	obs_data_set_default_int(settings, "height", 2160);
	obs_data_set_default_double(settings, "sharpness", 0.15);
	obs_data_set_default_bool(settings, "anti_ringing", true);
	obs_data_set_default_bool(settings, "adaptive", true);
	obs_data_set_default_bool(settings, "split", false);
	obs_data_set_default_double(settings, "split_position", 0.5);
}

struct obs_source_info upscale_filter_info = {
    .id = "obs_upscaler_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = filter_name,
    .create = filter_create,
    .destroy = filter_destroy,
    .update = filter_update,
    .video_tick = filter_tick,
    .video_render = filter_render,
    .get_width = filter_width,
    .get_height = filter_height,
    .video_get_color_space = filter_space,
    .get_properties = filter_properties,
    .get_defaults = filter_defaults,
};
