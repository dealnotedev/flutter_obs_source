#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct {
	int client_id;
	uint64_t generation;
	wchar_t *text;
	int selection_base;
	int selection_extent;
	bool multiline;
	bool delta_model;
	char action[80];
	void *clipboard_window;
} flutter_text_input;

typedef void (*flutter_text_send)(const char *message, void *user_data);

void flutter_text_input_init(flutter_text_input *input);
void flutter_text_input_destroy(flutter_text_input *input);
// Returns an allocated JSONMethodCodec envelope, or NULL for an unknown method.
char *flutter_text_input_handle(flutter_text_input *input, const char *message, size_t size);
char *flutter_clipboard_handle(flutter_text_input *input, const char *message, size_t size);
// Called only after the framework declines a key. Editing shortcuts run in Dart.
void flutter_text_input_key(flutter_text_input *input, uint32_t virtual_key, uint32_t modifiers, const char *text,
			    flutter_text_send send, void *user_data);
