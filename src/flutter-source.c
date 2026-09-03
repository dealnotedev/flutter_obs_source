/*
 * OBS Studio source plug-in that embeds the Flutter engine on Windows.
 *
 * Each source owns a dedicated platform/UI event loop. Flutter renders into
 * software frame slots which are handed to the OBS graphics thread without
 * concurrent reads and writes. Audio is mixed independently for every source.
 */

#include <math.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include <graphics/graphics.h>
#include <obs-module.h>
#include <util/platform.h>

#include "flutter_embedder.h"
#include "flutter-mouse.h"
#include "runtime-validation.h"
#include "./third_party/cjson/cJSON.h"
#include "./third_party/miniaudio/miniaudio.h"

#define FRAME_SLOT_COUNT 3
#define AUDIO_QUEUE_SIZE 128
#define MAX_SOUNDS FLUTTER_MAX_SOUNDS
#define AUDIO_FRAMES_PER_TICK 960
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_TICK_NS 20000000ULL
#define DART_CONFIG_CAPACITY 4096
#define PATH_CAPACITY 4096
#define WORKER_WAIT_TIMEOUT_MS 60000

typedef enum {
	CMD_CREATE_ENGINE,
	CMD_DESTROY_ENGINE,
	CMD_RUN_ENGINE_TASK,
	CMD_UPDATE_ENGINE,
	CMD_SET_ACTIVE,
	CMD_MOUSE_INPUT,
} command_type_t;

typedef struct command {
	command_type_t type;
	struct command *next;
	uint64_t sequence;
	uint64_t target_time_ns;
	FlutterTask task;
	HANDLE done_event;

	uint32_t width;
	uint32_t height;
	uint32_t pixel_ratio_pct;
	bool metrics_changed;
	bool config_changed;
	bool active;
	char *dart_config;
	flutter_mouse_input mouse_input;
} command_t;

typedef struct {
	CRITICAL_SECTION cs;
	CONDITION_VARIABLE changed;
	command_t *head;
	uint64_t next_sequence;
	bool accepting;
} worker_queue_t;

typedef enum {
	FRAME_SLOT_FREE,
	FRAME_SLOT_WRITING,
	FRAME_SLOT_READY,
	FRAME_SLOT_READING,
} frame_slot_state;

typedef struct {
	uint8_t *data;
	size_t capacity;
	size_t row_bytes;
	uint32_t width;
	uint32_t height;
	uint64_t generation;
	frame_slot_state state;
} frame_slot;

struct flutter_source {
	obs_source_t *source;

	FlutterEngine engine;
	FlutterEngineAOTData aot_data;
	FlutterTaskRunnerDescription platform_runner_desc;
	FlutterCustomTaskRunners custom_runners;
	HANDLE worker_thread;
	DWORD engine_tid;
	worker_queue_t worker_queue;
	volatile LONG shutting_down;
	volatile LONG runner_destroyed;
	bool engine_init_succeeded;

	CRITICAL_SECTION frame_cs;
	frame_slot frames[FRAME_SLOT_COUNT];
	int ready_frame;
	uint64_t frame_generation;
	uint32_t width;
	uint32_t height;
	uint32_t pixel_ratio_pct;
	gs_texture_t *texture;
	uint32_t texture_width;
	uint32_t texture_height;

	CRITICAL_SECTION config_cs;
	char requested_dart_config[DART_CONFIG_CAPACITY];
	char dart_config[DART_CONFIG_CAPACITY];
	char assets_dir[PATH_CAPACITY];
	flutter_mouse_state mouse_state;

	CRITICAL_SECTION audio_cs;
	flutter_audio_cmd audio_queue[AUDIO_QUEUE_SIZE];
	uint32_t audio_head;
	uint32_t audio_tail;
	ma_engine ma;
	bool ma_initialized;
	ma_sound *sounds[MAX_SOUNDS];
	HANDLE audio_timer;
	float *mix_interleaved;
	float *mix_left;
	float *mix_right;
	uint64_t next_audio_timestamp;
	volatile LONG audio_tick_running;
	volatile LONG audio_reanchor;
	volatile LONG active;
};

static void log_flutter_result(const char *operation, FlutterEngineResult result)
{
	if (result != kSuccess)
		blog(LOG_ERROR, "[FlutterSource] %s failed (%d)", operation, (int)result);
}

static bool copy_string(char *destination, size_t capacity, const char *source, const char *field_name)
{
	if (!destination || !capacity || !source)
		return false;

	const size_t length = strlen(source);
	if (length >= capacity) {
		blog(LOG_WARNING, "[FlutterSource] %s was truncated from %zu to %zu bytes", field_name, length,
		     capacity - 1);
	}

	const size_t copied = length < capacity - 1 ? length : capacity - 1;
	memcpy(destination, source, copied);
	destination[copied] = '\0';
	return length < capacity;
}

static char *duplicate_string(const char *source)
{
	const size_t length = strlen(source) + 1;
	char *copy = malloc(length);
	if (copy)
		memcpy(copy, source, length);
	return copy;
}

static command_t *command_create(command_type_t type)
{
	command_t *command = calloc(1, sizeof(*command));
	if (!command)
		blog(LOG_ERROR, "[FlutterSource] Unable to allocate a worker command");
	else
		command->type = type;
	return command;
}

static void command_destroy(command_t *command)
{
	if (!command)
		return;
	free(command->dart_config);
	free(command);
}

static uint64_t command_due_time(const command_t *command)
{
	return command->type == CMD_RUN_ENGINE_TASK || command->type == CMD_MOUSE_INPUT ? command->target_time_ns : 0;
}

static void worker_queue_init(worker_queue_t *queue)
{
	InitializeCriticalSection(&queue->cs);
	InitializeConditionVariable(&queue->changed);
	queue->head = NULL;
	queue->next_sequence = 0;
	queue->accepting = true;
}

static void worker_queue_insert_locked(worker_queue_t *queue, command_t *command)
{
	command->sequence = ++queue->next_sequence;
	const uint64_t due = command_due_time(command);
	command_t **position = &queue->head;
	while (*position) {
		const uint64_t current_due = command_due_time(*position);
		if (due < current_due || (due == current_due && command->sequence < (*position)->sequence))
			break;
		position = &(*position)->next;
	}
	command->next = *position;
	*position = command;
}

static bool worker_queue_push(worker_queue_t *queue, command_t *command, bool control_command)
{
	EnterCriticalSection(&queue->cs);
	const bool accepted = queue->accepting || control_command;
	if (accepted) {
		worker_queue_insert_locked(queue, command);
		WakeConditionVariable(&queue->changed);
	}

	LeaveCriticalSection(&queue->cs);
	return accepted;
}

static bool worker_queue_push_mouse(worker_queue_t *queue, command_t *command)
{
	EnterCriticalSection(&queue->cs);
	if (!queue->accepting) {
		LeaveCriticalSection(&queue->cs);
		return false;
	}

	if (command->mouse_input.type == FLUTTER_MOUSE_INPUT_MOVE) {
		command_t **position = &queue->head;
		command_t **last_mouse_position = NULL;
		while (*position) {
			if ((*position)->type == CMD_MOUSE_INPUT)
				last_mouse_position = position;
			position = &(*position)->next;
		}
		if (last_mouse_position && (*last_mouse_position)->mouse_input.type == FLUTTER_MOUSE_INPUT_MOVE) {
			command_t *superseded = *last_mouse_position;
			*last_mouse_position = superseded->next;
			superseded->next = NULL;
			command_destroy(superseded);
		}
	}

	worker_queue_insert_locked(queue, command);
	WakeConditionVariable(&queue->changed);

	LeaveCriticalSection(&queue->cs);
	return true;
}

static void worker_queue_stop_accepting(worker_queue_t *queue)
{
	EnterCriticalSection(&queue->cs);
	queue->accepting = false;
	WakeAllConditionVariable(&queue->changed);
	LeaveCriticalSection(&queue->cs);
}

static command_t *worker_queue_wait_pop(worker_queue_t *queue)
{
	EnterCriticalSection(&queue->cs);
	for (;;) {
		if (!queue->head) {
			SleepConditionVariableCS(&queue->changed, &queue->cs, INFINITE);
			continue;
		}

		command_t *command = queue->head;
		if (command->type == CMD_RUN_ENGINE_TASK) {
			const uint64_t now = FlutterEngineGetCurrentTime();
			if (command->target_time_ns > now) {
				const uint64_t delta_ns = command->target_time_ns - now;
				uint64_t wait_ms_64 = (delta_ns + 999999ULL) / 1000000ULL;
				if (wait_ms_64 >= INFINITE)
					wait_ms_64 = INFINITE - 1;
				SleepConditionVariableCS(&queue->changed, &queue->cs, (DWORD)wait_ms_64);
				continue;
			}
		}

		queue->head = command->next;
		command->next = NULL;
		LeaveCriticalSection(&queue->cs);
		return command;
	}
}

static void worker_queue_destroy(worker_queue_t *queue)
{
	EnterCriticalSection(&queue->cs);
	command_t *command = queue->head;
	queue->head = NULL;
	LeaveCriticalSection(&queue->cs);

	while (command) {
		command_t *next = command->next;
		if (command->done_event)
			SetEvent(command->done_event);
		command_destroy(command);
		command = next;
	}
	DeleteCriticalSection(&queue->cs);
}

static bool audio_queue_push(struct flutter_source *ctx, const flutter_audio_cmd *command)
{
	bool accepted = false;
	EnterCriticalSection(&ctx->audio_cs);
	const uint32_t next = (ctx->audio_head + 1) % AUDIO_QUEUE_SIZE;
	if (next != ctx->audio_tail && !InterlockedCompareExchange(&ctx->shutting_down, 0, 0)) {
		ctx->audio_queue[ctx->audio_head] = *command;
		ctx->audio_head = next;
		accepted = true;
	}
	LeaveCriticalSection(&ctx->audio_cs);
	return accepted;
}

static bool audio_queue_pop(struct flutter_source *ctx, flutter_audio_cmd *command)
{
	bool available = false;
	EnterCriticalSection(&ctx->audio_cs);
	if (ctx->audio_tail != ctx->audio_head) {
		*command = ctx->audio_queue[ctx->audio_tail];
		ctx->audio_tail = (ctx->audio_tail + 1) % AUDIO_QUEUE_SIZE;
		available = true;
	}
	LeaveCriticalSection(&ctx->audio_cs);
	return available;
}

static bool module_asset_paths(char *assets_path, char *icu_path, char *aot_path)
{
	HMODULE module = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)&module_asset_paths, &module)) {
		blog(LOG_ERROR, "[FlutterSource] GetModuleHandleExW failed (%lu)", GetLastError());
		return false;
	}

	wchar_t module_path[PATH_CAPACITY];
	const DWORD length = GetModuleFileNameW(module, module_path, PATH_CAPACITY);
	if (!length || length >= PATH_CAPACITY) {
		blog(LOG_ERROR, "[FlutterSource] Unable to resolve the plugin DLL path (%lu)", GetLastError());
		return false;
	}

	wchar_t *separator = wcsrchr(module_path, L'\\');
	if (!separator)
		separator = wcsrchr(module_path, L'/');
	if (!separator) {
		blog(LOG_ERROR, "[FlutterSource] Plugin DLL path has no directory");
		return false;
	}
	*separator = L'\0';

	wchar_t assets_w[PATH_CAPACITY];
	wchar_t icu_w[PATH_CAPACITY];
	wchar_t aot_w[PATH_CAPACITY];
	if (_snwprintf_s(assets_w, PATH_CAPACITY, _TRUNCATE, L"%ls\\flutter_obs_source\\flutter_assets", module_path) < 0 ||
	    _snwprintf_s(icu_w, PATH_CAPACITY, _TRUNCATE, L"%ls\\flutter_obs_source\\icudtl.dat", module_path) < 0 ||
	    _snwprintf_s(aot_w, PATH_CAPACITY, _TRUNCATE, L"%ls\\flutter_obs_source\\app.so", module_path) < 0) {
		blog(LOG_ERROR, "[FlutterSource] Runtime asset path exceeds %d characters", PATH_CAPACITY - 1);
		return false;
	}

	const DWORD assets_attributes = GetFileAttributesW(assets_w);
	const DWORD icu_attributes = GetFileAttributesW(icu_w);
	const DWORD aot_attributes = GetFileAttributesW(aot_w);
	if (assets_attributes == INVALID_FILE_ATTRIBUTES || !(assets_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    icu_attributes == INVALID_FILE_ATTRIBUTES || (icu_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
	    aot_attributes == INVALID_FILE_ATTRIBUTES || (aot_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		blog(LOG_ERROR,
		     "[FlutterSource] Runtime bundle is incomplete. Expected flutter_assets, icudtl.dat and app.so in %ls\\flutter_obs_source",
		     module_path);
		return false;
	}

	if (!WideCharToMultiByte(CP_UTF8, 0, assets_w, -1, assets_path, PATH_CAPACITY, NULL, NULL) ||
	    !WideCharToMultiByte(CP_UTF8, 0, icu_w, -1, icu_path, PATH_CAPACITY, NULL, NULL) ||
	    !WideCharToMultiByte(CP_UTF8, 0, aot_w, -1, aot_path, PATH_CAPACITY, NULL, NULL)) {
		blog(LOG_ERROR, "[FlutterSource] Runtime asset path cannot be represented as UTF-8");
		return false;
	}

	return true;
}

static bool surface_present_cb(void *user_data, const void *allocation, size_t row_bytes, size_t height)
{
	struct flutter_source *ctx = user_data;
	if (!ctx || !allocation || InterlockedCompareExchange(&ctx->shutting_down, 0, 0))
		return false;

	EnterCriticalSection(&ctx->frame_cs);
	const uint32_t width = ctx->width;
	const uint32_t expected_height = ctx->height;
	const uint64_t generation = ctx->frame_generation;
	size_t required_size = 0;
	if (height != expected_height || row_bytes < (size_t)width * 4 ||
	    (height && row_bytes > SIZE_MAX / height) ||
	    !flutter_checked_frame_size(width, expected_height, &required_size)) {
		LeaveCriticalSection(&ctx->frame_cs);
		return true;
	}

	int slot_index = -1;
	for (int i = 0; i < FRAME_SLOT_COUNT; ++i) {
		if (ctx->frames[i].state == FRAME_SLOT_FREE) {
			slot_index = i;
			ctx->frames[i].state = FRAME_SLOT_WRITING;
			break;
		}
	}
	LeaveCriticalSection(&ctx->frame_cs);

	if (slot_index < 0)
		return true;

	frame_slot *slot = &ctx->frames[slot_index];
	if (slot->capacity < required_size) {
		uint8_t *resized = realloc(slot->data, required_size);
		if (!resized) {
			blog(LOG_ERROR, "[FlutterSource] Unable to allocate %zu bytes for a Flutter frame", required_size);
			EnterCriticalSection(&ctx->frame_cs);
			slot->state = FRAME_SLOT_FREE;
			LeaveCriticalSection(&ctx->frame_cs);
			return false;
		}
		slot->data = resized;
		slot->capacity = required_size;
	}

	const size_t tight_row_bytes = (size_t)width * 4;
	const uint8_t *source = allocation;
	for (uint32_t row = 0; row < expected_height; ++row)
		memcpy(slot->data + row * tight_row_bytes, source + row * row_bytes, tight_row_bytes);

	EnterCriticalSection(&ctx->frame_cs);
	if (ctx->frame_generation != generation || ctx->width != width || ctx->height != expected_height ||
	    InterlockedCompareExchange(&ctx->shutting_down, 0, 0)) {
		slot->state = FRAME_SLOT_FREE;
	} else {
		if (ctx->ready_frame >= 0 && ctx->frames[ctx->ready_frame].state == FRAME_SLOT_READY)
			ctx->frames[ctx->ready_frame].state = FRAME_SLOT_FREE;
		slot->row_bytes = tight_row_bytes;
		slot->width = width;
		slot->height = expected_height;
		slot->generation = generation;
		slot->state = FRAME_SLOT_READY;
		ctx->ready_frame = slot_index;
	}
	LeaveCriticalSection(&ctx->frame_cs);
	return true;
}

static void log_message_cb(const char *tag, const char *message, void *user_data)
{
	(void)user_data;
	blog(LOG_INFO, "[Flutter] [%s] %s", tag ? tag : "no-tag", message ? message : "(null)");
}

static bool platform_message_equals(const FlutterPlatformMessage *message, const char *expected)
{
	const size_t expected_size = strlen(expected);
	return message->message_size == expected_size &&
	       (!expected_size || (message->message && memcmp(message->message, expected, expected_size) == 0));
}

static void send_platform_response(struct flutter_source *ctx, const FlutterPlatformMessage *message,
				   const char *response)
{
	if (!message->response_handle)
		return;
	const uint8_t *data = response ? (const uint8_t *)response : NULL;
	const size_t size = response ? strlen(response) : 0;
	log_flutter_result("FlutterEngineSendPlatformMessageResponse",
			   FlutterEngineSendPlatformMessageResponse(ctx->engine, message->response_handle, data, size));
}

static void platform_message_cb(const FlutterPlatformMessage *message, void *user_data)
{
	struct flutter_source *ctx = user_data;
	if (!ctx || !message || !message->channel || !ctx->engine)
		return;

	if (strcmp(message->channel, "obs_config") == 0) {
		if (platform_message_equals(message, "get_dart_config"))
			send_platform_response(ctx, message, ctx->dart_config);
		else
			send_platform_response(ctx, message, "{\"ok\":false,\"error\":\"unknown_request\"}");
		return;
	}

	if (strcmp(message->channel, "obs_audio") == 0) {
		flutter_audio_cmd command;
		if (!flutter_parse_audio_json((const char *)message->message, message->message_size, &command)) {
			send_platform_response(ctx, message, "{\"ok\":false,\"error\":\"invalid_command\"}");
			return;
		}
		if (!audio_queue_push(ctx, &command)) {
			send_platform_response(ctx, message, "{\"ok\":false,\"error\":\"queue_full\"}");
			return;
		}
		send_platform_response(ctx, message, "{\"ok\":true}");
		return;
	}

	send_platform_response(ctx, message, NULL);
}

static bool runs_on_worker_thread(void *user_data)
{
	const struct flutter_source *ctx = user_data;
	return ctx && GetCurrentThreadId() == ctx->engine_tid;
}

static void post_task_to_worker(FlutterTask task, uint64_t target_time_ns, void *user_data)
{
	struct flutter_source *ctx = user_data;
	if (!ctx || InterlockedCompareExchange(&ctx->shutting_down, 0, 0))
		return;

	command_t *command = command_create(CMD_RUN_ENGINE_TASK);
	if (!command)
		return;
	command->task = task;
	command->target_time_ns = target_time_ns;
	if (!worker_queue_push(&ctx->worker_queue, command, false))
		command_destroy(command);
}

static void task_runner_destroyed(void *user_data)
{
	struct flutter_source *ctx = user_data;
	if (ctx)
		InterlockedExchange(&ctx->runner_destroyed, 1);
}

static bool send_message_to_dart(struct flutter_source *ctx, const char *channel, const char *payload)
{
	if (!ctx->engine)
		return false;
	const FlutterPlatformMessage message = {
		.struct_size = sizeof(message),
		.channel = channel,
		.message = payload ? (const uint8_t *)payload : NULL,
		.message_size = payload ? strlen(payload) : 0,
		.response_handle = NULL,
	};
	const FlutterEngineResult result = FlutterEngineSendPlatformMessage(ctx->engine, &message);
	log_flutter_result("FlutterEngineSendPlatformMessage", result);
	return result == kSuccess;
}

static bool engine_init(struct flutter_source *ctx)
{
	char assets[PATH_CAPACITY];
	char icu[PATH_CAPACITY];
	char aot[PATH_CAPACITY];
	if (!module_asset_paths(assets, icu, aot))
		return false;
	copy_string(ctx->assets_dir, sizeof(ctx->assets_dir), assets, "Flutter assets path");

	const FlutterSoftwareRendererConfig software = {
		.struct_size = sizeof(software),
		.surface_present_callback = surface_present_cb,
	};
	const FlutterRendererConfig renderer = {
		.type = kSoftware,
		.software = software,
	};

	ctx->platform_runner_desc = (FlutterTaskRunnerDescription){
		.struct_size = sizeof(FlutterTaskRunnerDescription),
		.user_data = ctx,
		.runs_task_on_current_thread_callback = runs_on_worker_thread,
		.post_task_callback = post_task_to_worker,
		.identifier = (size_t)(uintptr_t)ctx,
		.destruction_callback = task_runner_destroyed,
	};
	ctx->custom_runners = (FlutterCustomTaskRunners){
		.struct_size = sizeof(FlutterCustomTaskRunners),
		.platform_task_runner = &ctx->platform_runner_desc,
		.ui_task_runner = &ctx->platform_runner_desc,
	};

	static const char *arguments[] = {"obs_flutter"};
	FlutterProjectArgs project = {
		.struct_size = sizeof(project),
		.assets_path = assets,
		.icu_data_path = icu,
		.command_line_argc = (int)(sizeof(arguments) / sizeof(arguments[0])),
		.command_line_argv = arguments,
		.log_message_callback = log_message_cb,
		.platform_message_callback = platform_message_cb,
		.custom_task_runners = &ctx->custom_runners,
		.shutdown_dart_vm_when_done = true,
	};

	const FlutterEngineAOTDataSource aot_source = {
		.type = kFlutterEngineAOTDataSourceTypeElfPath,
		.elf_path = aot,
	};
	FlutterEngineResult result = FlutterEngineCreateAOTData(&aot_source, &ctx->aot_data);
	if (result != kSuccess) {
		log_flutter_result("FlutterEngineCreateAOTData", result);
		return false;
	}
	project.aot_data = ctx->aot_data;

	result = FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &renderer, &project, ctx, &ctx->engine);
	if (result != kSuccess) {
		log_flutter_result("FlutterEngineInitialize", result);
		FlutterEngineCollectAOTData(ctx->aot_data);
		ctx->aot_data = NULL;
		return false;
	}

	result = FlutterEngineRunInitialized(ctx->engine);
	if (result != kSuccess) {
		log_flutter_result("FlutterEngineRunInitialized", result);
		FlutterEngineShutdown(ctx->engine);
		ctx->engine = NULL;
		FlutterEngineCollectAOTData(ctx->aot_data);
		ctx->aot_data = NULL;
		return false;
	}

	const FlutterWindowMetricsEvent metrics = {
		.struct_size = sizeof(metrics),
		.width = ctx->width,
		.height = ctx->height,
		.pixel_ratio = (double)ctx->pixel_ratio_pct / 100.0,
	};
	log_flutter_result("FlutterEngineSendWindowMetricsEvent",
			   FlutterEngineSendWindowMetricsEvent(ctx->engine, &metrics));
	log_flutter_result("FlutterEngineScheduleFrame", FlutterEngineScheduleFrame(ctx->engine));
	blog(LOG_INFO, "[FlutterSource] Engine started with merged platform/UI runner");
	return true;
}

static void engine_shutdown(struct flutter_source *ctx)
{
	if (ctx->engine) {
		log_flutter_result("FlutterEngineShutdown", FlutterEngineShutdown(ctx->engine));
		ctx->engine = NULL;
	}
	if (ctx->aot_data) {
		log_flutter_result("FlutterEngineCollectAOTData", FlutterEngineCollectAOTData(ctx->aot_data));
		ctx->aot_data = NULL;
	}
}

static void engine_update(struct flutter_source *ctx, const command_t *command)
{
	if (!ctx->engine)
		return;

	if (command->config_changed && command->dart_config) {
		copy_string(ctx->dart_config, sizeof(ctx->dart_config), command->dart_config, "Dart config");
		send_message_to_dart(ctx, "obs_config", ctx->dart_config);
	}

	if (command->metrics_changed) {
		const FlutterWindowMetricsEvent metrics = {
			.struct_size = sizeof(metrics),
			.width = command->width,
			.height = command->height,
			.pixel_ratio = (double)command->pixel_ratio_pct / 100.0,
		};
		log_flutter_result("FlutterEngineSendWindowMetricsEvent",
				   FlutterEngineSendWindowMetricsEvent(ctx->engine, &metrics));
		log_flutter_result("FlutterEngineScheduleFrame", FlutterEngineScheduleFrame(ctx->engine));
	}
}

static void engine_handle_mouse_input(struct flutter_source *ctx, const flutter_mouse_input *input)
{
	if (!ctx->engine)
		return;

	flutter_mouse_output output;
	flutter_mouse_translate(&ctx->mouse_state, input, &output);
	if (output.pointer_event_count) {
		log_flutter_result("FlutterEngineSendPointerEvent",
				   FlutterEngineSendPointerEvent(ctx->engine, output.pointer_events,
							 output.pointer_event_count));
	}
	if (output.has_focus_event) {
		log_flutter_result("FlutterEngineSendViewFocusEvent",
				   FlutterEngineSendViewFocusEvent(ctx->engine, &output.focus_event));
	}
}

static DWORD WINAPI worker_thread_fn(LPVOID parameter)
{
	struct flutter_source *ctx = parameter;
	ctx->engine_tid = GetCurrentThreadId();

	for (;;) {
		command_t *command = worker_queue_wait_pop(&ctx->worker_queue);
		bool exit_worker = false;

		switch (command->type) {
		case CMD_CREATE_ENGINE:
			ctx->engine_init_succeeded = engine_init(ctx);
			break;
		case CMD_DESTROY_ENGINE:
			engine_shutdown(ctx);
			exit_worker = true;
			break;
		case CMD_RUN_ENGINE_TASK:
			if (ctx->engine)
				log_flutter_result("FlutterEngineRunTask", FlutterEngineRunTask(ctx->engine, &command->task));
			break;
		case CMD_UPDATE_ENGINE:
			engine_update(ctx, command);
			break;
		case CMD_SET_ACTIVE:
			if (ctx->engine)
				send_message_to_dart(ctx, "flutter/lifecycle",
						     command->active ? "AppLifecycleState.resumed" : "AppLifecycleState.paused");
			break;
		case CMD_MOUSE_INPUT:
			engine_handle_mouse_input(ctx, &command->mouse_input);
			break;
		}

		if (command->done_event)
			SetEvent(command->done_event);
		command_destroy(command);
		if (exit_worker)
			return 0;
	}
}

static bool wait_for_worker_event(HANDLE event, const char *operation)
{
	const DWORD result = WaitForSingleObject(event, WORKER_WAIT_TIMEOUT_MS);
	if (result == WAIT_OBJECT_0)
		return true;
	blog(LOG_ERROR, "[FlutterSource] Timed out while waiting for %s; waiting for safe completion", operation);
	return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
}

static bool initialize_audio(struct flutter_source *ctx)
{
	ma_engine_config config = ma_engine_config_init();
	config.channels = 2;
	config.sampleRate = AUDIO_SAMPLE_RATE;
	config.noDevice = MA_TRUE;

	const ma_result result = ma_engine_init(&config, &ctx->ma);
	if (result != MA_SUCCESS) {
		blog(LOG_ERROR, "[FlutterSource] ma_engine_init failed (%d); audio is disabled", result);
		return false;
	}
	ctx->ma_initialized = true;

	ctx->mix_interleaved = calloc(AUDIO_FRAMES_PER_TICK * 2, sizeof(float));
	ctx->mix_left = calloc(AUDIO_FRAMES_PER_TICK, sizeof(float));
	ctx->mix_right = calloc(AUDIO_FRAMES_PER_TICK, sizeof(float));
	if (!ctx->mix_interleaved || !ctx->mix_left || !ctx->mix_right) {
		blog(LOG_ERROR, "[FlutterSource] Unable to allocate audio mix buffers; audio is disabled");
		free(ctx->mix_interleaved);
		free(ctx->mix_left);
		free(ctx->mix_right);
		ctx->mix_interleaved = NULL;
		ctx->mix_left = NULL;
		ctx->mix_right = NULL;
		ma_engine_uninit(&ctx->ma);
		ctx->ma_initialized = false;
		return false;
	}
	return true;
}

static void uninitialize_audio(struct flutter_source *ctx)
{
	for (int i = 0; i < MAX_SOUNDS; ++i) {
		if (ctx->sounds[i]) {
			ma_sound_uninit(ctx->sounds[i]);
			free(ctx->sounds[i]);
			ctx->sounds[i] = NULL;
		}
	}
	if (ctx->ma_initialized) {
		ma_engine_uninit(&ctx->ma);
		ctx->ma_initialized = false;
	}
	free(ctx->mix_interleaved);
	free(ctx->mix_left);
	free(ctx->mix_right);
	ctx->mix_interleaved = NULL;
	ctx->mix_left = NULL;
	ctx->mix_right = NULL;
}

static VOID CALLBACK audio_tick(PVOID parameter, BOOLEAN timer_fired)
{
	(void)timer_fired;
	struct flutter_source *ctx = parameter;
	if (!ctx || InterlockedCompareExchange(&ctx->shutting_down, 0, 0) ||
	    InterlockedCompareExchange(&ctx->audio_tick_running, 1, 0) != 0)
		return;

	flutter_audio_cmd command;
	while (audio_queue_pop(ctx, &command)) {
		if (command.id < 0 || command.id >= MAX_SOUNDS)
			continue;

		switch (command.type) {
		case FLUTTER_AUDIO_CMD_LOAD: {
			if (!ctx->ma_initialized)
				break;
			if (ctx->sounds[command.id]) {
				ma_sound_uninit(ctx->sounds[command.id]);
				free(ctx->sounds[command.id]);
				ctx->sounds[command.id] = NULL;
			}

			char full_path[PATH_CAPACITY];
			const int written = command.is_relative
					    ? snprintf(full_path, sizeof(full_path), "%s\\%s", ctx->assets_dir, command.path)
					    : snprintf(full_path, sizeof(full_path), "%s", command.path);
			if (written < 0 || (size_t)written >= sizeof(full_path)) {
				blog(LOG_ERROR, "[FlutterSource] Audio path is too long");
				break;
			}

			ma_sound *sound = calloc(1, sizeof(*sound));
			if (!sound) {
				blog(LOG_ERROR, "[FlutterSource] Unable to allocate sound %d", command.id);
				break;
			}
			const ma_result result = ma_sound_init_from_file(&ctx->ma, full_path,
							       MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC, NULL, NULL,
							       sound);
			if (result != MA_SUCCESS) {
				blog(LOG_ERROR, "[FlutterSource] Unable to load %s (miniaudio %d)", full_path, result);
				free(sound);
			} else {
				ctx->sounds[command.id] = sound;
			}
			break;
		}
		case FLUTTER_AUDIO_CMD_PLAY:
			if (ctx->sounds[command.id]) {
				ma_sound_set_volume(ctx->sounds[command.id], command.volume);
				ma_sound_set_looping(ctx->sounds[command.id], command.loop);
				ma_sound_start(ctx->sounds[command.id]);
			}
			break;
		case FLUTTER_AUDIO_CMD_PAUSE:
			if (ctx->sounds[command.id])
				ma_sound_stop(ctx->sounds[command.id]);
			break;
		case FLUTTER_AUDIO_CMD_RESUME:
			if (ctx->sounds[command.id])
				ma_sound_start(ctx->sounds[command.id]);
			break;
		case FLUTTER_AUDIO_CMD_STOP:
			if (ctx->sounds[command.id])
				ma_sound_stop(ctx->sounds[command.id]);
			break;
		case FLUTTER_AUDIO_CMD_VOLUME:
			if (ctx->sounds[command.id])
				ma_sound_set_volume(ctx->sounds[command.id], command.volume);
			break;
		}
	}

	if (ctx->ma_initialized && InterlockedCompareExchange(&ctx->active, 0, 0)) {
		if (InterlockedExchange(&ctx->audio_reanchor, 0))
			ctx->next_audio_timestamp = 0;
		memset(ctx->mix_interleaved, 0, sizeof(float) * AUDIO_FRAMES_PER_TICK * 2);
		ma_uint64 frames_read = 0;
		const ma_result result =
			ma_engine_read_pcm_frames(&ctx->ma, ctx->mix_interleaved, AUDIO_FRAMES_PER_TICK, &frames_read);
		if (result == MA_SUCCESS || result == MA_AT_END) {
			for (int i = 0; i < AUDIO_FRAMES_PER_TICK; ++i) {
				ctx->mix_left[i] = ctx->mix_interleaved[i * 2];
				ctx->mix_right[i] = ctx->mix_interleaved[i * 2 + 1];
			}

			const uint64_t now = os_gettime_ns();
			if (!ctx->next_audio_timestamp || now > ctx->next_audio_timestamp + AUDIO_TICK_NS * 5 ||
			    ctx->next_audio_timestamp > now + AUDIO_TICK_NS * 5)
				ctx->next_audio_timestamp = now;

			const struct obs_source_audio output = {
				.data = {(uint8_t *)ctx->mix_left, (uint8_t *)ctx->mix_right},
				.frames = AUDIO_FRAMES_PER_TICK,
				.timestamp = ctx->next_audio_timestamp,
				.samples_per_sec = AUDIO_SAMPLE_RATE,
				.speakers = SPEAKERS_STEREO,
				.format = AUDIO_FORMAT_FLOAT_PLANAR,
			};
			ctx->next_audio_timestamp += AUDIO_TICK_NS;
			obs_source_output_audio(ctx->source, &output);
		}
	}

	InterlockedExchange(&ctx->audio_tick_running, 0);
}

static void stop_audio_timer(struct flutter_source *ctx)
{
	if (ctx->audio_timer) {
		if (!DeleteTimerQueueTimer(NULL, ctx->audio_timer, INVALID_HANDLE_VALUE)) {
			const DWORD error = GetLastError();
			if (error != ERROR_IO_PENDING)
				blog(LOG_WARNING, "[FlutterSource] DeleteTimerQueueTimer failed (%lu)", error);
		}
		ctx->audio_timer = NULL;
	}
}

static bool enqueue_control_and_wait(struct flutter_source *ctx, command_type_t type, const char *operation)
{
	HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!done) {
		blog(LOG_ERROR, "[FlutterSource] CreateEventW failed for %s (%lu)", operation, GetLastError());
		return false;
	}
	command_t *command = command_create(type);
	if (!command) {
		CloseHandle(done);
		return false;
	}
	command->done_event = done;
	if (!worker_queue_push(&ctx->worker_queue, command, true)) {
		command_destroy(command);
		CloseHandle(done);
		return false;
	}
	const bool completed = wait_for_worker_event(done, operation);
	CloseHandle(done);
	return completed;
}

static void release_frame_resources(struct flutter_source *ctx)
{
	obs_enter_graphics();
	if (ctx->texture)
		gs_texture_destroy(ctx->texture);
	ctx->texture = NULL;
	obs_leave_graphics();

	for (int i = 0; i < FRAME_SLOT_COUNT; ++i) {
		free(ctx->frames[i].data);
		ctx->frames[i].data = NULL;
		ctx->frames[i].capacity = 0;
	}
}

static void dispose_source(struct flutter_source *ctx)
{
	if (!ctx)
		return;
	stop_audio_timer(ctx);
	uninitialize_audio(ctx);
	release_frame_resources(ctx);
	worker_queue_destroy(&ctx->worker_queue);
	DeleteCriticalSection(&ctx->audio_cs);
	DeleteCriticalSection(&ctx->config_cs);
	DeleteCriticalSection(&ctx->frame_cs);
	bfree(ctx);
}

static const char *source_get_name(void *unused)
{
	(void)unused;
	return "Freydis Overlay";
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	struct flutter_source *ctx = bzalloc(sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->source = source;
	ctx->ready_frame = -1;
	ctx->frame_generation = 1;
	InitializeCriticalSection(&ctx->frame_cs);
	InitializeCriticalSection(&ctx->config_cs);
	InitializeCriticalSection(&ctx->audio_cs);
	worker_queue_init(&ctx->worker_queue);
	flutter_mouse_state_init(&ctx->mouse_state);

	ctx->width = flutter_normalize_setting(obs_data_get_int(settings, "width"), 640, 320, 3840);
	ctx->height = flutter_normalize_setting(obs_data_get_int(settings, "height"), 480, 240, 2160);
	ctx->pixel_ratio_pct = flutter_normalize_setting(obs_data_get_int(settings, "pixel_ratio"), 100, 25, 400);

	const char *config = obs_data_get_string(settings, "dart_config");
	if (!config || !config[0])
		config = "{}";
	char normalized_config[DART_CONFIG_CAPACITY];
	copy_string(normalized_config, sizeof(normalized_config), config, "Dart config");
	copy_string(ctx->requested_dart_config, sizeof(ctx->requested_dart_config), normalized_config, "Dart config");
	copy_string(ctx->dart_config, sizeof(ctx->dart_config), ctx->requested_dart_config, "Dart config");

	initialize_audio(ctx);
	ctx->worker_thread = CreateThread(NULL, 0, worker_thread_fn, ctx, 0, &ctx->engine_tid);
	if (!ctx->worker_thread) {
		blog(LOG_ERROR, "[FlutterSource] CreateThread failed (%lu)", GetLastError());
		dispose_source(ctx);
		return NULL;
	}

	if (!enqueue_control_and_wait(ctx, CMD_CREATE_ENGINE, "Flutter engine creation") || !ctx->engine_init_succeeded) {
		InterlockedExchange(&ctx->shutting_down, 1);
		worker_queue_stop_accepting(&ctx->worker_queue);
		enqueue_control_and_wait(ctx, CMD_DESTROY_ENGINE, "failed Flutter engine cleanup");
		WaitForSingleObject(ctx->worker_thread, INFINITE);
		CloseHandle(ctx->worker_thread);
		ctx->worker_thread = NULL;
		dispose_source(ctx);
		return NULL;
	}

	if (ctx->ma_initialized &&
	    !CreateTimerQueueTimer(&ctx->audio_timer, NULL, audio_tick, ctx, 0, 20, WT_EXECUTEDEFAULT)) {
		blog(LOG_ERROR, "[FlutterSource] CreateTimerQueueTimer failed (%lu); audio is disabled", GetLastError());
		ctx->audio_timer = NULL;
	}
	return ctx;
}

static void source_destroy(void *data)
{
	struct flutter_source *ctx = data;
	if (!ctx)
		return;

	InterlockedExchange(&ctx->shutting_down, 1);
	InterlockedExchange(&ctx->active, 0);
	stop_audio_timer(ctx);
	worker_queue_stop_accepting(&ctx->worker_queue);
	enqueue_control_and_wait(ctx, CMD_DESTROY_ENGINE, "Flutter engine shutdown");
	if (ctx->worker_thread) {
		wait_for_worker_event(ctx->worker_thread, "Flutter worker shutdown");
		CloseHandle(ctx->worker_thread);
		ctx->worker_thread = NULL;
	}
	dispose_source(ctx);
}

static void source_render(void *data, gs_effect_t *effect)
{
	struct flutter_source *ctx = data;
	int slot_index = -1;
	uint32_t current_width;
	uint32_t current_height;

	EnterCriticalSection(&ctx->frame_cs);
	current_width = ctx->width;
	current_height = ctx->height;
	if (ctx->ready_frame >= 0 && ctx->frames[ctx->ready_frame].state == FRAME_SLOT_READY) {
		slot_index = ctx->ready_frame;
		ctx->frames[slot_index].state = FRAME_SLOT_READING;
		ctx->ready_frame = -1;
	}
	LeaveCriticalSection(&ctx->frame_cs);

	if (slot_index >= 0) {
		frame_slot *slot = &ctx->frames[slot_index];
		if (!ctx->texture || ctx->texture_width != slot->width || ctx->texture_height != slot->height) {
			if (ctx->texture)
				gs_texture_destroy(ctx->texture);
			ctx->texture = gs_texture_create(slot->width, slot->height, GS_BGRA, 1, NULL, GS_DYNAMIC);
			ctx->texture_width = slot->width;
			ctx->texture_height = slot->height;
		}
		if (ctx->texture)
			gs_texture_set_image(ctx->texture, slot->data, (uint32_t)slot->row_bytes, false);

		EnterCriticalSection(&ctx->frame_cs);
		slot->state = FRAME_SLOT_FREE;
		LeaveCriticalSection(&ctx->frame_cs);
	} else if (ctx->texture &&
		   (ctx->texture_width != current_width || ctx->texture_height != current_height)) {
		gs_texture_destroy(ctx->texture);
		ctx->texture = NULL;
		ctx->texture_width = 0;
		ctx->texture_height = 0;
	}

	if (!ctx->texture || !effect)
		return;

	const bool srgb_previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (image) {
		gs_effect_set_texture_srgb(image, ctx->texture);
		gs_draw_sprite(ctx->texture, 0, current_width, current_height);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb_previous);
}

static uint32_t source_get_width(void *data)
{
	struct flutter_source *ctx = data;
	EnterCriticalSection(&ctx->frame_cs);
	const uint32_t width = ctx->width;
	LeaveCriticalSection(&ctx->frame_cs);
	return width;
}

static uint32_t source_get_height(void *data)
{
	struct flutter_source *ctx = data;
	EnterCriticalSection(&ctx->frame_cs);
	const uint32_t height = ctx->height;
	LeaveCriticalSection(&ctx->frame_cs);
	return height;
}

static obs_properties_t *source_properties(void *data)
{
	(void)data;
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_int(properties, "width", "Width", 320, 3840, 1);
	obs_properties_add_int(properties, "height", "Height", 240, 2160, 1);
	obs_properties_add_int(properties, "pixel_ratio", "Pixel Ratio (%)", 25, 400, 5);
	obs_properties_add_text(properties, "dart_config", "Dart Config (JSON)", OBS_TEXT_MULTILINE);
	return properties;
}

static void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 640);
	obs_data_set_default_int(settings, "height", 480);
	obs_data_set_default_int(settings, "pixel_ratio", 100);
	obs_data_set_default_string(settings, "dart_config", "{}");
}

static void source_update(void *data, obs_data_t *settings)
{
	struct flutter_source *ctx = data;
	if (!ctx || InterlockedCompareExchange(&ctx->shutting_down, 0, 0))
		return;

	const uint32_t width = flutter_normalize_setting(obs_data_get_int(settings, "width"), 640, 320, 3840);
	const uint32_t height = flutter_normalize_setting(obs_data_get_int(settings, "height"), 480, 240, 2160);
	const uint32_t pixel_ratio = flutter_normalize_setting(obs_data_get_int(settings, "pixel_ratio"), 100, 25, 400);
	const char *config = obs_data_get_string(settings, "dart_config");
	if (!config || !config[0])
		config = "{}";
	char normalized_config[DART_CONFIG_CAPACITY];
	copy_string(normalized_config, sizeof(normalized_config), config, "Dart config");

	bool metrics_changed;
	EnterCriticalSection(&ctx->frame_cs);
	metrics_changed = width != ctx->width || height != ctx->height || pixel_ratio != ctx->pixel_ratio_pct;
	if (metrics_changed) {
		ctx->width = width;
		ctx->height = height;
		ctx->pixel_ratio_pct = pixel_ratio;
		++ctx->frame_generation;
		if (ctx->ready_frame >= 0 && ctx->frames[ctx->ready_frame].state == FRAME_SLOT_READY)
			ctx->frames[ctx->ready_frame].state = FRAME_SLOT_FREE;
		ctx->ready_frame = -1;
	}
	LeaveCriticalSection(&ctx->frame_cs);

	bool config_changed;
	char *config_copy = NULL;
	EnterCriticalSection(&ctx->config_cs);
	config_changed =
		strncmp(ctx->requested_dart_config, normalized_config, sizeof(ctx->requested_dart_config)) != 0;
	if (config_changed) {
		config_copy = duplicate_string(normalized_config);
		if (config_copy)
			copy_string(ctx->requested_dart_config, sizeof(ctx->requested_dart_config), normalized_config,
				    "Dart config");
		else
			config_changed = false;
	}
	LeaveCriticalSection(&ctx->config_cs);
	if (!config_copy &&
	    strncmp(ctx->requested_dart_config, normalized_config, sizeof(ctx->requested_dart_config)) != 0)
		blog(LOG_ERROR, "[FlutterSource] Unable to copy Dart config for the platform runner");

	if (!metrics_changed && !config_changed)
		return;

	command_t *command = command_create(CMD_UPDATE_ENGINE);
	if (!command) {
		free(config_copy);
		return;
	}
	command->width = width;
	command->height = height;
	command->pixel_ratio_pct = pixel_ratio;
	command->metrics_changed = metrics_changed;
	command->config_changed = config_changed;
	if (config_changed) {
		command->dart_config = config_copy;
		config_copy = NULL;
	}
	if (!worker_queue_push(&ctx->worker_queue, command, false))
		command_destroy(command);
	free(config_copy);
}

static void enqueue_active_state(struct flutter_source *ctx, bool active)
{
	if (!ctx || InterlockedCompareExchange(&ctx->shutting_down, 0, 0))
		return;
	if (active)
		InterlockedExchange(&ctx->audio_reanchor, 1);
	InterlockedExchange(&ctx->active, active ? 1 : 0);
	command_t *command = command_create(CMD_SET_ACTIVE);
	if (!command)
		return;
	command->active = active;
	if (!worker_queue_push(&ctx->worker_queue, command, false))
		command_destroy(command);
}

static void source_activate(void *data)
{
	enqueue_active_state(data, true);
}

static void source_deactivate(void *data)
{
	enqueue_active_state(data, false);
}

static int64_t flutter_buttons_from_obs(uint32_t modifiers)
{
	int64_t buttons = 0;
	if (modifiers & INTERACT_MOUSE_LEFT)
		buttons |= kFlutterPointerButtonMousePrimary;
	if (modifiers & INTERACT_MOUSE_RIGHT)
		buttons |= kFlutterPointerButtonMouseSecondary;
	if (modifiers & INTERACT_MOUSE_MIDDLE)
		buttons |= kFlutterPointerButtonMouseMiddle;
	return buttons;
}

static int64_t flutter_button_from_obs(int32_t type)
{
	switch (type) {
	case MOUSE_LEFT:
		return kFlutterPointerButtonMousePrimary;
	case MOUSE_RIGHT:
		return kFlutterPointerButtonMouseSecondary;
	case MOUSE_MIDDLE:
		return kFlutterPointerButtonMouseMiddle;
	default:
		return 0;
	}
}

static double mouse_scroll_pixels_per_tick(void)
{
	UINT lines_per_scroll = 3;
	if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines_per_scroll, 0))
		lines_per_scroll = 3;
	if (lines_per_scroll == WHEEL_PAGESCROLL)
		return 100.0;
	return (double)lines_per_scroll * 100.0 / 3.0;
}

static void enqueue_mouse_input(struct flutter_source *ctx, flutter_mouse_input input)
{
	if (!ctx || InterlockedCompareExchange(&ctx->shutting_down, 0, 0))
		return;

	command_t *command = command_create(CMD_MOUSE_INPUT);
	if (!command)
		return;
	command->target_time_ns = FlutterEngineGetCurrentTime();
	input.timestamp_us = command->target_time_ns / 1000;
	command->mouse_input = input;
	if (!worker_queue_push_mouse(&ctx->worker_queue, command))
		command_destroy(command);
}

static void source_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			       uint32_t click_count)
{
	(void)click_count;
	struct flutter_source *ctx = data;
	if (!ctx || !event)
		return;

	const int64_t changed_button = flutter_button_from_obs(type);
	if (!changed_button)
		return;
	int64_t buttons = flutter_buttons_from_obs(event->modifiers);
	buttons = mouse_up ? buttons & ~changed_button : buttons | changed_button;
	enqueue_mouse_input(ctx, (flutter_mouse_input){
		.type = FLUTTER_MOUSE_INPUT_BUTTON,
		.x = event->x,
		.y = event->y,
		.buttons = buttons,
	});
}

static void source_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct flutter_source *ctx = data;
	if (!ctx || (!event && !mouse_leave))
		return;

	flutter_mouse_input input = {
		.type = mouse_leave ? FLUTTER_MOUSE_INPUT_LEAVE : FLUTTER_MOUSE_INPUT_MOVE,
	};
	if (!mouse_leave) {
		input.x = event->x;
		input.y = event->y;
		input.buttons = flutter_buttons_from_obs(event->modifiers);
	}
	enqueue_mouse_input(ctx, input);
}

static void source_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	struct flutter_source *ctx = data;
	if (!ctx || !event)
		return;

	const double pixels_per_tick = mouse_scroll_pixels_per_tick();
	enqueue_mouse_input(ctx, (flutter_mouse_input){
		.type = FLUTTER_MOUSE_INPUT_WHEEL,
		.x = event->x,
		.y = event->y,
		.buttons = flutter_buttons_from_obs(event->modifiers),
		.scroll_delta_x = (double)x_delta * pixels_per_tick / WHEEL_DELTA,
		.scroll_delta_y = -(double)y_delta * pixels_per_tick / WHEEL_DELTA,
	});
}

static void source_focus(void *data, bool focus)
{
	enqueue_mouse_input(data, (flutter_mouse_input){
		.type = FLUTTER_MOUSE_INPUT_FOCUS,
		.focused = focus,
	});
}

struct obs_source_info flutter_source_info = {
	.id = "flutter_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_AUDIO | OBS_SOURCE_INTERACTION,
	.get_name = source_get_name,
	.create = source_create,
	.destroy = source_destroy,
	.video_render = source_render,
	.get_defaults = source_defaults,
	.get_width = source_get_width,
	.get_height = source_get_height,
	.update = source_update,
	.get_properties = source_properties,
	.activate = source_activate,
	.deactivate = source_deactivate,
	.mouse_click = source_mouse_click,
	.mouse_move = source_mouse_move,
	.mouse_wheel = source_mouse_wheel,
	.focus = source_focus,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};
