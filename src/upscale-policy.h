// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum upscale_mode { UPSCALE_PERFORMANCE, UPSCALE_BALANCED, UPSCALE_QUALITY };
enum upscale_size { UPSCALE_4K, UPSCALE_1440P, UPSCALE_DOUBLE, UPSCALE_CUSTOM };
enum upscale_result { UPSCALE_READY, UPSCALE_NO_SOURCE, UPSCALE_NO_ENLARGEMENT, UPSCALE_TOO_LARGE };

struct upscale_dimensions {
	uint32_t width;
	uint32_t height;
	enum upscale_result result;
};

/* A conservative allocation cap, independent of vendor/model marketing names. */
#define UPSCALE_MAX_DIMENSION 8192u
#define UPSCALE_MAX_PIXELS 33554432ull

struct upscale_dimensions upscale_dimensions(uint32_t width, uint32_t height, int size, uint32_t custom_width,
					     uint32_t custom_height);
float upscale_clamp_sharpness(double value);
int upscale_clamp_mode(int mode);

/* OBS-wide render pressure: never infer that this particular filter caused it. */
struct upscale_adaptation {
	int effective_mode;
	unsigned pressure_windows;
};
void upscale_adaptation_reset(struct upscale_adaptation *state, int requested_mode);
bool upscale_adaptation_sample(struct upscale_adaptation *state, bool enabled, int requested_mode, uint32_t frames,
			       uint32_t lagged, double render_ns, double budget_ns);
