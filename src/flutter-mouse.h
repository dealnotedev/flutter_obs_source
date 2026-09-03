#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flutter_embedder.h"

#define FLUTTER_MOUSE_MAX_POINTER_EVENTS 2

typedef enum {
	FLUTTER_MOUSE_INPUT_MOVE,
	FLUTTER_MOUSE_INPUT_BUTTON,
	FLUTTER_MOUSE_INPUT_WHEEL,
	FLUTTER_MOUSE_INPUT_LEAVE,
	FLUTTER_MOUSE_INPUT_FOCUS,
} flutter_mouse_input_type;

typedef struct {
	flutter_mouse_input_type type;
	uint64_t timestamp_us;
	double x;
	double y;
	int64_t buttons;
	double scroll_delta_x;
	double scroll_delta_y;
	bool focused;
} flutter_mouse_input;

typedef struct {
	bool pointer_added;
	bool pointer_down;
	bool focus_known;
	bool focused;
	int64_t buttons;
	double x;
	double y;
} flutter_mouse_state;

typedef struct {
	FlutterPointerEvent pointer_events[FLUTTER_MOUSE_MAX_POINTER_EVENTS];
	size_t pointer_event_count;
	bool has_focus_event;
	FlutterViewFocusEvent focus_event;
} flutter_mouse_output;

void flutter_mouse_state_init(flutter_mouse_state *state);
void flutter_mouse_translate(flutter_mouse_state *state, const flutter_mouse_input *input,
			     flutter_mouse_output *output);
