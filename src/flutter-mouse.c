#include "flutter-mouse.h"

#include <string.h>

#define FLUTTER_IMPLICIT_VIEW_ID 0
#define FLUTTER_MOUSE_DEVICE_ID 0

static FlutterPointerEvent pointer_event(const flutter_mouse_input *input, FlutterPointerPhase phase,
					 int64_t buttons)
{
	return (FlutterPointerEvent){
		.struct_size = sizeof(FlutterPointerEvent),
		.phase = phase,
		.timestamp = (size_t)input->timestamp_us,
		.x = input->x,
		.y = input->y,
		.device = FLUTTER_MOUSE_DEVICE_ID,
		.signal_kind = kFlutterPointerSignalKindNone,
		.device_kind = kFlutterPointerDeviceKindMouse,
		.buttons = buttons,
		.view_id = FLUTTER_IMPLICIT_VIEW_ID,
	};
}

static void append_pointer_event(flutter_mouse_output *output, FlutterPointerEvent event)
{
	if (output->pointer_event_count < FLUTTER_MOUSE_MAX_POINTER_EVENTS)
		output->pointer_events[output->pointer_event_count++] = event;
}

static void ensure_pointer_added(flutter_mouse_state *state, const flutter_mouse_input *input,
				 flutter_mouse_output *output)
{
	if (state->pointer_added)
		return;

	append_pointer_event(output, pointer_event(input, kAdd, 0));
	state->pointer_added = true;
}

static FlutterPointerPhase phase_for_buttons(const flutter_mouse_state *state, int64_t buttons)
{
	if (buttons == 0)
		return state->pointer_down ? kUp : kHover;
	return state->pointer_down ? kMove : kDown;
}

static void remove_pointer(flutter_mouse_state *state, const flutter_mouse_input *input,
			   flutter_mouse_output *output)
{
	if (!state->pointer_added) {
		state->pointer_down = false;
		state->buttons = 0;
		return;
	}

	flutter_mouse_input positioned = *input;
	positioned.x = state->x;
	positioned.y = state->y;
	if (state->pointer_down)
		append_pointer_event(output, pointer_event(&positioned, kCancel, state->buttons));

	state->pointer_down = false;
	state->buttons = 0;
	append_pointer_event(output, pointer_event(&positioned, kRemove, 0));
	state->pointer_added = false;
}

void flutter_mouse_state_init(flutter_mouse_state *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

void flutter_mouse_translate(flutter_mouse_state *state, const flutter_mouse_input *input,
			     flutter_mouse_output *output)
{
	if (!output)
		return;
	memset(output, 0, sizeof(*output));
	if (!state || !input)
		return;

	if (input->type == FLUTTER_MOUSE_INPUT_FOCUS) {
		if (!input->focused)
			remove_pointer(state, input, output);

		if (!state->focus_known || state->focused != input->focused) {
			output->has_focus_event = true;
			output->focus_event = (FlutterViewFocusEvent){
				.struct_size = sizeof(FlutterViewFocusEvent),
				.view_id = FLUTTER_IMPLICIT_VIEW_ID,
				.state = input->focused ? kFocused : kUnfocused,
				.direction = kUndefined,
			};
			state->focus_known = true;
			state->focused = input->focused;
		}
		return;
	}

	if (input->type == FLUTTER_MOUSE_INPUT_LEAVE) {
		remove_pointer(state, input, output);
		return;
	}

	const int64_t previous_buttons = state->buttons;
	if (input->type == FLUTTER_MOUSE_INPUT_BUTTON && !state->pointer_added && input->buttons == 0) {
		state->pointer_down = false;
		state->buttons = 0;
		return;
	}
	if (input->type == FLUTTER_MOUSE_INPUT_BUTTON && input->buttons == previous_buttons)
		return;

	state->x = input->x;
	state->y = input->y;
	state->buttons = input->buttons;
	ensure_pointer_added(state, input, output);

	FlutterPointerEvent event = pointer_event(input, phase_for_buttons(state, state->buttons), state->buttons);
	if (input->type == FLUTTER_MOUSE_INPUT_WHEEL) {
		event.signal_kind = kFlutterPointerSignalKindScroll;
		event.scroll_delta_x = input->scroll_delta_x;
		event.scroll_delta_y = input->scroll_delta_y;
	}
	append_pointer_event(output, event);

	if (event.phase == kDown)
		state->pointer_down = true;
	else if (event.phase == kUp || event.phase == kCancel)
		state->pointer_down = false;
}
