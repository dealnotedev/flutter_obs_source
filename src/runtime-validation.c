#include "runtime-validation.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "./third_party/cjson/cJSON.h"

#define FLUTTER_AUDIO_MAX_POSITION_MS 86400000.0

bool flutter_checked_frame_size(uint32_t width, uint32_t height, size_t *size_out)
{
	if (!size_out || !width || !height || width > SIZE_MAX / 4 || height > SIZE_MAX / ((size_t)width * 4))
		return false;
	*size_out = (size_t)width * (size_t)height * 4;
	return true;
}

uint32_t flutter_normalize_setting(int64_t value, uint32_t fallback, uint32_t minimum, uint32_t maximum)
{
	if (value <= 0)
		return fallback;
	if ((uint64_t)value < minimum)
		return minimum;
	if ((uint64_t)value > maximum)
		return maximum;
	return (uint32_t)value;
}

static bool copy_path(char *destination, size_t capacity, const char *source)
{
	const size_t length = strlen(source);
	if (length >= capacity)
		return false;
	memcpy(destination, source, length + 1);
	return true;
}

bool flutter_parse_audio_json(const char *data, size_t length, flutter_audio_cmd *output)
{
	if (!output)
		return false;
	memset(output, 0, sizeof(*output));
	output->volume = 1.0f;

	if (!data || !length || length > INT_MAX)
		return false;
	cJSON *root = cJSON_ParseWithLength(data, (int)length);
	if (!root)
		return false;

	bool valid = false;
	const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsString(command) || !command->valuestring || !cJSON_IsNumber(id) || id->valueint < 0 ||
	    id->valueint >= FLUTTER_MAX_SOUNDS)
		goto done;

	output->id = id->valueint;
	if (strcmp(command->valuestring, "load") == 0)
		output->type = FLUTTER_AUDIO_CMD_LOAD;
	else if (strcmp(command->valuestring, "play") == 0)
		output->type = FLUTTER_AUDIO_CMD_PLAY;
	else if (strcmp(command->valuestring, "pause") == 0)
		output->type = FLUTTER_AUDIO_CMD_PAUSE;
	else if (strcmp(command->valuestring, "resume") == 0)
		output->type = FLUTTER_AUDIO_CMD_RESUME;
	else if (strcmp(command->valuestring, "seek") == 0)
		output->type = FLUTTER_AUDIO_CMD_SEEK;
	else if (strcmp(command->valuestring, "stop") == 0)
		output->type = FLUTTER_AUDIO_CMD_STOP;
	else if (strcmp(command->valuestring, "volume") == 0)
		output->type = FLUTTER_AUDIO_CMD_VOLUME;
	else
		goto done;

	const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
	if (volume) {
		if (!cJSON_IsNumber(volume) || !isfinite(volume->valuedouble))
			goto done;
		output->volume = (float)volume->valuedouble;
		if (output->volume < 0.0f)
			output->volume = 0.0f;
		if (output->volume > 4.0f)
			output->volume = 4.0f;
	}

	const cJSON *loop = cJSON_GetObjectItemCaseSensitive(root, "loop");
	if (loop && !cJSON_IsBool(loop))
		goto done;
	output->loop = loop ? cJSON_IsTrue(loop) : false;

	if (output->type == FLUTTER_AUDIO_CMD_SEEK) {
		const cJSON *position = cJSON_GetObjectItemCaseSensitive(root, "position_ms");
		if (!cJSON_IsNumber(position) || !isfinite(position->valuedouble) || position->valuedouble < 0.0 ||
		    position->valuedouble > FLUTTER_AUDIO_MAX_POSITION_MS)
			goto done;
		output->position_ms = (uint64_t)position->valuedouble;
	}

	if (output->type == FLUTTER_AUDIO_CMD_LOAD) {
		const cJSON *absolute_path = cJSON_GetObjectItemCaseSensitive(root, "absolute_path");
		const cJSON *asset = cJSON_GetObjectItemCaseSensitive(root, "asset");
		if (cJSON_IsString(absolute_path) && absolute_path->valuestring && absolute_path->valuestring[0]) {
			if (!copy_path(output->path, sizeof(output->path), absolute_path->valuestring))
				goto done;
			output->is_relative = false;
		} else if (cJSON_IsString(asset) && asset->valuestring && asset->valuestring[0]) {
			if (!copy_path(output->path, sizeof(output->path), asset->valuestring))
				goto done;
			output->is_relative = true;
		} else {
			goto done;
		}
	}

	valid = true;

done:
	cJSON_Delete(root);
	return valid;
}
