#include <obs-module.h>
#include "flutter_embedder.h"
#include <graphics/graphics.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

struct flutter_source {
	obs_source_t *source;
	FlutterEngine engine;
	uint8_t *pixel_data;
	uint32_t width, height;
	gs_texture_t *texture;
	FlutterEngineAOTData aot_data;
	volatile LONG dirty_pixels;

	// --- Для потока движка и раннера ---
	HANDLE engine_thread;
	DWORD engine_thread_id;
	HANDLE engine_ready_event;
	// для custom_task_runner
};

typedef struct {
	FlutterTask task_queue[128];
	uint64_t time_queue[128];
	int head, tail;
	CRITICAL_SECTION lock;
	HANDLE sem;
	volatile bool running;
	DWORD thread_id;
	struct flutter_source *ctx;
} flutter_task_runner_t;

static DWORD WINAPI flutter_engine_thread_proc(LPVOID param);

static bool my_surface_present_callback(void *user_data, const void *allocation, size_t row_bytes, size_t height)
{
	struct flutter_source *ctx = user_data;
	if (!ctx->pixel_data)
		return false;
	memcpy(ctx->pixel_data, allocation, row_bytes * height);
	InterlockedExchange(&ctx->dirty_pixels, 1);
	return true;
}

static void log_message_callback(const char *tag, const char *message, void *user_data)
{
	blog(LOG_INFO, "[Flutter] [%s] %s", tag ? tag : "no-tag", message ? message : "(null)");
}

static void platform_message_callback(const FlutterPlatformMessage *message, void *user_data)
{
	blog(LOG_INFO, "[Flutter] platform_message_callback");
	const struct flutter_source *ctx = user_data;
	if (message->response_handle) {
		FlutterEngineSendPlatformMessageResponse(ctx->engine, message->response_handle, NULL, 0);
	}
}

static void get_assets_paths(wchar_t *assets_path, wchar_t *icu_path, wchar_t *aot_path)
{
	HMODULE hModule = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&get_assets_paths, &hModule);

	wchar_t dll_path[MAX_PATH];
	GetModuleFileNameW(hModule, dll_path, MAX_PATH);
	PathRemoveFileSpecW(dll_path);

	wchar_t base_folder[MAX_PATH];
	wsprintfW(base_folder, L"%s\\flutter_template", dll_path);

	wsprintfW(assets_path, L"%s\\flutter_assets", base_folder);
	wsprintfW(icu_path, L"%s\\icudtl.dat", base_folder);
	wsprintfW(aot_path, L"%s\\app.so", base_folder);
}

// --- Custom Task Runner (per flutter_source) ---

bool my_runs_task_on_current_thread(void *user_data)
{
	flutter_task_runner_t *runner = (flutter_task_runner_t *)user_data;
	return GetCurrentThreadId() == runner->thread_id;
}

void my_post_task(FlutterTask task, uint64_t target_time_nanos, void *user_data)
{
	flutter_task_runner_t *runner = (flutter_task_runner_t *)user_data;
	EnterCriticalSection(&runner->lock);
	int next = (runner->tail + 1) % 128;
	if (next != runner->head) {
		runner->task_queue[runner->tail] = task;
		runner->time_queue[runner->tail] = target_time_nanos;
		runner->tail = next;
		ReleaseSemaphore(runner->sem, 1, NULL);
	}
	LeaveCriticalSection(&runner->lock);
}

DWORD WINAPI flutter_engine_thread_proc(LPVOID param)
{
	struct flutter_source *context = (struct flutter_source *)param;

	// --- Task Runner (живет в этом же потоке) ---
	flutter_task_runner_t runner = { 0 };
	InitializeCriticalSection(&runner.lock);
	runner.sem = CreateSemaphore(NULL, 0, 128, NULL);
	runner.head = runner.tail = 0;
	runner.running = true;
	runner.thread_id = GetCurrentThreadId();
	runner.ctx = context;

	// ---- Custom Task Runner для platform thread ----
	FlutterTaskRunnerDescription platform_task_runner = { 0 };
	platform_task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
	platform_task_runner.user_data = &runner;
	platform_task_runner.runs_task_on_current_thread_callback = my_runs_task_on_current_thread;
	platform_task_runner.post_task_callback = my_post_task;

	FlutterCustomTaskRunners custom_runners = { 0 };
	custom_runners.struct_size = sizeof(FlutterCustomTaskRunners);
	custom_runners.platform_task_runner = &platform_task_runner;

	context->pixel_data = (uint8_t*)malloc(context->width * context->height * 4);
	memset(context->pixel_data, 0, context->width * context->height * 4);

	wchar_t assets_path_w[MAX_PATH], icu_path_w[MAX_PATH], aot_path_w[MAX_PATH];
	get_assets_paths(assets_path_w, icu_path_w, aot_path_w);

	char assets_path[MAX_PATH], icu_path[MAX_PATH], aot_path[MAX_PATH];
	WideCharToMultiByte(CP_UTF8, 0, assets_path_w, -1, assets_path, MAX_PATH, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, icu_path_w, -1, icu_path, MAX_PATH, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, aot_path_w, -1, aot_path, MAX_PATH, NULL, NULL);

	FlutterSoftwareRendererConfig software_config = { 0 };
	software_config.struct_size = sizeof(FlutterSoftwareRendererConfig);
	software_config.surface_present_callback = my_surface_present_callback;

	FlutterRendererConfig renderer_config = { 0 };
	renderer_config.type = kSoftware;
	renderer_config.software = software_config;

	blog(LOG_INFO, "[FlutterSource] PreConfig (thread=%lu)", (unsigned long)GetCurrentThreadId());

	FlutterProjectArgs project_args = { 0 };
	project_args.struct_size = sizeof(FlutterProjectArgs);

	project_args.assets_path = assets_path;
	project_args.icu_data_path = icu_path;

	static const char* engine_argv[] = { "obs_flutter", "--verbose-logging" };
	project_args.command_line_argc = _countof(engine_argv);
	project_args.command_line_argv = engine_argv;

	project_args.log_message_callback = log_message_callback;
	project_args.platform_message_callback = platform_message_callback;

	FlutterEngineAOTDataSource aot_source = {
		.type = kFlutterEngineAOTDataSourceTypeElfPath,
		.elf_path = aot_path,
	};

	FlutterEngineAOTData aot_data = NULL;
	if (FlutterEngineCreateAOTData(&aot_source, &aot_data) != kSuccess) {
		blog(LOG_ERROR, "Failed to load AOT data");
		SetEvent(context->engine_ready_event);
		return 0;
	}

	project_args.aot_data = aot_data;
	project_args.custom_task_runners = &custom_runners;
	context->aot_data = aot_data;

	blog(LOG_INFO, "[FlutterSource] PreRun (thread=%lu)", (unsigned long)GetCurrentThreadId());

	FlutterEngineResult result =
		FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, context, &context->engine);

	SetEvent(context->engine_ready_event);

	if (result != kSuccess) {
		blog(LOG_ERROR, "[FlutterSource] FlutterEngineRun failed: %d", result);
		return 0;
	}

	blog(LOG_INFO, "[FlutterSource] FlutterEngineRun OK: %d", result);

	FlutterWindowMetricsEvent window_event = { 0 };
	window_event.struct_size = sizeof(FlutterWindowMetricsEvent);
	window_event.width = context->width;
	window_event.height = context->height;
	window_event.pixel_ratio = 1.0f;
	FlutterEngineSendWindowMetricsEvent(context->engine, &window_event);

	FlutterEngineScheduleFrame(context->engine);

	blog(LOG_INFO, "[FlutterSource] Sent WindowMetricsEvent and ScheduleFrame");

	// Главный task runner loop для обработки platform задач
	while (runner.running) {
		WaitForSingleObject(runner.sem, INFINITE);

		while (1) {
			EnterCriticalSection(&runner.lock);
			if (runner.head == runner.tail) {
				LeaveCriticalSection(&runner.lock);
				break;
			}
			FlutterTask task = runner.task_queue[runner.head];
			uint64_t target_time = runner.time_queue[runner.head];
			runner.head = (runner.head + 1) % 128;
			LeaveCriticalSection(&runner.lock);

			uint64_t now = (uint64_t)GetTickCount64() * 1000000ull;
			if (target_time > now)
				Sleep((DWORD)((target_time - now) / 1000000ull));
			FlutterEngineRunTask(NULL, &task);

			blog(LOG_INFO, "[FlutterSource] Task Executed");
		}
	}

	DeleteCriticalSection(&runner.lock);
	CloseHandle(runner.sem);

	return 0;
}

// --- OBS Source glue ---

static const char* flutter_source_get_name(void* unused)
{
	UNUSED_PARAMETER(unused);
	return "Flutter Source";
}

static void* flutter_source_create(obs_data_t* settings, obs_source_t* source)
{
	struct flutter_source* context = bzalloc(sizeof(struct flutter_source));
	context->source = source;
	context->width = (uint32_t)obs_data_get_int(settings, "width");
	context->height = (uint32_t)obs_data_get_int(settings, "height");
	if (context->width == 0)
		context->width = 320;
	if (context->height == 0)
		context->height = 240;

	context->engine_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);

	// Запуск движка в отдельном потоке
	context->engine_thread = CreateThread(NULL, 0, flutter_engine_thread_proc, context, 0, &context->engine_thread_id);

	// Ждём готовности движка
	WaitForSingleObject(context->engine_ready_event, INFINITE);
	CloseHandle(context->engine_ready_event);

	return context;
}

static void flutter_source_destroy(void* data)
{
	struct flutter_source* context = data;
	blog(LOG_INFO, "[FlutterSource] DESTROY called");

	if (context->engine) {
		const FlutterEngineResult res = FlutterEngineShutdown(context->engine);
		blog(LOG_INFO, "[FlutterSource] FlutterEngineShutdown result: %d", res);
		context->engine = NULL;
	}

	if (context->aot_data) {
		FlutterEngineCollectAOTData(context->aot_data);
		context->aot_data = NULL;
	}

	if (context->texture) {
		gs_texture_destroy(context->texture);
		context->texture = NULL;
	}

	if (context->pixel_data) {
		free(context->pixel_data);
		context->pixel_data = NULL;
	}

	if (context->engine_thread) {
		// Thread will auto-exit after engine shutdown
		CloseHandle(context->engine_thread);
	}

	bfree(context);

	blog(LOG_INFO, "[FlutterSource] DESTROY ended");
}

static void flutter_source_render(void* data, const gs_effect_t* effect)
{
	struct flutter_source* ctx = data;
	if (!ctx->texture) {
		ctx->texture = gs_texture_create(ctx->width, ctx->height, GS_BGRA, 1, NULL, GS_DYNAMIC);
	}
	if (InterlockedCompareExchange(&ctx->dirty_pixels, 0, 1) == 1) {
		gs_texture_set_image(ctx->texture, ctx->pixel_data, ctx->width * 4, false);
	}
	if (!ctx->texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_eparam_t* param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, ctx->texture);
	gs_draw_sprite(ctx->texture, 0, ctx->width, ctx->height);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

static uint32_t flutter_source_get_width(const void* data)
{
	const struct flutter_source* ctx = data;
	return ctx->width;
}

static uint32_t flutter_source_get_height(const void* data)
{
	const struct flutter_source* ctx = data;
	return ctx->height;
}

static obs_properties_t* flutter_source_properties(void* data)
{
	obs_properties_t* props = obs_properties_create();
	obs_properties_add_int(props, "width", "Width", 320, 3840, 1);
	obs_properties_add_int(props, "height", "Height", 240, 2160, 1);
	return props;
}

static void flutter_source_update(void* data, obs_data_t* settings)
{
	struct flutter_source* ctx = data;
	uint32_t new_width = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t new_height = (uint32_t)obs_data_get_int(settings, "height");

	if (new_width == 0)
		new_width = 320;
	if (new_height == 0)
		new_height = 240;

	if (ctx->width == new_width && ctx->height == new_height)
		return;

	if (ctx->texture) {
		gs_texture_destroy(ctx->texture);
		ctx->texture = NULL;
	}
	if (ctx->pixel_data) {
		free(ctx->pixel_data);
		ctx->pixel_data = NULL;
	}

	ctx->width = new_width;
	ctx->height = new_height;

	ctx->pixel_data = (uint8_t*)malloc(ctx->width * ctx->height * 4);
	memset(ctx->pixel_data, 0, ctx->width * ctx->height * 4);

	if (ctx->engine) {
		FlutterWindowMetricsEvent window_event = { 0 };
		window_event.struct_size = sizeof(FlutterWindowMetricsEvent);
		window_event.width = ctx->width;
		window_event.height = ctx->height;
		window_event.pixel_ratio = 1.0f;
		FlutterEngineSendWindowMetricsEvent(ctx->engine, &window_event);

		FlutterEngineScheduleFrame(ctx->engine);
	}
}

struct obs_source_info flutter_source_info = {
	.id = "flutter_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = flutter_source_get_name,
	.create = flutter_source_create,
	.destroy = flutter_source_destroy,
	.video_render = flutter_source_render,
	.get_width = flutter_source_get_width,
	.get_height = flutter_source_get_height,
	.update = flutter_source_update,
	.get_properties = flutter_source_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA
};
