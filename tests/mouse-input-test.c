#include <math.h>
#include <stdio.h>
#include <string.h>

#include "flutter-mouse.h"

#define CHECK(condition)                                                                                              \
	do {                                                                                                           \
		if (!(condition)) {                                                                                     \
			fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition);                         \
			return 1;                                                                                          \
		}                                                                                                      \
	} while (0)

static flutter_mouse_input input(flutter_mouse_input_type type, uint64_t timestamp_us, double x, double y,
				 int64_t buttons)
{
	return (flutter_mouse_input){
		.type = type,
		.timestamp_us = timestamp_us,
		.x = x,
		.y = y,
		.buttons = buttons,
	};
}

static int test_hover_adds_pointer(void)
{
	flutter_mouse_state state;
	flutter_mouse_output output;
	flutter_mouse_state_init(&state);

	flutter_mouse_input move = input(FLUTTER_MOUSE_INPUT_MOVE, 10, 25, 40, 0);
	flutter_mouse_translate(&state, &move, &output);
	CHECK(output.pointer_event_count == 2);
	CHECK(output.pointer_events[0].phase == kAdd);
	CHECK(output.pointer_events[0].buttons == 0);
	CHECK(output.pointer_events[1].phase == kHover);
	CHECK(output.pointer_events[1].x == 25);
	CHECK(output.pointer_events[1].y == 40);
	CHECK(output.pointer_events[1].device_kind == kFlutterPointerDeviceKindMouse);
	CHECK(output.pointer_events[1].view_id == 0);
	return 0;
}

static int test_button_sequence_and_drag(void)
{
	flutter_mouse_state state;
	flutter_mouse_output output;
	flutter_mouse_state_init(&state);

	flutter_mouse_input primary_down =
		input(FLUTTER_MOUSE_INPUT_BUTTON, 20, 10, 15, kFlutterPointerButtonMousePrimary);
	flutter_mouse_translate(&state, &primary_down, &output);
	CHECK(output.pointer_event_count == 2);
	CHECK(output.pointer_events[0].phase == kAdd);
	CHECK(output.pointer_events[1].phase == kDown);
	CHECK(output.pointer_events[1].buttons == kFlutterPointerButtonMousePrimary);

	flutter_mouse_input drag = input(FLUTTER_MOUSE_INPUT_MOVE, 21, 30, 35, kFlutterPointerButtonMousePrimary);
	flutter_mouse_translate(&state, &drag, &output);
	CHECK(output.pointer_event_count == 1);
	CHECK(output.pointer_events[0].phase == kMove);

	flutter_mouse_input secondary_down = input(FLUTTER_MOUSE_INPUT_BUTTON, 22, 30, 35,
						      kFlutterPointerButtonMousePrimary |
							      kFlutterPointerButtonMouseSecondary);
	flutter_mouse_translate(&state, &secondary_down, &output);
	CHECK(output.pointer_event_count == 1);
	CHECK(output.pointer_events[0].phase == kMove);

	flutter_mouse_input primary_up =
		input(FLUTTER_MOUSE_INPUT_BUTTON, 23, 30, 35, kFlutterPointerButtonMouseSecondary);
	flutter_mouse_translate(&state, &primary_up, &output);
	CHECK(output.pointer_event_count == 1);
	CHECK(output.pointer_events[0].phase == kMove);

	flutter_mouse_input secondary_up = input(FLUTTER_MOUSE_INPUT_BUTTON, 24, 30, 35, 0);
	flutter_mouse_translate(&state, &secondary_up, &output);
	CHECK(output.pointer_event_count == 1);
	CHECK(output.pointer_events[0].phase == kUp);
	CHECK(output.pointer_events[0].buttons == 0);
	return 0;
}

static int test_wheel_uses_pointer_signal(void)
{
	flutter_mouse_state state;
	flutter_mouse_output output;
	flutter_mouse_state_init(&state);

	flutter_mouse_input wheel = input(FLUTTER_MOUSE_INPUT_WHEEL, 30, 50, 60, 0);
	wheel.scroll_delta_x = 25;
	wheel.scroll_delta_y = -100;
	flutter_mouse_translate(&state, &wheel, &output);
	CHECK(output.pointer_event_count == 2);
	CHECK(output.pointer_events[0].phase == kAdd);
	CHECK(output.pointer_events[1].phase == kHover);
	CHECK(output.pointer_events[1].signal_kind == kFlutterPointerSignalKindScroll);
	CHECK(fabs(output.pointer_events[1].scroll_delta_x - 25) < 0.001);
	CHECK(fabs(output.pointer_events[1].scroll_delta_y + 100) < 0.001);
	return 0;
}

static int test_leave_and_focus_loss_reset_pointer(void)
{
	flutter_mouse_state state;
	flutter_mouse_output output;
	flutter_mouse_state_init(&state);

	flutter_mouse_input down =
		input(FLUTTER_MOUSE_INPUT_BUTTON, 40, 70, 80, kFlutterPointerButtonMousePrimary);
	flutter_mouse_translate(&state, &down, &output);

	flutter_mouse_input focus_out = input(FLUTTER_MOUSE_INPUT_FOCUS, 41, 0, 0, 0);
	focus_out.focused = false;
	flutter_mouse_translate(&state, &focus_out, &output);
	CHECK(output.pointer_event_count == 2);
	CHECK(output.pointer_events[0].phase == kCancel);
	CHECK(output.pointer_events[0].x == 70);
	CHECK(output.pointer_events[0].y == 80);
	CHECK(output.pointer_events[1].phase == kRemove);
	CHECK(output.has_focus_event);
	CHECK(output.focus_event.state == kUnfocused);

	flutter_mouse_input late_up = input(FLUTTER_MOUSE_INPUT_BUTTON, 42, 75, 85, 0);
	flutter_mouse_translate(&state, &late_up, &output);
	CHECK(output.pointer_event_count == 0);

	flutter_mouse_input move = input(FLUTTER_MOUSE_INPUT_MOVE, 43, 90, 100, 0);
	flutter_mouse_translate(&state, &move, &output);
	CHECK(output.pointer_event_count == 2);
	CHECK(output.pointer_events[0].phase == kAdd);

	flutter_mouse_input leave = input(FLUTTER_MOUSE_INPUT_LEAVE, 44, 0, 0, 0);
	flutter_mouse_translate(&state, &leave, &output);
	CHECK(output.pointer_event_count == 1);
	CHECK(output.pointer_events[0].phase == kRemove);
	CHECK(output.pointer_events[0].x == 90);
	CHECK(output.pointer_events[0].y == 100);
	return 0;
}

static int test_focus_changes_are_deduplicated(void)
{
	flutter_mouse_state state;
	flutter_mouse_output output;
	flutter_mouse_state_init(&state);

	flutter_mouse_input focus_in = input(FLUTTER_MOUSE_INPUT_FOCUS, 50, 0, 0, 0);
	focus_in.focused = true;
	flutter_mouse_translate(&state, &focus_in, &output);
	CHECK(output.has_focus_event);
	CHECK(output.focus_event.state == kFocused);
	CHECK(output.focus_event.direction == kUndefined);

	flutter_mouse_translate(&state, &focus_in, &output);
	CHECK(!output.has_focus_event);
	CHECK(output.pointer_event_count == 0);
	return 0;
}

int main(void)
{
	CHECK(test_hover_adds_pointer() == 0);
	CHECK(test_button_sequence_and_drag() == 0);
	CHECK(test_wheel_uses_pointer_signal() == 0);
	CHECK(test_leave_and_focus_loss_reset_pointer() == 0);
	CHECK(test_focus_changes_are_deduplicated() == 0);
	return 0;
}
