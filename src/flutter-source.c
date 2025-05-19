/*
 * flutter_source_custom_task_runners.c
 * -----------------------------------
 * OBS Studio source plug‑in that embeds the Flutter engine on Windows.
 * The engine runs on a dedicated worker thread and communicates with OBS
 * via a software texture.  A custom platform task‑runner is used so that
 * all platform messages are executed on the same worker thread.
 *
 * Build:  drop this file into an OBS plug‑in project and link against
 *         the Flutter Embedder DLL + libobs.
 */

//  ────────────────   Standard library / Windows   ────────────────
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

//  ────────────────   OBS & Flutter headers   ────────────────
#include <obs-module.h>
#include <graphics/graphics.h>
#include "flutter_embedder.h"

//  ────────────────────────────────────────────────────────────────
//  Worker‑thread infrastructure
//  ────────────────────────────────────────────────────────────────

typedef enum {
	CMD_CREATE_ENGINE,
	CMD_DESTROY_ENGINE,
	CMD_RUN_ENGINE_TASK, // Execute a pending FlutterTask
	CMD_EXIT,
} command_type_t;

struct flutter_source; // forward declaration

typedef struct {
	command_type_t type;
	struct flutter_source *ctx; // source instance owner
	FlutterTask task;           // used by CMD_RUN_ENGINE_TASK
	uint64_t target_time_ns;    //   "    "
	HANDLE done_event;          // event to signal when cmd is done
} command_t;

#define QUEUE_CAPACITY 128

typedef struct {
	command_t items[QUEUE_CAPACITY];
	int head, tail; // ring‑buffer indexes
	CRITICAL_SECTION cs;
	HANDLE sem; // counts pending commands
} command_queue_t;

static command_queue_t g_queue;
static HANDLE g_worker_thread = NULL;
static DWORD g_worker_tid = 0;  // thread id
static LONG g_source_count = 0; // active sources

// ––– queue helpers ––––––––––––––––––––––––––––––––––––––––––––
static void queue_init(command_queue_t *q)
{
	InitializeCriticalSection(&q->cs);
	q->sem = CreateSemaphore(NULL, 0, QUEUE_CAPACITY, NULL);
	q->head = q->tail = 0;
}

static void queue_destroy(command_queue_t *q)
{
	DeleteCriticalSection(&q->cs);
	CloseHandle(q->sem);
}

static void queue_push(command_queue_t *q, const command_t *cmd)
{
	EnterCriticalSection(&q->cs);
	q->items[q->tail] = *cmd;
	q->tail = (q->tail + 1) % QUEUE_CAPACITY;
	LeaveCriticalSection(&q->cs);
	ReleaseSemaphore(q->sem, 1, NULL);
}

static bool queue_pop(command_queue_t *q, command_t *out)
{
	WaitForSingleObject(q->sem, INFINITE);

	EnterCriticalSection(&q->cs);
	if (q->head == q->tail) {
		LeaveCriticalSection(&q->cs);
		return false;
	}
	*out = q->items[q->head];
	q->head = (q->head + 1) % QUEUE_CAPACITY;
	LeaveCriticalSection(&q->cs);
	return true;
}

//  ────────────────────────────────────────────────────────────────
//  OBS <‑‑> Flutter source structure
//  ────────────────────────────────────────────────────────────────

struct flutter_source {
	// OBS data
	obs_source_t *source;

	// Flutter data
	FlutterEngine engine;
	FlutterEngineAOTData aot_data;
	uint32_t width, height;
	uint8_t *pixel_data; // RGBA buffer sent to texture
	gs_texture_t *texture;
	volatile LONG dirty_pixels;

	// custom task‑runner bookkeeping
	DWORD engine_tid; // worker thread id
	FlutterTaskRunnerDescription platform_runner_desc;
	FlutterCustomTaskRunners custom_runners;
};

//  ────────────────────────────────────────────────────────────────
//  Forward declarations
//  ────────────────────────────────────────────────────────────────
static void engine_init(struct flutter_source *ctx);
static void engine_shutdown(struct flutter_source *ctx);

//  ────────────────────────────────────────────────────────────────
//  Logging helpers
//  ────────────────────────────────────────────────────────────────
static inline void log_tid(const char *tag)
{
	blog(LOG_INFO, "[%s] tid=%lu", tag, (unsigned long)GetCurrentThreadId());
}

//  ────────────────────────────────────────────────────────────────
//  Flutter embedder callbacks
//  ────────────────────────────────────────────────────────────────

static bool surface_present_cb(void *user_data, const void *allocation, size_t row_bytes, size_t height)
{
	struct flutter_source *ctx = user_data;
	if (!ctx->pixel_data)
		return false;
	memcpy(ctx->pixel_data, allocation, row_bytes * height);
	InterlockedExchange(&ctx->dirty_pixels, 1);
	return true;
}

static void log_message_cb(const char *tag, const char *msg, void *user_data)
{
	(void)user_data;
	log_tid("log_message");
	blog(LOG_INFO, "[Flutter] [%s] %s", tag ? tag : "no‑tag", msg ? msg : "(null)");
}

static void platform_message_cb(const FlutterPlatformMessage *msg, void *user_data)
{
	struct flutter_source *ctx = user_data;
	log_tid("platform_message");

	// Echo an empty success reply so Dart side can await the call safely
	if (msg->response_handle) {
		FlutterEngineSendPlatformMessageResponse(ctx->engine, msg->response_handle, NULL, 0);
	}
}

//  ────────────────────────────────────────────────────────────────
//  Task‑runner helpers (called by Flutter)
//  ────────────────────────────────────────────────────────────────

static bool runs_on_worker_thread(void *user_data)
{
	struct flutter_source *ctx = user_data;
	return GetCurrentThreadId() == ctx->engine_tid;
}

static bool post_task_to_worker(FlutterTask task, uint64_t target_time_ns, void *user_data)
{
	struct flutter_source *ctx = user_data;
	command_t cmd = {
		.type = CMD_RUN_ENGINE_TASK,
		.ctx = ctx,
		.task = task,
		.target_time_ns = target_time_ns,
		.done_event = NULL,
	};
	queue_push(&g_queue, &cmd);
	return true;
}

//  ────────────────────────────────────────────────────────────────
//  Worker thread main procedure
//  ────────────────────────────────────────────────────────────────

static DWORD WINAPI worker_thread_fn(LPVOID param)
{
	(void)param;
	g_worker_tid = GetCurrentThreadId();
	log_tid("worker_started");

	command_t cmd;
	while (queue_pop(&g_queue, &cmd)) {
		switch (cmd.type) {
		case CMD_CREATE_ENGINE:
			engine_init(cmd.ctx);
			break;
		case CMD_DESTROY_ENGINE:
			engine_shutdown(cmd.ctx);
			break;
		case CMD_RUN_ENGINE_TASK: {
			uint64_t now = FlutterEngineGetCurrentTime();
			if (now < cmd.target_time_ns) {
				DWORD sleep_ms = (DWORD)((cmd.target_time_ns - now) / 1000000ULL);
				if (sleep_ms)
					Sleep(sleep_ms);
			}
			if (cmd.ctx && cmd.ctx->engine)
				FlutterEngineRunTask(cmd.ctx->engine, &cmd.task);
			break;
		}
		case CMD_EXIT:
			if (cmd.done_event)
				SetEvent(cmd.done_event);
			return 0;
		}
		if (cmd.done_event)
			SetEvent(cmd.done_event);
	}
	return 0;
}

static void ensure_worker_thread(void)
{
	if (!g_worker_thread) {
		queue_init(&g_queue);
		g_worker_thread = CreateThread(NULL, 0, worker_thread_fn, NULL, 0, &g_worker_tid);
	}
}

static void stop_worker_thread(void)
{
	if (g_worker_thread) {
		HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
		command_t cmd = {.type = CMD_EXIT, .done_event = done};
		queue_push(&g_queue, &cmd);
		WaitForSingleObject(done, INFINITE);
		CloseHandle(done);
		CloseHandle(g_worker_thread);
		queue_destroy(&g_queue);
		g_worker_thread = NULL;
	}
}

//  ────────────────────────────────────────────────────────────────
//  Helpers: locate assets next to the plug‑in DLL
//  ────────────────────────────────────────────────────────────────

static void locate_assets(wchar_t *assets_path, wchar_t *icu_path, wchar_t *aot_path)
{
	HMODULE self = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)&locate_assets, &self);

	wchar_t dll_folder[MAX_PATH];
	GetModuleFileNameW(self, dll_folder, MAX_PATH);
	PathRemoveFileSpecW(dll_folder);

	wsprintfW(assets_path, L"%s\\flutter_template\\flutter_assets", dll_folder);
	wsprintfW(icu_path, L"%s\\flutter_template\\icudtl.dat", dll_folder);
	wsprintfW(aot_path, L"%s\\flutter_template\\app.so", dll_folder);
}

//  ────────────────────────────────────────────────────────────────
//  Flutter engine lifecycle (runs on worker thread)
//  ────────────────────────────────────────────────────────────────

static void engine_init(struct flutter_source *ctx)
{
	log_tid("engine_init");
	ctx->engine_tid = GetCurrentThreadId();

	// Allocate pixel buffer for the software renderer
	ctx->pixel_data = calloc(ctx->width * ctx->height, 4);

	// Resolve asset paths (UTF‑8)
	wchar_t assets_w[MAX_PATH], icu_w[MAX_PATH], aot_w[MAX_PATH];
	locate_assets(assets_w, icu_w, aot_w);

	char assets[MAX_PATH], icu[MAX_PATH], aot[MAX_PATH];
	WideCharToMultiByte(CP_UTF8, 0, assets_w, -1, assets, MAX_PATH, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, icu_w, -1, icu, MAX_PATH, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, aot_w, -1, aot, MAX_PATH, NULL, NULL);

	// Software renderer configuration
	FlutterSoftwareRendererConfig sw = {
		.struct_size = sizeof(sw),
		.surface_present_callback = surface_present_cb,
	};
	FlutterRendererConfig renderer = {
		.type = kSoftware,
		.software = sw,
	};

	// Custom platform task‑runner (this worker thread)
	ctx->platform_runner_desc = (FlutterTaskRunnerDescription){
		.struct_size = sizeof(FlutterTaskRunnerDescription),
		.user_data = ctx,
		.runs_task_on_current_thread_callback = runs_on_worker_thread,
		.post_task_callback = post_task_to_worker,
	};
	ctx->custom_runners = (FlutterCustomTaskRunners){
		.struct_size = sizeof(FlutterCustomTaskRunners),
		.platform_task_runner = &ctx->platform_runner_desc,
	};

	// Project arguments
	static const char *argv[] = {"obs_flutter", "--verbose-logging"};
	FlutterProjectArgs args = {
		.struct_size = sizeof(FlutterProjectArgs),
		.assets_path = assets,
		.icu_data_path = icu,
		.command_line_argc = (int)(sizeof(argv) / sizeof(argv[0])),
		.command_line_argv = argv,
		.log_message_callback = log_message_cb,
		.platform_message_callback = platform_message_cb,
		.custom_task_runners = &ctx->custom_runners,
	};

	// Optional AOT data (ignored if file missing)
	FlutterEngineAOTDataSource aot_src = {
		.type = kFlutterEngineAOTDataSourceTypeElfPath,
		.elf_path = aot,
	};
	if (FlutterEngineCreateAOTData(&aot_src, &ctx->aot_data) == kSuccess)
		args.aot_data = ctx->aot_data;

	// Run engine
	FlutterEngineResult res = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer, &args, ctx, &ctx->engine);
	if (res != kSuccess) {
		blog(LOG_ERROR, "FlutterEngineRun failed (%d)", res);
		return;
	}

	// Initial window metrics
	FlutterWindowMetricsEvent wm = {
		.struct_size = sizeof(wm),
		.width = ctx->width,
		.height = ctx->height,
		.pixel_ratio = 1.0f,
	};
	FlutterEngineSendWindowMetricsEvent(ctx->engine, &wm);
	FlutterEngineScheduleFrame(ctx->engine);
	blog(LOG_INFO, "Flutter engine started");
}

static void engine_shutdown(struct flutter_source *ctx)
{
	log_tid("engine_shutdown");
	if (ctx->engine) {
		FlutterEngineShutdown(ctx->engine);
		ctx->engine = NULL;
	}
}

//  ────────────────────────────────────────────────────────────────
//  OBS source implementation
//  ────────────────────────────────────────────────────────────────

static const char *source_get_name(void *unused)
{
	(void)unused;
	return "Flutter Source";
}

static void *source_create(obs_data_t *settings, obs_source_t *src)
{
	struct flutter_source *ctx = bzalloc(sizeof(*ctx));
	ctx->source = src;
	ctx->width = (uint32_t)obs_data_get_int(settings, "width");
	ctx->height = (uint32_t)obs_data_get_int(settings, "height");
	if (!ctx->width)
		ctx->width = 320;
	if (!ctx->height)
		ctx->height = 240;

	if (InterlockedIncrement(&g_source_count) == 1)
		ensure_worker_thread();

	// Request engine creation on worker thread (synchronous)
	HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
	command_t cmd = {.type = CMD_CREATE_ENGINE, .ctx = ctx, .done_event = done};
	queue_push(&g_queue, &cmd);
	WaitForSingleObject(done, INFINITE);
	CloseHandle(done);
	return ctx;
}

static void source_destroy(void *data)
{
	struct flutter_source *ctx = data;

	// Request engine shutdown (synchronous)
	HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
	command_t cmd = {.type = CMD_DESTROY_ENGINE, .ctx = ctx, .done_event = done};
	queue_push(&g_queue, &cmd);
	WaitForSingleObject(done, INFINITE);
	CloseHandle(done);

	if (ctx->texture)
		gs_texture_destroy(ctx->texture);
	free(ctx->pixel_data);
	bfree(ctx);

	if (InterlockedDecrement(&g_source_count) == 0)
		stop_worker_thread();
}

static void source_render(void *data, const gs_effect_t *effect)
{
	struct flutter_source *ctx = data;

	if (!ctx->texture)
		ctx->texture = gs_texture_create(ctx->width, ctx->height, GS_BGRA, 1, NULL, GS_DYNAMIC);

	if (InterlockedCompareExchange(&ctx->dirty_pixels, 0, 1) == 1)
		gs_texture_set_image(ctx->texture, ctx->pixel_data, ctx->width * 4, false);

	if (!ctx->texture)
		return;

	bool srgb_prev = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_eparam_t *img_param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(img_param, ctx->texture);
	gs_draw_sprite(ctx->texture, 0, ctx->width, ctx->height);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb_prev);
}

static uint32_t source_get_width(const void *data)
{
	return ((struct flutter_source *)data)->width;
}
static uint32_t source_get_height(const void *data)
{
	return ((struct flutter_source *)data)->height;
}

static obs_properties_t *source_properties(void *data)
{
	(void)data;
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", "Width", 320, 3840, 1);
	obs_properties_add_int(p, "height", "Height", 240, 2160, 1);
	return p;
}

static void source_update(void *data, obs_data_t *settings)
{
	struct flutter_source *ctx = data;
	uint32_t w = (uint32_t)obs_data_get_int(settings, "width");
	uint32_t h = (uint32_t)obs_data_get_int(settings, "height");
	if (!w)
		w = 320;
	if (!h)
		h = 240;

	if (w == ctx->width && h == ctx->height)
		return;

	ctx->width = w;
	ctx->height = h;

	if (ctx->texture)
		gs_texture_destroy(ctx->texture), ctx->texture = NULL;
	free(ctx->pixel_data);
	ctx->pixel_data = calloc(ctx->width * ctx->height, 4);

	if (ctx->engine) {
		FlutterWindowMetricsEvent wm = {
			.struct_size = sizeof(wm),
			.width = ctx->width,
			.height = ctx->height,
			.pixel_ratio = 1.0f,
		};
		FlutterEngineSendWindowMetricsEvent(ctx->engine, &wm);
		FlutterEngineScheduleFrame(ctx->engine);
	}
}

struct obs_source_info flutter_source_info = {
	.id = "flutter_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = source_get_name,
	.create = source_create,
	.destroy = source_destroy,
	.video_render = source_render,
	.get_width = source_get_width,
	.get_height = source_get_height,
	.update = source_update,
	.get_properties = source_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};
