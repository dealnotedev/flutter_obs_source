#include <obs-module.h>
#include "flutter_embedder.h"
#include <graphics/graphics.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

// OBS_DECLARE_MODULE()
// OBS_MODULE_USE_DEFAULT_LOCALE("plugintemplate-for-obs", "en-US")

struct flutter_source {
	obs_source_t *source;
	FlutterEngine engine;
	uint8_t *pixel_data;
	uint32_t width, height;
	gs_texture_t *texture;
	FlutterEngineAOTData aot_data;

	volatile LONG dirty_pixels;
};

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
}

void init_flutter_engine(struct flutter_source *context)
{
	context->pixel_data = (uint8_t *)malloc(context->width * context->height * 4);
	memset(context->pixel_data, 0, context->width * context->height * 4);

	FlutterSoftwareRendererConfig software_config = {0};
	software_config.struct_size = sizeof(FlutterSoftwareRendererConfig);
	software_config.surface_present_callback = my_surface_present_callback;

	FlutterRendererConfig renderer_config = {0};
	renderer_config.type = kSoftware;
	renderer_config.software = software_config;

	blog(LOG_INFO, "[FlutterSource] PreConfig");

	FlutterProjectArgs project_args = {0};
	project_args.struct_size = sizeof(FlutterProjectArgs);

	project_args.assets_path = "D:/obstemplate_aot/flutter_assets";
	project_args.icu_data_path = "D:/obstemplate_aot/icudtl.dat";

	//project_args.vm_snapshot_data = "D:/obstemplate/vm_snapshot_data";
	//project_args.isolate_snapshot_data = "D:/obstemplate/isolate_snapshot_data";
	//project_args.vm_snapshot_instructions       = NULL;
	//project_args.isolate_snapshot_instructions  = NULL;

	static const char *engine_argv[] = {"obs_flutter", // dummy exe name
					    "--verbose-logging",
					    "--disable-service-auth-codes",
					    "--observatory-port=0",
					    "--enable-software-rendering",
					    "--skia-deterministic-rendering"};

	project_args.command_line_argc = _countof(engine_argv);
	project_args.command_line_argv = engine_argv;
	project_args.log_message_callback = log_message_callback;
	project_args.platform_message_callback = platform_message_callback;

	FlutterEngineAOTDataSource aot_source = {
		.type = kFlutterEngineAOTDataSourceTypeElfPath,
		.elf_path = "D:/obstemplate_aot/app.so",
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

	const DWORD tid = GetCurrentThreadId();
	blog(LOG_INFO, "[FlutterSource] INIT, thread id: %lu", (unsigned long)tid);

	init_flutter_engine(context);

	return context;
}

static void flutter_source_destroy(void *data)
{
	blog(LOG_INFO, "[FlutterSource] DESTROY called");

	const DWORD tid = GetCurrentThreadId();
	blog(LOG_INFO, "[FlutterSource] DESTROY, thread id: %lu", (unsigned long)tid);

	struct flutter_source *context = data;

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

	bfree(context);

	blog(LOG_INFO, "[FlutterSource] DESTROY ended");
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

struct obs_source_info flutter_source_info = {.id = "flutter_source",
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
					      .icon_type = OBS_ICON_TYPE_MEDIA};