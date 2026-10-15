// SPDX-License-Identifier: GPL-2.0-or-later
#include "upscale-policy.h"

#include <math.h>

struct upscale_dimensions upscale_dimensions(uint32_t width, uint32_t height, int size, uint32_t custom_width,
					     uint32_t custom_height)
{
	struct upscale_dimensions result = {width, height, UPSCALE_NO_SOURCE};
	if (!width || !height)
		return result;
	if (width > UPSCALE_MAX_DIMENSION || height > UPSCALE_MAX_DIMENSION ||
	    (uint64_t)width * height > UPSCALE_MAX_PIXELS) {
		result.result = UPSCALE_TOO_LARGE;
		return result;
	}

	double box_width = 3840.0, box_height = 2160.0;
	if (size == UPSCALE_1440P) {
		box_width = 2560.0;
		box_height = 1440.0;
	} else if (size == UPSCALE_DOUBLE) {
		box_width = width * 2.0;
		box_height = height * 2.0;
	} else if (size == UPSCALE_CUSTOM) {
		box_width = custom_width;
		box_height = custom_height;
	}
	if (box_width < 16 || box_height < 16 || box_width > UPSCALE_MAX_DIMENSION ||
	    box_height > UPSCALE_MAX_DIMENSION || box_width * box_height > UPSCALE_MAX_PIXELS) {
		result.result = UPSCALE_TOO_LARGE;
		return result;
	}
	const double scale = fmin(box_width / width, box_height / height);
	if (scale <= 1.0) {
		result.result = UPSCALE_NO_ENLARGEMENT;
		return result;
	}
	/* Fit within the bounding box without stretching or cropping the source. */
	result.width = (uint32_t)floor(width * scale + 0.5);
	result.height = (uint32_t)floor(height * scale + 0.5);
	result.result = UPSCALE_READY;
	return result;
}

float upscale_clamp_sharpness(double value)
{
	if (!isfinite(value))
		return 0.0f;
	return (float)fmax(0.0, fmin(1.0, value));
}

int upscale_clamp_mode(int mode)
{
	return mode < UPSCALE_PERFORMANCE || mode > UPSCALE_QUALITY ? UPSCALE_BALANCED : mode;
}

void upscale_adaptation_reset(struct upscale_adaptation *state, int requested_mode)
{
	state->effective_mode = upscale_clamp_mode(requested_mode);
	state->pressure_windows = 0;
}

bool upscale_adaptation_sample(struct upscale_adaptation *state, bool enabled, int requested_mode, uint32_t frames,
			       uint32_t lagged, double render_ns, double budget_ns)
{
	requested_mode = upscale_clamp_mode(requested_mode);
	if (!enabled) {
		bool changed = state->effective_mode != requested_mode;
		upscale_adaptation_reset(state, requested_mode);
		return changed;
	}
	if (state->effective_mode > requested_mode)
		upscale_adaptation_reset(state, requested_mode);
	/* Two consecutive 2-second windows, with enough frames to ignore startup. */
	bool pressure =
	    frames >= 30 && budget_ns > 0.0 && ((double)lagged / frames >= 0.01 || render_ns >= budget_ns * 0.90);
	state->pressure_windows = pressure ? state->pressure_windows + 1 : 0;
	if (state->pressure_windows < 2 || state->effective_mode == UPSCALE_PERFORMANCE)
		return false;
	state->effective_mode--;
	state->pressure_windows = 0;
	return true;
}
