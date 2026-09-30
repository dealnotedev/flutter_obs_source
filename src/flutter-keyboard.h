#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "flutter_embedder.h"

// Modifier snapshot captured on the OBS event thread, before queueing the event.
enum {
	FLUTTER_KEY_SHIFT_LEFT = 1 << 0,
	FLUTTER_KEY_SHIFT_RIGHT = 1 << 1,
	FLUTTER_KEY_CONTROL_LEFT = 1 << 2,
	FLUTTER_KEY_CONTROL_RIGHT = 1 << 3,
	FLUTTER_KEY_ALT_LEFT = 1 << 4,
	FLUTTER_KEY_ALT_RIGHT = 1 << 5,
	FLUTTER_KEY_META_LEFT = 1 << 6,
	FLUTTER_KEY_META_RIGHT = 1 << 7,
	FLUTTER_KEY_CAPS_LOCK = 1 << 8,
	FLUTTER_KEY_NUM_LOCK = 1 << 9,
	FLUTTER_KEY_SCROLL_LOCK = 1 << 10,
};

typedef struct {
	uint64_t timestamp_us;
	uint32_t scan_code;
	uint32_t virtual_key;
	uint32_t modifiers;
	bool key_up;
	const char *text;
} flutter_key_input;

#define FLUTTER_KEY_SLOTS 768
#define FLUTTER_KEY_MAX_EVENTS 20
#define FLUTTER_KEY_RAW_MESSAGE_CAPACITY 256

typedef struct {
	uint64_t physical;
	uint64_t logical;
} flutter_pressed_key;

typedef struct {
	flutter_pressed_key pressed[FLUTTER_KEY_SLOTS];
	uint32_t locks;
} flutter_keyboard_state;

typedef struct {
	FlutterKeyEvent events[FLUTTER_KEY_MAX_EVENTS];
	size_t count;
	// Send after the key data events. Flutter dispatches ordinary keys when
	// this Windows JSON message arrives on flutter/keyevent.
	char raw_message[FLUTTER_KEY_RAW_MESSAGE_CAPACITY];
} flutter_key_output;

void flutter_keyboard_init(flutter_keyboard_state *state);
void flutter_keyboard_translate(flutter_keyboard_state *state, const flutter_key_input *input,
				flutter_key_output *output);
bool flutter_keyboard_release_next(flutter_keyboard_state *state, uint64_t timestamp_us, FlutterKeyEvent *event);
