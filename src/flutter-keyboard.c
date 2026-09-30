#include "flutter-keyboard.h"

#include <string.h>
#include <stdio.h>
#include <windows.h>

typedef struct {
	uint32_t native;
	uint64_t flutter;
} key_mapping;

#include "third_party/flutter/key-map.h"

#define WINDOWS_KEY_PLANE UINT64_C(0x01600000000)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static uint64_t lookup(const key_mapping *map, size_t count, uint32_t native)
{
	for (size_t i = 0; i < count; ++i)
		if (map[i].native == native)
			return map[i].flutter;
	return 0;
}

static uint32_t normalize_scan(uint32_t scan)
{
	// Qt sets bit 8 for an extended Windows scan code. Flutter's table uses E0.
	return (scan & 0xff) | ((scan & 0xff00) ? 0xe000 : 0);
}

static size_t key_slot(uint32_t scan, uint32_t key)
{
	return scan ? (scan & 0xff) + ((scan & 0xe000) ? 256 : 0) : 512 + (key & 0xff);
}

static uint32_t side_specific_key(uint32_t scan, uint32_t key)
{
	if (key == VK_SHIFT)
		return scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
	if (key == VK_CONTROL)
		return scan == 0xe01d ? VK_RCONTROL : VK_LCONTROL;
	if (key == VK_MENU)
		return scan == 0xe038 ? VK_RMENU : VK_LMENU;
	return key;
}

static flutter_pressed_key key_ids(uint32_t scan, uint32_t key)
{
	uint64_t physical = lookup(windows_physical_keys, ARRAY_SIZE(windows_physical_keys), scan);
	uint64_t logical = lookup(windows_scan_logical_keys, ARRAY_SIZE(windows_scan_logical_keys), scan);
	if (scan == 0xe01c)
		logical = UINT64_C(0x0020000020d); // numpad Enter
	key = side_specific_key(scan, key);
	if (!logical)
		logical = lookup(windows_logical_keys, ARRAY_SIZE(windows_logical_keys), key);
	if (!logical) {
		if (key >= 'A' && key <= 'Z')
			logical = key + ('a' - 'A');
		else if (key >= '0' && key <= '9')
			logical = key;
		else
			logical = WINDOWS_KEY_PLANE | key;
	}
	return (flutter_pressed_key){physical ? physical : WINDOWS_KEY_PLANE | (scan ? scan : key), logical};
}

static FlutterKeyEvent make_event(flutter_pressed_key key, uint64_t timestamp, FlutterKeyEventType type,
				  bool synthesized, const char *text)
{
	return (FlutterKeyEvent){
		.struct_size = sizeof(FlutterKeyEvent),
		.timestamp = (double)timestamp,
		.type = type,
		.physical = key.physical,
		.logical = key.logical,
		.character = text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7f ? text : NULL,
		.synthesized = synthesized,
		.device_type = kFlutterKeyEventDeviceTypeKeyboard,
	};
}

static void synthesize(flutter_keyboard_state *state, flutter_key_output *output, uint32_t scan, uint32_t key,
		       bool down, uint64_t timestamp)
{
	flutter_pressed_key *pressed = &state->pressed[key_slot(scan, key)];
	if (down == (pressed->physical != 0))
		return;
	flutter_pressed_key ids = down ? key_ids(scan, key) : *pressed;
	output->events[output->count++] =
		make_event(ids, timestamp, down ? kFlutterKeyEventTypeDown : kFlutterKeyEventTypeUp, true, NULL);
	*pressed = down ? ids : (flutter_pressed_key){0};
}

static uint32_t first_code_point(const char *text)
{
	if (!text || !text[0])
		return 0;
	const unsigned char lead = (unsigned char)text[0];
	const int bytes = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
	if (strlen(text) < (size_t)bytes)
		return 0;
	wchar_t units[2];
	const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, bytes, units, 2);
	if (count == 2)
		return 0x10000u + (((uint32_t)units[0] - 0xd800u) << 10) + ((uint32_t)units[1] - 0xdc00u);
	return count == 1 ? (uint32_t)units[0] : 0;
}

static void format_raw_message(const flutter_keyboard_state *state, const flutter_key_input *input, uint32_t scan,
			       flutter_key_output *output)
{
	// RawKeyEventDataWindows uses different modifier bits from OBS and from
	// our snapshot. Lock bits here mean physically pressed, not toggled on.
	const uint32_t scans[] = {0x2a, 0x36, 0x1d, 0xe01d, 0x38, 0xe038, 0xe05b, 0xe05c, 0x3a, 0xe045, 0x46};
	const uint32_t masks[] = {0x003, 0x005, 0x018, 0x028, 0x0c0, 0x140, 0x200, 0x400, 0x800, 0x1000, 0x2000};
	uint32_t modifiers = 0;
	for (size_t i = 0; i < ARRAY_SIZE(scans); ++i)
		if (state->pressed[key_slot(scans[i], 0)].physical)
			modifiers |= masks[i];
	snprintf(output->raw_message, sizeof(output->raw_message),
		 "{\"keymap\":\"windows\",\"type\":\"%s\",\"keyCode\":%u,\"scanCode\":%u,"
		 "\"characterCodePoint\":%u,\"modifiers\":%u}",
		 input->key_up ? "keyup" : "keydown", side_specific_key(scan, input->virtual_key), scan,
		 input->key_up ? 0 : first_code_point(input->text), modifiers);
}

void flutter_keyboard_init(flutter_keyboard_state *state)
{
	memset(state, 0, sizeof(*state));
}

void flutter_keyboard_translate(flutter_keyboard_state *state, const flutter_key_input *input,
				flutter_key_output *output)
{
	memset(output, 0, sizeof(*output));
	if (!input->virtual_key || input->virtual_key > 255 || input->virtual_key == VK_PROCESSKEY)
		return;
	const uint32_t scan = normalize_scan(input->scan_code);
	const size_t slot = key_slot(scan, input->virtual_key);
	const uint32_t modifier_scans[] = {0x2a, 0x36, 0x1d, 0xe01d, 0x38, 0xe038, 0xe05b, 0xe05c};
	const uint32_t modifier_keys[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
					  VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN};
	for (size_t i = 0; i < ARRAY_SIZE(modifier_scans); ++i) {
		if (scan != modifier_scans[i])
			synthesize(state, output, modifier_scans[i], modifier_keys[i],
				   (input->modifiers & (1u << i)) != 0, input->timestamp_us);
	}

	const uint32_t lock_scans[] = {0x3a, 0xe045, 0x46};
	const uint32_t lock_keys[] = {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL};
	for (size_t i = 0; i < ARRAY_SIZE(lock_scans); ++i) {
		const uint32_t flag = FLUTTER_KEY_CAPS_LOCK << i;
		bool desired = (input->modifiers & flag) != 0;
		// A real down toggles the framework lock, so synchronize its preceding state.
		if (scan == lock_scans[i] && !input->key_up && !state->pressed[slot].physical)
			desired = !desired;
		if (desired != ((state->locks & flag) != 0)) {
			synthesize(state, output, lock_scans[i], lock_keys[i], false, input->timestamp_us);
			synthesize(state, output, lock_scans[i], lock_keys[i], true, input->timestamp_us);
			synthesize(state, output, lock_scans[i], lock_keys[i], false, input->timestamp_us);
			state->locks ^= flag;
		}
	}

	flutter_pressed_key *pressed = &state->pressed[slot];
	if (input->key_up && !pressed->physical)
		return; // A late release after focus loss, or a key pressed outside Interact.
	const FlutterKeyEventType type = input->key_up       ? kFlutterKeyEventTypeUp
					 : pressed->physical ? kFlutterKeyEventTypeRepeat
							     : kFlutterKeyEventTypeDown;
	if (!pressed->physical)
		*pressed = key_ids(scan, input->virtual_key);
	output->events[output->count++] =
		make_event(*pressed, input->timestamp_us, type, false, input->key_up ? NULL : input->text);
	if (type == kFlutterKeyEventTypeDown) {
		for (size_t i = 0; i < ARRAY_SIZE(lock_scans); ++i)
			if (scan == lock_scans[i])
				state->locks ^= FLUTTER_KEY_CAPS_LOCK << i;
	}
	if (input->key_up)
		*pressed = (flutter_pressed_key){0};
	format_raw_message(state, input, scan, output);
}

bool flutter_keyboard_release_next(flutter_keyboard_state *state, uint64_t timestamp_us, FlutterKeyEvent *event)
{
	for (size_t i = 0; i < ARRAY_SIZE(state->pressed); ++i) {
		if (!state->pressed[i].physical)
			continue;
		*event = make_event(state->pressed[i], timestamp_us, kFlutterKeyEventTypeUp, true, NULL);
		state->pressed[i] = (flutter_pressed_key){0};
		return true;
	}
	return false;
}
