#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime-validation.h"
#include "third_party/cjson/cJSON.h"

#define CHECK(condition)                                                                                              \
	do {                                                                                                           \
		if (!(condition)) {                                                                                     \
			fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition);                         \
			return 1;                                                                                          \
		}                                                                                                      \
	} while (0)

int main(void)
{
	CHECK(flutter_normalize_setting(-1, 640, 320, 3840) == 640);
	CHECK(flutter_normalize_setting(100, 640, 320, 3840) == 320);
	CHECK(flutter_normalize_setting(5000, 640, 320, 3840) == 3840);
	CHECK(flutter_normalize_setting(1920, 640, 320, 3840) == 1920);

	size_t frame_size = 0;
	CHECK(flutter_checked_frame_size(1920, 1080, &frame_size));
	CHECK(frame_size == 1920u * 1080u * 4u);
	CHECK(!flutter_checked_frame_size(0, 1080, &frame_size));
	CHECK(!flutter_checked_frame_size(UINT32_MAX, UINT32_MAX, &frame_size));

	flutter_audio_cmd command;
	const char *play = "{\"cmd\":\"play\",\"id\":7,\"volume\":0.5,\"loop\":true}";
	CHECK(flutter_parse_audio_json(play, strlen(play), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_PLAY);
	CHECK(command.id == 7);
	CHECK(fabsf(command.volume - 0.5f) < 0.001f);
	CHECK(command.loop);

	const char *pause = "{\"cmd\":\"pause\",\"id\":7}";
	CHECK(flutter_parse_audio_json(pause, strlen(pause), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_PAUSE);
	CHECK(command.id == 7);

	const char *resume = "{\"cmd\":\"resume\",\"id\":7}";
	CHECK(flutter_parse_audio_json(resume, strlen(resume), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_RESUME);
	CHECK(command.id == 7);

	const char *seek = "{\"cmd\":\"seek\",\"id\":7,\"position_ms\":90500}";
	CHECK(flutter_parse_audio_json(seek, strlen(seek), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_SEEK);
	CHECK(command.id == 7);
	CHECK(command.position_ms == 90500);

	const char *load = "{\"cmd\":\"load\",\"id\":2,\"asset\":\"assets/sound.wav\"}";
	CHECK(flutter_parse_audio_json(load, strlen(load), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_LOAD);
	CHECK(command.is_relative);
	CHECK(strcmp(command.path, "assets/sound.wav") == 0);

	const char *load_with_session =
		"{\"cmd\":\"load\",\"id\":2,\"asset\":\"assets/sound.wav\",\"session_id\":\"track-2\"}";
	CHECK(flutter_parse_audio_json(load_with_session, strlen(load_with_session), &command));
	CHECK(strcmp(command.session_id, "track-2") == 0);

	char *event_json = flutter_create_audio_event_json("error", 2, "track-2", "decode failed");
	CHECK(event_json);
	cJSON *event = cJSON_Parse(event_json);
	free(event_json);
	CHECK(event);
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "event")), "error") == 0);
	CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(event, "id")) == 2);
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "session_id")), "track-2") == 0);
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "message")), "decode failed") == 0);
	cJSON_Delete(event);

	char escaped_session[FLUTTER_AUDIO_SESSION_CAPACITY];
	memset(escaped_session, 1, sizeof(escaped_session) - 1);
	escaped_session[sizeof(escaped_session) - 1] = '\0';
	event_json = flutter_create_audio_event_json("loaded", 2, escaped_session, NULL);
	CHECK(event_json);
	event = cJSON_Parse(event_json);
	free(event_json);
	CHECK(event);
	CHECK(strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "session_id"))) ==
	      sizeof(escaped_session) - 1);
	cJSON_Delete(event);
	CHECK(!flutter_create_audio_event_json("progress", 2, "track-2", NULL));

	const char *clamped = "{\"cmd\":\"volume\",\"id\":2,\"volume\":99}";
	CHECK(flutter_parse_audio_json(clamped, strlen(clamped), &command));
	CHECK(command.volume == 4.0f);

	const char *release = "{\"cmd\":\"release\",\"id\":2}";
	CHECK(flutter_parse_audio_json(release, strlen(release), &command));
	CHECK(command.type == FLUTTER_AUDIO_CMD_RELEASE);

	const char *negative_id = "{\"cmd\":\"play\",\"id\":-1}";
	CHECK(!flutter_parse_audio_json(negative_id, strlen(negative_id), &command));
	const char *large_id = "{\"cmd\":\"stop\",\"id\":256}";
	CHECK(!flutter_parse_audio_json(large_id, strlen(large_id), &command));
	const char *missing_path = "{\"cmd\":\"load\",\"id\":0}";
	CHECK(!flutter_parse_audio_json(missing_path, strlen(missing_path), &command));
	const char *missing_position = "{\"cmd\":\"seek\",\"id\":0}";
	CHECK(!flutter_parse_audio_json(missing_position, strlen(missing_position), &command));
	const char *negative_position = "{\"cmd\":\"seek\",\"id\":0,\"position_ms\":-1}";
	CHECK(!flutter_parse_audio_json(negative_position, strlen(negative_position), &command));
	CHECK(!flutter_parse_audio_json("not-json", 8, &command));
	CHECK(!flutter_parse_audio_json("{}", 2, &command));

	return 0;
}
