#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLUTTER_MAX_SOUNDS 256
#define FLUTTER_AUDIO_PATH_CAPACITY 1024
#define FLUTTER_AUDIO_SESSION_CAPACITY 160

typedef enum {
	FLUTTER_AUDIO_CMD_LOAD,
	FLUTTER_AUDIO_CMD_PLAY,
	FLUTTER_AUDIO_CMD_PAUSE,
	FLUTTER_AUDIO_CMD_RESUME,
	FLUTTER_AUDIO_CMD_SEEK,
	FLUTTER_AUDIO_CMD_STOP,
	FLUTTER_AUDIO_CMD_VOLUME,
	FLUTTER_AUDIO_CMD_RELEASE,
} flutter_audio_cmd_type;

typedef struct {
	flutter_audio_cmd_type type;
	int id;
	float volume;
	uint64_t position_ms;
	bool loop;
	bool is_relative;
	char path[FLUTTER_AUDIO_PATH_CAPACITY];
	char session_id[FLUTTER_AUDIO_SESSION_CAPACITY];
} flutter_audio_cmd;

bool flutter_checked_frame_size(uint32_t width, uint32_t height, size_t *size_out);
uint32_t flutter_normalize_setting(int64_t value, uint32_t fallback, uint32_t minimum, uint32_t maximum);
bool flutter_parse_audio_json(const char *data, size_t length, flutter_audio_cmd *output);
/* Returns a malloc-allocated JSON message. The caller owns the result. */
char *flutter_create_audio_event_json(const char *event, int id, const char *session_id, const char *message);
