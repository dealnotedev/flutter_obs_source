#include "flutter-renderer.h"
#include <obs.h>
#include <windows.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <string>
#include <vector>

static void *preloaded_gl_clear = nullptr;

static void require(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void publish(flutter_renderer *renderer, uint32_t width, uint32_t height)
{
	const auto config = flutter_renderer_config(renderer);
	if (config.type == kSoftware) {
		/* Padded rows exercise pitch handling. The pattern uses premultiplied alpha. */
		const size_t pitch = width * 4 + 16;
		std::vector<uint8_t> pixels(pitch * height, 0);
		for (uint32_t y = 0; y < height; ++y) {
			for (uint32_t x = 0; x < width; ++x) {
				auto *pixel = pixels.data() + y * pitch + x * 4;
				if (x < width / 2 && y < height / 2) {
					pixel[2] = pixel[3] = 255;
				} else if (x >= width / 2 && y >= height / 2) {
					pixel[0] = pixel[3] = 128;
				}
			}
		}
		require(config.software.surface_present_callback(renderer, pixels.data(), pitch, height),
			"software present");
	} else {
		const auto &gl = config.open_gl;
		require(gl.make_current(renderer), "make render context current");
		FlutterFrameInfo info = {};
		info.struct_size = sizeof(info);
		info.size = {width, height};
		gl.fbo_with_frame_info_callback(renderer, &info);
#define GL_PROC(type, variable, name)                                                \
	auto variable = reinterpret_cast<type>(gl.gl_proc_resolver(renderer, name)); \
	require(variable != nullptr, name)
		GL_PROC(decltype(&glViewport), viewport, "glViewport");
		GL_PROC(decltype(&glDisable), disable, "glDisable");
		GL_PROC(decltype(&glEnable), enable, "glEnable");
		GL_PROC(decltype(&glScissor), scissor, "glScissor");
		GL_PROC(decltype(&glClearColor), clear_color, "glClearColor");
		GL_PROC(decltype(&glClear), clear, "glClear");
#undef GL_PROC
		require(reinterpret_cast<void *>(clear) != preloaded_gl_clear,
			"private ANGLE entry points stay separate from preloaded OBS ANGLE");
		viewport(0, 0, width, height);
		disable(GL_SCISSOR_TEST);
		clear_color(0, 0, 0, 0);
		clear(GL_COLOR_BUFFER_BIT);
		enable(GL_SCISSOR_TEST);
		scissor(0, height / 2, width / 2, height / 2);
		clear_color(1, 0, 0, 1);
		clear(GL_COLOR_BUFFER_BIT);
		scissor(width / 2, 0, width / 2, height / 2);
		clear_color(0, 0, 0.5f, 0.5f);
		clear(GL_COLOR_BUFFER_BIT);
		disable(GL_SCISSOR_TEST);
		require(gl.present(renderer), "GPU present");
		require(gl.clear_current(renderer), "release render context");
	}
}

static bool capture(flutter_renderer *renderer, uint32_t width, uint32_t height, bool quiet = false,
		    obs_source_t *source = nullptr)
{
	obs_enter_graphics();
	auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	auto *stage = gs_stagesurface_create(width, height, GS_RGBA);
	require(target && stage, "capture resources");
	require(gs_texrender_begin(target, width, height), "begin capture");
	struct vec4 transparent = {};
	gs_clear(GS_CLEAR_COLOR, &transparent, 0, 0);
	gs_ortho(0, float(width), 0, float(height), -100, 100);
	gs_matrix_identity();
	if (source) {
		obs_source_video_render(source);
	} else {
		auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		while (gs_effect_loop(effect, "Draw"))
			flutter_renderer_render(renderer, effect);
	}
	gs_texrender_end(target);
	gs_stage_texture(stage, gs_texrender_get_texture(target));
	uint8_t *pixels = nullptr;
	uint32_t pitch = 0;
	require(gs_stagesurface_map(stage, &pixels, &pitch), "map captured pixels");
	const auto *red = pixels + 2 * pitch + 2 * 4;
	const auto *empty = pixels + 2 * pitch + (width - 3) * 4;
	const auto *blue = pixels + (height - 3) * pitch + (width - 3) * 4;
	const bool correct = red[0] >= 250 && red[1] == 0 && red[2] == 0 && red[3] == 255 && empty[0] == 0 &&
			     empty[1] == 0 && empty[2] == 0 && empty[3] == 0 && blue[0] == 0 && blue[1] == 0 &&
			     blue[2] >= 120 && blue[3] >= 127 && blue[3] <= 129;
	if (!correct && !quiet)
		fprintf(stderr, "pixels: red=%u,%u,%u,%u empty=%u,%u,%u,%u blue=%u,%u,%u,%u\n", red[0], red[1], red[2],
			red[3], empty[0], empty[1], empty[2], empty[3], blue[0], blue[1], blue[2], blue[3]);
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	obs_leave_graphics();
	return correct;
}

static void test_sources()
{
	auto *settings = obs_data_create();
	obs_data_set_int(settings, "width", 320);
	obs_data_set_int(settings, "height", 240);
	auto *software = obs_source_create("flutter_source", "Software fixture", settings, nullptr);
	auto *gpu = obs_source_create("flutter_source_gpu", "GPU fixture", settings, nullptr);
	require(software && gpu, "create both registered sources simultaneously");
	for (const auto size : {std::pair<uint32_t, uint32_t>{320, 240}, {640, 480}, {320, 240}}) {
		const auto [width, height] = size;
		obs_data_set_int(settings, "width", width);
		obs_data_set_int(settings, "height", height);
		for (auto *source : {software, gpu}) {
			obs_source_update(source, settings);
			bool correct = false;
			for (int i = 0; i < 500 && !correct; ++i) {
				correct = capture(nullptr, width, height, true, source);
				if (!correct)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			require(correct, "registered source renders real Flutter after update");
		}
	}
	obs_source_release(gpu);
	obs_source_release(software);
	obs_data_release(settings);
	printf("PASS: both OBS sources coexist, exchange config messages, render, resize and shut down\n");
}

static void test_renderer(flutter_renderer_type type)
{
	int owner = 42;
	auto *renderer = flutter_renderer_create(type, 32, 24, &owner);
	require(renderer != nullptr, "create renderer");
	require(flutter_renderer_owner(renderer) == &owner, "engine callback owner");
	for (const auto size : {std::pair<uint32_t, uint32_t>{32, 24}, {64, 48}, {32, 24}}) {
		const auto [width, height] = size;
		flutter_renderer_resize(renderer, width, height);
		/* Saturate the queue before consuming: the newest completed frame must survive. */
		for (int i = 0; i < 20; ++i)
			publish(renderer, width, height);
		require(capture(renderer, width, height), "pixels, orientation and alpha after resize/drop");
		require(capture(renderer, width, height), "last frame persists when Flutter is idle");
	}
	std::atomic<bool> done = false;
	std::thread producer([&] {
		for (int i = 0; i < 200; ++i)
			publish(renderer, 32, 24);
		done = true;
	});
	while (!done) {
		obs_enter_graphics();
		flutter_renderer_render(renderer, nullptr);
		obs_leave_graphics();
		std::this_thread::yield();
	}
	producer.join();
	require(capture(renderer, 32, 24), "concurrent frame transfer");
	if (type == FLUTTER_RENDERER_GPU) {
		const auto gl = flutter_renderer_config(renderer).open_gl;
		std::thread io([&] {
			require(gl.make_resource_current(renderer), "shared IO context");
			require(gl.clear_current(renderer), "release IO context");
		});
		io.join();
	}
	flutter_renderer_destroy(renderer);
	printf("PASS: %s renderer transfer, alpha, resize, dropped frames, threads, cleanup\n",
	       type == FLUTTER_RENDERER_GPU ? "GPU" : "software");
}

int main(int argc, char **argv)
{
	require(argc >= 4,
		"usage: renderer_transfer_tests GRAPHICS_DLL OBS_DATA_DIR PLUGIN_DLL [--engine] [--preload-angle DIR]");
	bool engine = false;
	std::vector<HMODULE> preloaded;
	decltype(&eglGetProcAddress) preloaded_get_proc = nullptr;
	for (int i = 4; i < argc; ++i) {
		const std::string option = argv[i];
		if (option == "--engine") {
			engine = true;
		} else if (option == "--preload-angle" && i + 1 < argc) {
			const auto directory = std::filesystem::absolute(argv[++i]);
			for (const auto *name :
			     {L"libGLESv2.dll", L"libEGL.dll", L"d3dcompiler_47.dll", L"vulkan-1.dll"}) {
				const auto path = directory / name;
				if (!std::filesystem::exists(path)) {
					require(std::wstring(name) != L"libGLESv2.dll" &&
							std::wstring(name) != L"libEGL.dll",
						"both preloaded ANGLE libraries exist");
					continue;
				}
				auto module = LoadLibraryExW(path.c_str(), nullptr,
							     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
								     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				require(module != nullptr, "preload existing OBS library");
				preloaded.push_back(module);
				if (std::wstring(name) == L"libEGL.dll")
					preloaded_get_proc = reinterpret_cast<decltype(preloaded_get_proc)>(
						GetProcAddress(module, "eglGetProcAddress"));
			}
			require(preloaded_get_proc != nullptr, "preloaded EGL resolver");
			preloaded_gl_clear = reinterpret_cast<void *>(preloaded_get_proc("glClear"));
			require(preloaded_gl_clear != nullptr, "preloaded GLES entry point");
		} else {
			require(false, "unknown test option");
		}
	}
	require(obs_startup("en-US", nullptr, nullptr), "OBS startup");
	const std::string data_path = std::string(argv[2]) + "/";
	obs_add_data_path(data_path.c_str());
	obs_video_info video = {};
	video.graphics_module = argv[1];
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.output_width = 64;
	video.base_height = video.output_height = 64;
	video.output_format = VIDEO_FORMAT_RGBA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	video.scale_type = OBS_SCALE_BILINEAR;
	require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "OBS D3D11 startup");
	obs_module_t *module = nullptr;
	require(obs_open_module(&module, argv[3], "") == MODULE_SUCCESS && obs_init_module(module),
		"load source plugin");
	require(std::string(obs_source_get_display_name("flutter_source")) == "Freydis Overlay (Software)",
		"register software source with its existing scene ID");
	require(std::string(obs_source_get_display_name("flutter_source_gpu")) == "Freydis Overlay (GPU)",
		"register separate GPU source");
	test_renderer(FLUTTER_RENDERER_SOFTWARE);
	test_renderer(FLUTTER_RENDERER_GPU);
	if (engine) {
		test_sources();
	}
	obs_shutdown();
	if (preloaded_get_proc) {
		require(reinterpret_cast<void *>(preloaded_get_proc("glClear")) == preloaded_gl_clear,
			"preloaded ANGLE remains usable after private renderers shut down");
		printf("PASS: private ANGLE coexists with preloaded OBS libraries\n");
	}
	for (auto loaded = preloaded.rbegin(); loaded != preloaded.rend(); ++loaded)
		FreeLibrary(*loaded);
	return 0;
}
