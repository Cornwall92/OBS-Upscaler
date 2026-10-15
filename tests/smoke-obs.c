// SPDX-License-Identifier: GPL-2.0-or-later
/* Real libobs/driver smoke test. On headless Linux run under xvfb-run.
 * It tests correctness, not real-GPU performance or a 4K60 throughput claim. */
#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_2.h>

/* Hosted Windows runners need not expose a physical GPU. OBS can create a
 * D3D11 device on the DXGI software adapter, although its normal adapter list
 * hides that adapter. Selecting its real index exercises the unmodified OBS
 * D3D11 backend and shader compiler without making a hardware speed claim. */
static uint32_t test_adapter(void)
{
	IDXGIFactory1 *factory = NULL;
	HRESULT result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
	if (FAILED(result)) {
		fprintf(stderr, "DXGI enumeration unavailable (0x%08lx); trying adapter 0.\n", (unsigned long)result);
		return 0;
	}

	uint32_t selected = 0;
	bool found = false;
	for (UINT index = 0;; index++) {
		IDXGIAdapter1 *adapter = NULL;
		result = factory->lpVtbl->EnumAdapters1(factory, index, &adapter);
		if (result == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(result)) {
			fprintf(stderr, "DXGI adapter enumeration failed (0x%08lx).\n", (unsigned long)result);
			break;
		}
		DXGI_ADAPTER_DESC1 description;
		result = adapter->lpVtbl->GetDesc1(adapter, &description);
		found = SUCCEEDED(result) && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
		adapter->lpVtbl->Release(adapter);
		if (found) {
			selected = index;
			break;
		}
	}
	factory->lpVtbl->Release(factory);
	if (found)
		printf("D3D11 smoke test: selected DXGI software adapter %u.\n", selected);
	else
		puts("D3D11 smoke test: no software adapter found; trying adapter 0.");
	return selected;
}
#endif

static unsigned failures;
#define CHECK(condition, message)                                                                                      \
	do {                                                                                                           \
		if (!(condition)) {                                                                                    \
			fprintf(stderr, "FAIL: %s\n", message);                                                        \
			failures++;                                                                                    \
		}                                                                                                      \
	} while (0)

enum { INPUT_SIZE = 8, OUTPUT_SIZE = 32, OUTPUT_BYTES = OUTPUT_SIZE * OUTPUT_SIZE * 4 };

static bool read_texture(gs_texture_t *texture, uint32_t width, uint32_t height, uint8_t *output)
{
	gs_stagesurf_t *stage = gs_stagesurface_create(width, height, GS_RGBA);
	if (!stage)
		return false;
	gs_stage_texture(stage, texture);
	uint8_t *mapped = NULL;
	uint32_t stride = 0;
	const bool mapped_ok = gs_stagesurface_map(stage, &mapped, &stride);
	if (mapped_ok) {
		for (uint32_t y = 0; y < height; y++)
			memcpy(output + (size_t)y * width * 4, mapped + (size_t)y * stride, width * 4);
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	return mapped_ok;
}

static bool draw_shader(gs_effect_t *effect, const char *technique, const uint8_t *pixels, float sharpness, bool split,
			uint8_t *output)
{
	const uint8_t *planes[] = {pixels};
	gs_texture_t *texture = gs_texture_create(INPUT_SIZE, INPUT_SIZE, GS_RGBA, 1, planes, 0);
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	bool ok = false;
	if (!texture || !render)
		goto cleanup;
	if (!gs_texrender_begin(render, OUTPUT_SIZE, OUTPUT_SIZE))
		goto cleanup;
	struct vec4 clear = {0};
	struct vec2 size, inverse;
	vec2_set(&size, INPUT_SIZE, INPUT_SIZE);
	vec2_set(&inverse, 1.0f / INPUT_SIZE, 1.0f / INPUT_SIZE);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, OUTPUT_SIZE, 0.0f, OUTPUT_SIZE, -100.0f, 100.0f);
	gs_enable_framebuffer_srgb(false);
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);
	gs_blend_state_push();
	gs_enable_blending(false);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "source_size"), &size);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "texel_size"), &inverse);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "sharpness"), sharpness);
	gs_effect_set_bool(gs_effect_get_param_by_name(effect, "hdr_input"), false);
	gs_effect_set_bool(gs_effect_get_param_by_name(effect, "anti_ringing"), true);
	gs_effect_set_bool(gs_effect_get_param_by_name(effect, "split_enabled"), split);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "split_position"), 1.0f);
	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(texture, 0, OUTPUT_SIZE, OUTPUT_SIZE);
	gs_blend_state_pop();
	gs_texrender_end(render);
	ok = read_texture(gs_texrender_get_texture(render), OUTPUT_SIZE, OUTPUT_SIZE, output);
cleanup:
	gs_texrender_destroy(render);
	gs_texture_destroy(texture);
	return ok;
}

static bool near_byte(uint8_t actual, uint8_t expected)
{
	return abs((int)actual - expected) <= 2;
}

static void test_shaders(const char *data_path)
{
	char path[4096];
	int count = snprintf(path, sizeof(path), "%s/upscale.effect", data_path);
	CHECK(count > 0 && (size_t)count < sizeof(path), "shader path fits buffer");
	char *errors = NULL;
	gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
	if (errors) {
		fprintf(stderr, "Shader compiler: %s\n", errors);
		bfree(errors);
	}
	CHECK(effect != NULL, "driver compiles all shader techniques");
	if (!effect)
		return;
	const char *techniques[] = {"Performance", "Balanced", "Quality"};
	uint8_t input[INPUT_SIZE * INPUT_SIZE * 4];
	uint8_t output[OUTPUT_BYTES], baseline[OUTPUT_BYTES], unsharpened[OUTPUT_BYTES];
	const uint8_t solid[] = {64, 32, 16, 128};
	for (size_t mode = 0; mode < 3; mode++) {
		CHECK(gs_effect_get_technique(effect, techniques[mode]) != NULL, "technique is present");
		for (size_t pixel = 0; pixel < sizeof(input); pixel += 4)
			memcpy(input + pixel, solid, 4);
		for (unsigned sharpen = 0; sharpen < 2; sharpen++) {
			bool rendered = draw_shader(effect, techniques[mode], input, (float)sharpen, false, output);
			CHECK(rendered, "constant image renders and reads back");
			bool constant = rendered;
			for (size_t pixel = 0; rendered && pixel < OUTPUT_BYTES; pixel++)
				constant = constant && near_byte(output[pixel], solid[pixel % 4]);
			CHECK(constant, "constant colour and alpha survive reconstruction/sharpening");
			if (rendered && !constant)
				fprintf(stderr, "%s sharp=%u first=%u,%u,%u,%u center=%u,%u,%u,%u\n", techniques[mode],
					sharpen, output[0], output[1], output[2], output[3], output[2112], output[2113],
					output[2114], output[2115]);
		}
		memset(input, 0, sizeof(input));
		CHECK(draw_shader(effect, techniques[mode], input, 1.0f, false, output), "transparent image renders");
		bool transparent = true;
		for (size_t pixel = 0; pixel < OUTPUT_BYTES; pixel++)
			transparent = transparent && output[pixel] == 0;
		CHECK(transparent, "transparent image remains transparent black");
	}
	/* Asymmetric opaque quadrants reveal flipped coordinates or channel swaps. */
	for (unsigned y = 0; y < INPUT_SIZE; y++) {
		for (unsigned x = 0; x < INPUT_SIZE; x++) {
			uint8_t *pixel = input + (y * INPUT_SIZE + x) * 4;
			pixel[0] = x < INPUT_SIZE / 2 ? 255 : 0;
			pixel[1] = y < INPUT_SIZE / 2 ? 255 : 0;
			pixel[2] = x >= INPUT_SIZE / 2 && y >= INPUT_SIZE / 2 ? 255 : 0;
			pixel[3] = 255;
		}
	}
	CHECK(draw_shader(effect, "Performance", input, 0.0f, false, baseline), "bilinear baseline renders");
	for (size_t mode = 0; mode < 3; mode++) {
		CHECK(draw_shader(effect, techniques[mode], input, 1.0f, true, output), "split view renders");
		bool matches = true;
		for (size_t pixel = 0; pixel < OUTPUT_BYTES; pixel++)
			matches = matches && near_byte(output[pixel], baseline[pixel]);
		CHECK(matches, "split-position 100 percent matches bilinear baseline in every mode");
		CHECK(draw_shader(effect, techniques[mode], input, 0.0f, false, output), "quadrants render");
		const size_t corners[] = {0, (OUTPUT_SIZE - 1) * 4, (OUTPUT_SIZE - 1) * OUTPUT_SIZE * 4,
					  OUTPUT_BYTES - 4};
		const uint8_t expected[][4] = {
		    {255, 255, 0, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {0, 0, 255, 255}};
		for (size_t corner = 0; corner < 4; corner++)
			for (size_t channel = 0; channel < 4; channel++)
				CHECK(near_byte(output[corners[corner] + channel], expected[corner][channel]),
				      "source orientation and channel order are preserved");
	}
	/* Hard premultiplied alpha transitions must not gain bright fringes. */
	for (unsigned y = 0; y < INPUT_SIZE; y++) {
		for (unsigned x = 0; x < INPUT_SIZE; x++) {
			uint8_t *pixel = input + (y * INPUT_SIZE + x) * 4;
			pixel[3] = x < INPUT_SIZE / 2 ? 0 : 128;
			pixel[0] = pixel[3];
			pixel[1] = pixel[3] / 2;
			pixel[2] = 0;
		}
	}
	for (size_t mode = 0; mode < 3; mode++) {
		CHECK(draw_shader(effect, techniques[mode], input, 0, false, unsharpened), "alpha edge renders");
		CHECK(draw_shader(effect, techniques[mode], input, 1, false, output), "sharp alpha edge renders");
		bool premultiplied = true, stable_alpha = true;
		for (size_t pixel = 0; pixel < OUTPUT_BYTES; pixel += 4) {
			premultiplied = premultiplied && output[pixel] <= output[pixel + 3] &&
					output[pixel + 1] <= output[pixel + 3] &&
					output[pixel + 2] <= output[pixel + 3];
			stable_alpha = stable_alpha && output[pixel + 3] == unsharpened[pixel + 3];
		}
		CHECK(premultiplied, "premultiplied alpha invariant survives edge reconstruction");
		CHECK(stable_alpha, "sharpening does not alter alpha coverage");
	}
	gs_effect_destroy(effect);
}

struct synthetic_source {
	uint32_t width, height;
	bool hdr, scrgb;
	obs_source_t *owner;
};

static const char *synthetic_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Upscaler smoke source";
}

static void synthetic_update(void *opaque, obs_data_t *settings)
{
	struct synthetic_source *source = opaque;
	source->width = (uint32_t)obs_data_get_int(settings, "width");
	source->height = (uint32_t)obs_data_get_int(settings, "height");
	source->hdr = obs_data_get_bool(settings, "hdr");
	source->scrgb = obs_data_get_bool(settings, "scrgb");
}

static void *synthetic_create(obs_data_t *settings, obs_source_t *owner)
{
	struct synthetic_source *source = bzalloc(sizeof(*source));
	source->owner = owner;
	synthetic_update(source, settings);
	return source;
}

static void synthetic_destroy(void *opaque)
{
	bfree(opaque);
}
static uint32_t synthetic_width(void *opaque)
{
	return ((struct synthetic_source *)opaque)->width;
}
static uint32_t synthetic_height(void *opaque)
{
	return ((struct synthetic_source *)opaque)->height;
}

static enum gs_color_space synthetic_color_space(void *opaque, size_t count, const enum gs_color_space *preferred)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred);
	struct synthetic_source *source = opaque;
	return source->hdr ? (source->scrgb ? GS_CS_709_SCRGB : GS_CS_709_EXTENDED) : GS_CS_SRGB;
}

static void synthetic_render(void *opaque, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct synthetic_source *source = opaque;
	/* Real sources and upstream filters can query their parent's filtered size
	 * while being rendered. This catches a filter holding its mutex across
	 * reentrant OBS render calls (which would deadlock in get_width). */
	CHECK(obs_source_get_width(source->owner) > 0, "upstream render can query filtered dimensions");
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 color;
	if (source->hdr)
		vec4_set(&color, 4.0f, 2.0f, -0.25f, 0.5f);
	else
		vec4_set(&color, 0.25f, 0.5f, 0.75f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	/* A custom source owns its straight-to-premultiplied alpha blend state,
	 * including when OBS renders it inside a colour conversion texture. */
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(NULL, 0, source->width, source->height);
	gs_blend_state_pop();
}

/* Read floating-point pixels after the complete filter/OBS colour pipeline.
 * A 32F destination avoids quantizing our assertions; the filter's own HDR
 * capture still uses OBS's normal 16F texture format. */
static bool render_float_source(obs_source_t *source, enum gs_color_space space, float *pixel)
{
	gs_texrender_t *render = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(64, 36, GS_RGBA32F);
	bool ok = false;
	if (!render || !stage || !gs_texrender_begin_with_color_space(render, 64, 36, space))
		goto cleanup;
	struct vec4 clear = {0};
	gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
	gs_ortho(0, 64, 0, 36, -100, 100);
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(source);
	gs_blend_state_pop();
	gs_texrender_end(render);
	gs_stage_texture(stage, gs_texrender_get_texture(render));
	uint8_t *mapped = NULL;
	uint32_t stride = 0;
	if (gs_stagesurface_map(stage, &mapped, &stride)) {
		memcpy(pixel, mapped + 18 * stride + 32 * 4 * sizeof(float), 4 * sizeof(float));
		gs_stagesurface_unmap(stage);
		ok = true;
	}
cleanup:
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	return ok;
}

static void test_hdr(obs_source_t *source, obs_source_t *filter, obs_data_t *source_settings, obs_data_t *settings)
{
	obs_data_set_int(settings, "size", 3);
	obs_data_set_int(settings, "width", 64);
	obs_data_set_int(settings, "height", 36);
	obs_data_set_bool(source_settings, "hdr", true);
	for (int input = 0; input < 2; input++) {
		obs_data_set_bool(source_settings, "scrgb", input != 0);
		obs_source_update(source, source_settings);
		for (int mode = 0; mode < 4; mode++) {
			obs_data_set_int(settings, "mode", mode < 3 ? mode : 2);
			obs_data_set_double(settings, "sharpness", 1.0);
			obs_data_set_bool(settings, "split", mode == 3);
			obs_data_set_double(settings, "split_position", 1.0);
			obs_source_update(filter, settings);
			os_sleep_ms(60);
			CHECK(obs_source_get_width(source) == 64 && obs_source_get_height(source) == 36,
			      "HDR source is upscaled rather than bypassed");
			const enum gs_color_space outputs[] = {GS_CS_709_EXTENDED, GS_CS_709_SCRGB, GS_CS_SRGB_16F};
			for (size_t output = 0; output < 3; output++) {
				float pixel[4] = {0};
				obs_enter_graphics();
				bool read = render_float_source(source, outputs[output], pixel);
				obs_leave_graphics();
				CHECK(read, "HDR output reads back as floating-point data");
				CHECK(isfinite(pixel[0]) && isfinite(pixel[1]) && isfinite(pixel[2]),
				      "HDR processing/conversion produces finite colours");
				CHECK(fabsf(pixel[3] - 0.5f) < 0.005f, "HDR processing preserves translucent alpha");
				if (output < 2) {
					float multiplier = input == (int)output
							       ? 1.0f
							       : (input == 0 ? obs_get_video_sdr_white_level() / 80.0f
									     : 80.0f / obs_get_video_sdr_white_level());
					const bool correct = fabsf(pixel[0] - 2.0f * multiplier) < 0.015f &&
							     fabsf(pixel[1] - multiplier) < 0.015f &&
							     fabsf(pixel[2] + 0.125f * multiplier) < 0.015f;
					if (!correct)
						fprintf(stderr,
							"HDR input %d mode %d output %zu: %f %f %f %f, scale %f\n",
							input, mode, output, pixel[0], pixel[1], pixel[2], pixel[3],
							multiplier);
					CHECK(correct, "HDR brightness, negative wide-gamut components and white-level "
						       "conversion survive");
				} else {
					CHECK(pixel[0] > pixel[1] && pixel[0] <= 1.0f && pixel[0] > 0.1f,
					      "OBS tone maps HDR to SDR without blank or all-white output");
				}
			}
		}
	}
	obs_data_set_bool(settings, "split", false);
	obs_data_set_bool(source_settings, "hdr", false);
	obs_source_update(source, source_settings);
	obs_source_update(filter, settings);
	os_sleep_ms(60);
	CHECK(obs_source_get_width(source) == 64, "filter keeps upscaling after HDR to SDR source change");
}

static void test_plugin(const char *binary_path, const char *data_path)
{
	obs_module_t *module = NULL;
	int result = obs_open_module(&module, binary_path, data_path);
	CHECK(result == MODULE_SUCCESS, "libobs opens the plugin binary");
	if (result != MODULE_SUCCESS)
		return;
	bool initialized = obs_init_module(module);
	CHECK(initialized, "plugin initializes");
	if (!initialized)
		return;
	struct obs_source_info source_info = {
	    .id = "upscaler_smoke_source",
	    .type = OBS_SOURCE_TYPE_INPUT,
	    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	    .get_name = synthetic_name,
	    .create = synthetic_create,
	    .destroy = synthetic_destroy,
	    .update = synthetic_update,
	    .get_width = synthetic_width,
	    .get_height = synthetic_height,
	    .video_render = synthetic_render,
	    .video_get_color_space = synthetic_color_space,
	};
	obs_register_source(&source_info);
	obs_data_t *source_settings = obs_data_create();
	obs_data_set_int(source_settings, "width", 32);
	obs_data_set_int(source_settings, "height", 18);
	obs_source_t *source = obs_source_create_private("upscaler_smoke_source", "Smoke source", source_settings);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "size", 3);
	obs_data_set_int(settings, "width", 64);
	obs_data_set_int(settings, "height", 36);
	obs_data_set_bool(settings, "adaptive", false);
	obs_source_t *filter = obs_source_create_private("obs_upscaler_filter", "Upscaler", settings);
	CHECK(source && filter, "synthetic source and upscaler filter are created");
	if (!source || !filter)
		goto cleanup;
	obs_source_filter_add(source, filter);
	os_sleep_ms(100);
	for (int mode = 0; mode < 3; mode++) {
		obs_data_set_int(settings, "mode", mode);
		obs_source_update(filter, settings);
		os_sleep_ms(100);
		CHECK(obs_source_get_width(source) == 64 && obs_source_get_height(source) == 36,
		      "filter advertises the enlarged dimensions");
		obs_enter_graphics();
		gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		bool rendered = render && gs_texrender_begin(render, 64, 36);
		CHECK(rendered, "plugin output render target created");
		if (rendered) {
			struct vec4 clear = {0};
			gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
			gs_ortho(0, 64, 0, 36, -100, 100);
			obs_source_video_render(source);
			gs_texrender_end(render);
			uint8_t output[64 * 36 * 4];
			bool read = read_texture(gs_texrender_get_texture(render), 64, 36, output);
			CHECK(read, "plugin output reads back");
			if (read) {
				const uint8_t *center = output + (18 * 64 + 32) * 4;
				CHECK(center[0] > 20 && center[1] > center[0] && center[2] > center[1] &&
					  center[3] == 255,
				      "plugin produces opaque nonblank source content in every mode");
			}
		}
		gs_texrender_destroy(render);
		obs_leave_graphics();
	}
	obs_data_set_int(settings, "size", 0);
	obs_source_update(filter, settings);
	os_sleep_ms(100);
	CHECK(obs_source_get_width(source) == 3840 && obs_source_get_height(source) == 2160,
	      "4K preset reports 3840x2160 for a 16:9 source");
	test_hdr(source, filter, source_settings, settings);
	obs_data_set_int(settings, "size", 3);
	obs_data_set_int(settings, "width", 100000);
	obs_source_update(filter, settings);
	os_sleep_ms(100);
	CHECK(obs_source_get_width(source) == 32, "unsafe allocation request safely bypasses");
	obs_source_filter_remove(source, filter);
cleanup:
	obs_source_release(filter);
	obs_source_release(source);
	obs_data_release(settings);
	obs_data_release(source_settings);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <plugin-binary> <plugin-data-directory> [graphics-module]\n", argv[0]);
		return 2;
	}
	if (argc > 4) {
		const char *actual = obs_get_version_string();
		size_t length = strlen(argv[4]);
		if (strncmp(actual, argv[4], length) != 0 || (actual[length] != '\0' && actual[length] != '-')) {
			fprintf(stderr, "FAIL: expected runtime %s, loaded %s\n", argv[4], actual);
			return 1;
		}
	}
	printf("Testing OBS runtime %s\n", obs_get_version_string());
	if (!obs_startup("en-US", NULL, NULL)) {
		fputs("FAIL: obs_startup\n", stderr);
		return 1;
	}
#ifdef _WIN32
	const char *graphics = "libobs-d3d11";
#else
	const char *graphics = "libobs-opengl";
#endif
	struct obs_video_info video = {
	    .graphics_module = argc > 3 ? argv[3] : graphics,
	    .fps_num = 60,
	    .fps_den = 1,
	    .base_width = 128,
	    .base_height = 72,
	    .output_width = 128,
	    .output_height = 72,
	    .output_format = VIDEO_FORMAT_RGBA,
	    .colorspace = VIDEO_CS_709,
	    .range = VIDEO_RANGE_FULL,
	    .scale_type = OBS_SCALE_BILINEAR,
	};
#ifdef _WIN32
	video.adapter = test_adapter();
#endif
	int result = obs_reset_video(&video);
	if (result != OBS_VIDEO_SUCCESS) {
		fprintf(stderr,
			"FAIL: graphics initialization (%d); Linux needs DISPLAY / xvfb-run; "
			"Windows needs a D3D11-capable DXGI adapter and OBS runtime dependencies.\n",
			result);
		obs_shutdown();
		return 1;
	}
	obs_set_video_levels(300.0f, 1000.0f);
	obs_enter_graphics();
	test_shaders(argv[2]);
	obs_leave_graphics();
	test_plugin(argv[1], argv[2]);
	struct obs_video_info after;
	CHECK(obs_get_video_info(&after) && after.fps_num == 60 && after.fps_den == 1,
	      "plugin does not alter the OBS frame rate");
	obs_shutdown();
	if (failures) {
		fprintf(stderr, "%u smoke check(s) failed\n", failures);
		return 1;
	}
	puts("PASS: shader compilation/readback, all presets, HDR/SDR conversion, alpha, split view, module load, "
	     "resize and safe bypass");
	return 0;
}
