#include <obs-module.h>
#include "flutter_embedder.h"
#include <graphics/graphics.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

// --- Worker Thread Logic ---

typedef enum {
    CMD_CREATE_ENGINE,
    CMD_DESTROY_ENGINE,
    CMD_EXIT
} flutter_command_type;

struct flutter_source;

typedef struct {
    flutter_command_type type;
    struct flutter_source *ctx;
    HANDLE done_event;
} flutter_command;

#define MAX_COMMANDS 128
typedef struct {
    flutter_command queue[MAX_COMMANDS];
    int head, tail;
    CRITICAL_SECTION lock;
    HANDLE sem_command;
} command_queue_t;

static command_queue_t g_command_queue;
static HANDLE g_worker_thread = NULL;
static LONG g_source_count = 0;

void command_queue_init(command_queue_t *q) {
    InitializeCriticalSection(&q->lock);
    q->sem_command = CreateSemaphore(NULL, 0, MAX_COMMANDS, NULL);
    q->head = q->tail = 0;
}
void command_queue_destroy(command_queue_t *q) {
    DeleteCriticalSection(&q->lock);
    CloseHandle(q->sem_command);
}

void command_queue_push(command_queue_t *q, flutter_command *cmd) {
    EnterCriticalSection(&q->lock);
    q->queue[q->tail] = *cmd;
    q->tail = (q->tail + 1) % MAX_COMMANDS;
    LeaveCriticalSection(&q->lock);
    ReleaseSemaphore(q->sem_command, 1, NULL);
}

bool command_queue_pop(command_queue_t *q, flutter_command *out_cmd) {
    WaitForSingleObject(q->sem_command, INFINITE);
    EnterCriticalSection(&q->lock);
    if (q->head == q->tail) {
        LeaveCriticalSection(&q->lock);
        return false;
    }
    *out_cmd = q->queue[q->head];
    q->head = (q->head + 1) % MAX_COMMANDS;
    LeaveCriticalSection(&q->lock);
    return true;
}

// --- Flutter Source ---

struct flutter_source {
    obs_source_t *source;
    FlutterEngine engine;
    uint8_t *pixel_data;
    uint32_t width, height;
    gs_texture_t *texture;
    FlutterEngineAOTData aot_data;
    volatile LONG dirty_pixels;
};

// forward declarations
void init_flutter_engine(struct flutter_source *context);
void shutdown_flutter_engine(struct flutter_source *context);

static bool my_surface_present_callback(void *user_data, const void *allocation, size_t row_bytes, size_t height)
{
    struct flutter_source *ctx = user_data;
    if (!ctx->pixel_data) return false;
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

// --- Worker thread code ---

static DWORD WINAPI flutter_worker_thread(LPVOID param) {
    while (1) {
        flutter_command cmd;
        if (!command_queue_pop(&g_command_queue, &cmd))
            continue;
        switch (cmd.type) {
            case CMD_CREATE_ENGINE:
                init_flutter_engine(cmd.ctx);
                break;
            case CMD_DESTROY_ENGINE:
                shutdown_flutter_engine(cmd.ctx);
                break;
            case CMD_EXIT:
                if (cmd.done_event)
                    SetEvent(cmd.done_event);
                return 0;
        }
        if (cmd.done_event)
            SetEvent(cmd.done_event);
    }
}

void ensure_worker_thread() {
    if (!g_worker_thread) {
        command_queue_init(&g_command_queue);
        g_worker_thread = CreateThread(NULL, 0, flutter_worker_thread, NULL, 0, NULL);
    }
}
void stop_worker_thread() {
    if (g_worker_thread) {
        HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
        flutter_command cmd = {.type = CMD_EXIT, .done_event = done};
        command_queue_push(&g_command_queue, &cmd);
        WaitForSingleObject(done, INFINITE);
        CloseHandle(done);
        CloseHandle(g_worker_thread);
        command_queue_destroy(&g_command_queue);
        g_worker_thread = NULL;
    }
}

// --- Flutter engine helpers ---

void init_flutter_engine(struct flutter_source *context)
{
    context->pixel_data = (uint8_t *)malloc(context->width * context->height * 4);
    memset(context->pixel_data, 0, context->width * context->height * 4);

    wchar_t assets_path_w[MAX_PATH], icu_path_w[MAX_PATH], aot_path_w[MAX_PATH];
    get_assets_paths(assets_path_w, icu_path_w, aot_path_w);

    char assets_path[MAX_PATH], icu_path[MAX_PATH], aot_path[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, assets_path_w, -1, assets_path, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, icu_path_w, -1, icu_path, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, aot_path_w, -1, aot_path, MAX_PATH, NULL, NULL);

    FlutterSoftwareRendererConfig software_config = {0};
    software_config.struct_size = sizeof(FlutterSoftwareRendererConfig);
    software_config.surface_present_callback = my_surface_present_callback;

    FlutterRendererConfig renderer_config = {0};
    renderer_config.type = kSoftware;
    renderer_config.software = software_config;

    blog(LOG_INFO, "[FlutterSource] PreConfig");

    FlutterProjectArgs project_args = {0};
    project_args.struct_size = sizeof(FlutterProjectArgs);

    project_args.assets_path = assets_path;
    project_args.icu_data_path = icu_path;

    static const char *engine_argv[] = {"obs_flutter", "--verbose-logging"};

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
        return;
    }

    project_args.aot_data = aot_data;
    context->aot_data = aot_data;

    blog(LOG_INFO, "[FlutterSource] PreRun");

    FlutterEngineResult result =
        FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer_config, &project_args, context, &context->engine);

    if (result != kSuccess) {
        blog(LOG_ERROR, "[FlutterSource] FlutterEngineRun failed: %d", result);
    } else {
        blog(LOG_INFO, "[FlutterSource] FlutterEngineRun OK: %d", result);

        FlutterWindowMetricsEvent window_event = {0};
        window_event.struct_size = sizeof(FlutterWindowMetricsEvent);
        window_event.width = context->width;
        window_event.height = context->height;
        window_event.pixel_ratio = 1.0f;
        FlutterEngineSendWindowMetricsEvent(context->engine, &window_event);

        FlutterEngineScheduleFrame(context->engine);

        blog(LOG_INFO, "[FlutterSource] Sent WindowMetricsEvent and ScheduleFrame");
    }
}

void shutdown_flutter_engine(struct flutter_source *context)
{
    if (context->engine) {
        FlutterEngineResult res = FlutterEngineShutdown(context->engine);
        blog(LOG_INFO, "[FlutterSource] FlutterEngineShutdown result: %d", res);
        context->engine = NULL;
    }
    if (context->aot_data) {
        FlutterEngineCollectAOTData(context->aot_data);
        context->aot_data = NULL;
    }
}

// --- OBS Source Integration ---

static const char *flutter_source_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Flutter Source";
}

static void *flutter_source_create(obs_data_t *settings, obs_source_t *source)
{
    struct flutter_source *context = bzalloc(sizeof(struct flutter_source));
    context->source = source;
    context->width = (uint32_t)obs_data_get_int(settings, "width");
    context->height = (uint32_t)obs_data_get_int(settings, "height");
    if (context->width == 0)
        context->width = 320;
    if (context->height == 0)
        context->height = 240;

    if (InterlockedIncrement(&g_source_count) == 1) {
        ensure_worker_thread();
    }

    // Создание engine через worker
    HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
    flutter_command cmd = {.type = CMD_CREATE_ENGINE, .ctx = context, .done_event = done};
    command_queue_push(&g_command_queue, &cmd);
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);

    return context;
}

static void flutter_source_destroy(void *data)
{
    struct flutter_source *context = data;

    // Остановка engine через worker
    HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
    flutter_command cmd = {.type = CMD_DESTROY_ENGINE, .ctx = context, .done_event = done};
    command_queue_push(&g_command_queue, &cmd);
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);

    if (context->texture) {
        gs_texture_destroy(context->texture);
        context->texture = NULL;
    }
    if (context->pixel_data) {
        free(context->pixel_data);
        context->pixel_data = NULL;
    }

    bfree(context);

    if (InterlockedDecrement(&g_source_count) == 0) {
        stop_worker_thread();
    }
}

static void flutter_source_render(void *data, const gs_effect_t *effect)
{
    struct flutter_source *ctx = data;
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

    gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture_srgb(param, ctx->texture);
    gs_draw_sprite(ctx->texture, 0, ctx->width, ctx->height);

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);
}

static uint32_t flutter_source_get_width(const void *data)
{
    const struct flutter_source *ctx = data;
    return ctx->width;
}

static uint32_t flutter_source_get_height(const void *data)
{
    const struct flutter_source *ctx = data;
    return ctx->height;
}

static obs_properties_t *flutter_source_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_int(props, "width", "Width", 320, 3840, 1);
    obs_properties_add_int(props, "height", "Height", 240, 2160, 1);
    return props;
}

static void flutter_source_update(void *data, obs_data_t *settings)
{
    struct flutter_source *ctx = data;

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

    ctx->pixel_data = (uint8_t *)malloc(ctx->width * ctx->height * 4);
    memset(ctx->pixel_data, 0, ctx->width * ctx->height * 4);

    if (ctx->engine) {
        FlutterWindowMetricsEvent window_event = {0};
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
