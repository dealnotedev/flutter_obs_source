#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "flutter-keyboard.h"
#include "flutter-text-input.h"
#include "third_party/cjson/cJSON.h"

#define CHECK(condition)                                                                        \
	do {                                                                                    \
		if (!(condition)) {                                                             \
			fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
			return 1;                                                               \
		}                                                                               \
	} while (0)

static flutter_key_input key(uint32_t vk, uint32_t scan, bool up, uint32_t modifiers, const char *text)
{
	return (flutter_key_input){.timestamp_us = 1234,
				   .virtual_key = vk,
				   .scan_code = scan,
				   .key_up = up,
				   .modifiers = modifiers,
				   .text = text};
}

static int test_key_sequences(void)
{
	flutter_keyboard_state state;
	flutter_key_output output;
	flutter_keyboard_init(&state);
	flutter_key_input event = key('A', 0x1e, false, 0, "a");
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 1);
	CHECK(output.events[0].type == kFlutterKeyEventTypeDown);
	CHECK(output.events[0].physical == 0x70004 && output.events[0].logical == 'a');
	CHECK(output.events[0].timestamp == 1234);
	CHECK(strcmp(output.events[0].character, "a") == 0);
	event.virtual_key = 'Q'; // Changing layouts while held must retain the logical key.
	event.text = "Q";
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 1 && output.events[0].type == kFlutterKeyEventTypeRepeat);
	CHECK(output.events[0].logical == 'a');
	event.key_up = true;
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 1 && output.events[0].type == kFlutterKeyEventTypeUp);
	CHECK(output.events[0].logical == 'a' && !output.events[0].character);
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 0);
	return 0;
}

static int test_modifiers_focus_and_locks(void)
{
	flutter_keyboard_state state;
	flutter_key_output output;
	flutter_keyboard_init(&state);
	flutter_key_input event = key('C', 0x2e, false, FLUTTER_KEY_CONTROL_RIGHT | FLUTTER_KEY_CAPS_LOCK, "\x03");
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 4); // held Ctrl, Caps down/up, real C
	CHECK(output.events[0].logical == UINT64_C(0x200000101));
	CHECK(output.events[0].synthesized);
	CHECK(output.events[1].logical == UINT64_C(0x100000104));
	CHECK(output.events[2].type == kFlutterKeyEventTypeUp);
	CHECK(!output.events[3].character);
	FlutterKeyEvent released;
	int count = 0;
	while (flutter_keyboard_release_next(&state, 4321, &released)) {
		CHECK(released.synthesized && released.type == kFlutterKeyEventTypeUp);
		CHECK(released.timestamp == 4321);
		++count;
	}
	CHECK(count == 2);
	event.key_up = true;
	event.modifiers = FLUTTER_KEY_CAPS_LOCK;
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 0);
	event = key(VK_CAPITAL, 0x3a, false, 0, ""); // turn Caps off
	flutter_keyboard_translate(&state, &event, &output);
	CHECK(output.count == 1 && output.events[0].type == kFlutterKeyEventTypeDown);
	CHECK(state.locks == 0);
	return 0;
}

static int test_special_keys(void)
{
	const struct {
		uint32_t vk, scan;
		uint64_t physical, logical;
	} cases[] = {
		{VK_RETURN, 0x11c, 0x70058, UINT64_C(0x20000020d)},  {VK_RETURN, 0x1c, 0x70028, UINT64_C(0x10000000d)},
		{VK_CONTROL, 0x11d, 0x700e4, UINT64_C(0x200000101)}, {VK_MENU, 0xe038, 0x700e6, UINT64_C(0x200000105)},
		{VK_SHIFT, 0x36, 0x700e5, UINT64_C(0x200000103)},    {VK_LEFT, 0x14b, 0x70050, UINT64_C(0x100000302)},
		{VK_HOME, 0x47, 0x7005f, UINT64_C(0x200000237)},     {VK_DELETE, 0x153, 0x7004c, UINT64_C(0x10000007f)},
		{VK_F12, 0x58, 0x70045, UINT64_C(0x10000080c)},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		flutter_keyboard_state state;
		flutter_key_output output;
		flutter_keyboard_init(&state);
		flutter_key_input event = key(cases[i].vk, cases[i].scan, false, 0, "");
		flutter_keyboard_translate(&state, &event, &output);
		CHECK(output.count == 1);
		CHECK(output.events[0].physical == cases[i].physical && output.events[0].logical == cases[i].logical);
	}
	return 0;
}

// Check Flutter's down/repeat/up invariant over a deterministic stream with
// missed releases, modifier changes, focus loss, and layout changes.
static int test_event_regularity(void)
{
	flutter_keyboard_state state;
	flutter_keyboard_init(&state);
	flutter_pressed_key pressed[FLUTTER_KEY_SLOTS] = {0};
	uint32_t seed = 42;
	for (int i = 0; i < 10000; ++i) {
		seed = seed * 1664525u + 1013904223u;
		flutter_key_input event = key('A' + ((seed >> 24) % 26), 0x1e + ((seed >> 16) % 8), (seed & 1) != 0,
					      (seed >> 3) & 0x7ff, "x");
		flutter_key_output output;
		flutter_keyboard_translate(&state, &event, &output);
		CHECK(output.count <= FLUTTER_KEY_MAX_EVENTS);
		FlutterKeyEvent events[FLUTTER_KEY_MAX_EVENTS + FLUTTER_KEY_SLOTS];
		size_t count = output.count;
		memcpy(events, output.events, count * sizeof(*events));
		if (i % 11 == 0) {
			FlutterKeyEvent release;
			while (flutter_keyboard_release_next(&state, 0, &release)) {
				events[count++] = release;
			}
		}
		for (size_t n = 0; n < count; ++n) {
			const FlutterKeyEvent *e = &events[n];
			size_t slot = 0;
			while (slot < FLUTTER_KEY_SLOTS && pressed[slot].physical != e->physical)
				++slot;
			if (e->type == kFlutterKeyEventTypeDown) {
				CHECK(slot == FLUTTER_KEY_SLOTS);
				for (slot = 0; pressed[slot].physical; ++slot) {
				}
				pressed[slot] = (flutter_pressed_key){e->physical, e->logical};
			} else {
				CHECK(slot < FLUTTER_KEY_SLOTS && pressed[slot].logical == e->logical);
				if (e->type == kFlutterKeyEventTypeUp)
					pressed[slot] = (flutter_pressed_key){0};
			}
		}
	}
	return 0;
}

typedef struct {
	cJSON *messages[8];
	int count;
} messages;

static void capture(const char *message, void *user_data)
{
	messages *output = user_data;
	if (output->count < 8)
		output->messages[output->count++] = cJSON_Parse(message);
}

static void clear_messages(messages *output)
{
	for (int i = 0; i < output->count; ++i)
		cJSON_Delete(output->messages[i]);
	memset(output, 0, sizeof(*output));
}

static bool call_ok(flutter_text_input *input, const char *message)
{
	char *response = flutter_text_input_handle(input, message, strlen(message));
	bool ok = response && strcmp(response, "[null]") == 0;
	free(response);
	return ok;
}

static cJSON *editing_state(messages *output, int index)
{
	return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(output->messages[index], "args"), 1);
}

static int test_unicode_selection_and_shortcuts(void)
{
	flutter_text_input input;
	flutter_text_input_init(&input);
	messages output = {0};
	CHECK(call_ok(&input, "{\"method\":\"TextInput.setClient\",\"args\":[7,{}]}"));
	CHECK(call_ok(
		&input,
		"{\"method\":\"TextInput.setEditingState\",\"args\":{\"text\":\"A😀Б\",\"selectionBase\":3,\"selectionExtent\":1}}"));
	flutter_text_input_key(&input, 'Z', 0, "ї", capture, &output);
	CHECK(output.count == 1);
	CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(editing_state(&output, 0), "text")->valuestring, "AїБ") == 0);
	CHECK(input.selection_base == 2 && input.selection_extent == 2);
	clear_messages(&output);
	flutter_text_input_key(&input, 'V', FLUTTER_KEY_CONTROL_LEFT, "v", capture, &output);
	flutter_text_input_key(&input, VK_BACK, 0, "\b", capture, &output);
	flutter_text_input_key(&input, VK_TAB, 0, "\t", capture, &output);
	CHECK(output.count == 0);
	flutter_text_input_key(&input, 'E', FLUTTER_KEY_CONTROL_LEFT | FLUTTER_KEY_ALT_RIGHT, "€", capture, &output);
	CHECK(output.count == 1);
	CHECK(wcscmp(input.text, L"Aї€Б") == 0);
	CHECK(!call_ok(
		&input,
		"{\"method\":\"TextInput.setEditingState\",\"args\":{\"text\":\"😀\",\"selectionBase\":1,\"selectionExtent\":1}}"));
	CHECK(wcscmp(input.text, L"Aї€Б") == 0);
	CHECK(call_ok(&input, "{\"method\":\"TextInput.clearClient\",\"args\":null}"));
	flutter_text_input_key(&input, 'A', 0, "a", capture, &output);
	CHECK(output.count == 1);
	clear_messages(&output);
	flutter_text_input_destroy(&input);
	return 0;
}

static int test_delta_enter_and_client_lifecycle(void)
{
	flutter_text_input input;
	flutter_text_input_init(&input);
	messages output = {0};
	CHECK(call_ok(
		&input,
		"{\"method\":\"TextInput.setClient\",\"args\":[8,{\"enableDeltaModel\":true,\"inputAction\":\"TextInputAction.newline\",\"inputType\":{\"name\":\"TextInputType.multiline\"}}]}"));
	flutter_text_input_key(&input, 'A', 0, "😀", capture, &output);
	CHECK(output.count == 1 && input.selection_extent == 2);
	cJSON *delta = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(editing_state(&output, 0), "deltas"), 0);
	CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(delta, "deltaText")->valuestring, "😀") == 0);
	CHECK(cJSON_GetObjectItemCaseSensitive(delta, "selectionExtent")->valueint == 2);
	clear_messages(&output);
	flutter_text_input_key(&input, VK_RETURN, 0, "\r", capture, &output);
	CHECK(output.count == 2 && wcscmp(input.text, L"😀\n") == 0);
	CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(output.messages[1], "method")->valuestring,
		     "TextInputClient.performAction") == 0);
	clear_messages(&output);
	const uint64_t generation = input.generation;
	CHECK(call_ok(&input,
		      "{\"method\":\"TextInput.setClient\",\"args\":[9,{\"inputAction\":\"TextInputAction.search\"}]}"));
	CHECK(input.generation != generation && !input.delta_model && input.text[0] == 0);
	flutter_text_input_key(&input, VK_RETURN, 0, "\r", capture, &output);
	CHECK(output.count == 1 && input.text[0] == 0);
	CHECK(strcmp(editing_state(&output, 0)->valuestring, "TextInputAction.search") == 0);
	CHECK(!call_ok(&input, "{\"method\":\"TextInput.setClient\",\"args\":[-1,{}]}"));
	CHECK(input.client_id == 9);
	char *unknown =
		flutter_text_input_handle(&input, "{\"method\":\"unknown\"}", strlen("{\"method\":\"unknown\"}"));
	CHECK(!unknown);
	clear_messages(&output);
	flutter_text_input_destroy(&input);
	return 0;
}

static void append_key_trace(cJSON *trace, const FlutterKeyEvent *event)
{
	cJSON *entry = cJSON_CreateObject();
	cJSON_AddNumberToObject(entry, "type", event->type);
	cJSON_AddNumberToObject(entry, "physical", (double)event->physical);
	cJSON_AddNumberToObject(entry, "logical", (double)event->logical);
	cJSON_AddBoolToObject(entry, "synthesized", event->synthesized);
	if (event->character)
		cJSON_AddStringToObject(entry, "character", event->character);
	cJSON_AddItemToArray(trace, entry);
}

static int print_key_trace(uint32_t virtual_key, uint32_t scan_code, uint32_t modifiers, int repeats)
{
	flutter_keyboard_state state;
	flutter_keyboard_init(&state);
	cJSON *trace = cJSON_CreateArray();
	for (int i = 0; i < repeats + 2; ++i) {
		flutter_key_input input = key(virtual_key, scan_code, i == repeats + 1, modifiers, "");
		flutter_key_output output;
		flutter_keyboard_translate(&state, &input, &output);
		for (size_t n = 0; n < output.count; ++n)
			append_key_trace(trace, &output.events[n]);
		if (output.raw_message[0]) {
			cJSON *entry = cJSON_CreateObject();
			cJSON_AddItemToObject(entry, "raw", cJSON_Parse(output.raw_message));
			cJSON_AddItemToArray(trace, entry);
		}
	}
	FlutterKeyEvent release;
	while (flutter_keyboard_release_next(&state, 1235, &release))
		append_key_trace(trace, &release);
	char *json = cJSON_PrintUnformatted(trace);
	CHECK(json);
	puts(json);
	cJSON_free(json);
	cJSON_Delete(trace);
	return 0;
}

int main(int argc, char **argv)
{
	if ((argc == 4 || argc == 6) && strcmp(argv[1], "--key-trace") == 0)
		return print_key_trace((uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0),
				       argc == 6 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0,
				       argc == 6 ? atoi(argv[5]) : 0);
	CHECK(test_key_sequences() == 0);
	CHECK(test_modifiers_focus_and_locks() == 0);
	CHECK(test_special_keys() == 0);
	CHECK(test_event_regularity() == 0);
	CHECK(test_unicode_selection_and_shortcuts() == 0);
	CHECK(test_delta_enter_and_client_lifecycle() == 0);
	puts("Keyboard and text input tests passed");
	return 0;
}
