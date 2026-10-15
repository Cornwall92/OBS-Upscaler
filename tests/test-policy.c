// SPDX-License-Identifier: GPL-2.0-or-later
#include "upscale-policy.h"

/* Keep test assertions active in optimized builds without conflicting /D /U
 * command-line definitions on Microsoft's compiler. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void expect_dimensions(uint32_t source_width, uint32_t source_height, int size, uint32_t box_width,
			      uint32_t box_height, uint32_t expected_width, uint32_t expected_height,
			      enum upscale_result expected_result)
{
	struct upscale_dimensions value = upscale_dimensions(source_width, source_height, size, box_width, box_height);
	assert(value.width == expected_width);
	assert(value.height == expected_height);
	assert(value.result == expected_result);
}

static void test_dimensions(void)
{
	expect_dimensions(1920, 1080, UPSCALE_4K, 0, 0, 3840, 2160, UPSCALE_READY);
	expect_dimensions(1920, 1080, UPSCALE_1440P, 0, 0, 2560, 1440, UPSCALE_READY);
	expect_dimensions(640, 480, UPSCALE_4K, 0, 0, 2880, 2160, UPSCALE_READY);
	expect_dimensions(1080, 1920, UPSCALE_4K, 0, 0, 1215, 2160, UPSCALE_READY);
	expect_dimensions(2560, 1080, UPSCALE_4K, 0, 0, 3840, 1620, UPSCALE_READY);
	expect_dimensions(853, 480, UPSCALE_DOUBLE, 0, 0, 1706, 960, UPSCALE_READY);
	expect_dimensions(640, 480, UPSCALE_CUSTOM, 2000, 1000, 1333, 1000, UPSCALE_READY);
	expect_dimensions(0, 1080, UPSCALE_4K, 0, 0, 0, 1080, UPSCALE_NO_SOURCE);
	expect_dimensions(1920, 0, UPSCALE_4K, 0, 0, 1920, 0, UPSCALE_NO_SOURCE);
	expect_dimensions(3840, 2160, UPSCALE_4K, 0, 0, 3840, 2160, UPSCALE_NO_ENLARGEMENT);
	expect_dimensions(4096, 2160, UPSCALE_4K, 0, 0, 4096, 2160, UPSCALE_NO_ENLARGEMENT);
	expect_dimensions(1920, 1080, UPSCALE_CUSTOM, 1000, 3000, 1920, 1080, UPSCALE_NO_ENLARGEMENT);
	expect_dimensions(UINT32_MAX, UINT32_MAX, UPSCALE_4K, 0, 0, UINT32_MAX, UINT32_MAX, UPSCALE_TOO_LARGE);
	expect_dimensions(8193, 100, UPSCALE_4K, 0, 0, 8193, 100, UPSCALE_TOO_LARGE);
	expect_dimensions(8192, 4097, UPSCALE_4K, 0, 0, 8192, 4097, UPSCALE_TOO_LARGE);
	expect_dimensions(8192, 4096, UPSCALE_4K, 0, 0, 8192, 4096, UPSCALE_NO_ENLARGEMENT);
	expect_dimensions(4096, 2048, UPSCALE_DOUBLE, 0, 0, 8192, 4096, UPSCALE_READY);
	expect_dimensions(4097, 2048, UPSCALE_DOUBLE, 0, 0, 4097, 2048, UPSCALE_TOO_LARGE);
	expect_dimensions(3000, 3000, UPSCALE_DOUBLE, 0, 0, 3000, 3000, UPSCALE_TOO_LARGE);
	expect_dimensions(640, 480, UPSCALE_CUSTOM, UINT32_MAX, 1000, 640, 480, UPSCALE_TOO_LARGE);
	expect_dimensions(640, 480, UPSCALE_CUSTOM, 0, 1000, 640, 480, UPSCALE_TOO_LARGE);
	expect_dimensions(640, 480, UPSCALE_CUSTOM, 1000, 15, 640, 480, UPSCALE_TOO_LARGE);
	expect_dimensions(640, 480, UPSCALE_CUSTOM, 8192, 8192, 640, 480, UPSCALE_TOO_LARGE);
	/* A stale/corrupt size enum safely uses the default 4K bounding box. */
	expect_dimensions(1920, 1080, -999, 0, 0, 3840, 2160, UPSCALE_READY);
}

static void test_setting_clamps(void)
{
	assert(upscale_clamp_sharpness(-1.0) == 0.0f);
	assert(upscale_clamp_sharpness(0.25) == 0.25f);
	assert(upscale_clamp_sharpness(2.0) == 1.0f);
	assert(upscale_clamp_sharpness(NAN) == 0.0f);
	assert(upscale_clamp_sharpness(INFINITY) == 0.0f);
	assert(upscale_clamp_sharpness(-INFINITY) == 0.0f);
	assert(upscale_clamp_mode(UPSCALE_PERFORMANCE) == UPSCALE_PERFORMANCE);
	assert(upscale_clamp_mode(UPSCALE_QUALITY) == UPSCALE_QUALITY);
	assert(upscale_clamp_mode(-1) == UPSCALE_BALANCED);
	assert(upscale_clamp_mode(3) == UPSCALE_BALANCED);
}

static void test_adaptation(void)
{
	struct upscale_adaptation state;
	const double budget = 1000000000.0 / 60.0;
	upscale_adaptation_reset(&state, UPSCALE_QUALITY);
	/* A startup sample with too few frames must not degrade the requested mode. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 29, 29, budget * 2.0, budget));
	assert(state.pressure_windows == 0 && state.effective_mode == UPSCALE_QUALITY);
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 2, 0, budget));
	/* A healthy window between bad windows resets the consecutive count. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 0, 0, budget));
	assert(state.pressure_windows == 0);
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 2, 0, budget));
	assert(upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 2, 0, budget));
	assert(state.effective_mode == UPSCALE_BALANCED && state.pressure_windows == 0);
	/* Render pressure alone also triggers fallback, even without lost frames. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 0, budget * 0.90, budget));
	assert(upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 0, budget * 0.90, budget));
	assert(state.effective_mode == UPSCALE_PERFORMANCE);
	for (unsigned i = 0; i < 8; i++)
		assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 120, budget, budget));
	assert(state.effective_mode == UPSCALE_PERFORMANCE);
	/* Stable windows do not immediately switch back and cause oscillation. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 0, 0, budget));
	assert(state.effective_mode == UPSCALE_PERFORMANCE);
	assert(upscale_adaptation_sample(&state, false, UPSCALE_QUALITY, 0, 0, 0, budget));
	assert(state.effective_mode == UPSCALE_QUALITY && state.pressure_windows == 0);
	assert(!upscale_adaptation_sample(&state, false, UPSCALE_QUALITY, 0, 0, 0, budget));
	/* An invalid/zero budget cannot signal genuine render pressure. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 120, budget, 0));
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 120, 120, budget, NAN));
	assert(state.pressure_windows == 0);
	/* At exactly one percent lost frames, two windows are sufficient. */
	assert(!upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 100, 1, 0, budget));
	assert(upscale_adaptation_sample(&state, true, UPSCALE_QUALITY, 100, 1, 0, budget));
	assert(state.effective_mode == UPSCALE_BALANCED);
	upscale_adaptation_reset(&state, -1);
	assert(state.effective_mode == UPSCALE_BALANCED);
}

int main(void)
{
	test_dimensions();
	test_setting_clamps();
	test_adaptation();
	puts("PASS: dimension boundaries, aspect preservation, setting clamps and fallback policy");
	return 0;
}
