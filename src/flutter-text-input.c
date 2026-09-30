#include "flutter-text-input.h"
#include "flutter-keyboard.h"
#include "third_party/cjson/cJSON.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char *copy_text(const char *text)
{
	const size_t size = strlen(text) + 1;
	char *result = malloc(size);
	if (result)
		memcpy(result, text, size);
	return result;
}

static wchar_t *from_utf8(const char *text)
{
	if (!text)
		return NULL;
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
	if (!size)
		return NULL;
	wchar_t *result = malloc((size_t)size * sizeof(wchar_t));
	if (result)
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, size);
	return result;
}

static char *to_utf8(const wchar_t *text)
{
	const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
	if (!size)
		return NULL;
	char *result = malloc((size_t)size);
	if (result)
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, size, NULL, NULL);
	return result;
}

static bool json_string_is(const cJSON *value, const char *text)
{
	return cJSON_IsString(value) && strcmp(value->valuestring, text) == 0;
}

static bool json_int(const cJSON *value, int *result)
{
	if (!cJSON_IsNumber(value) || value->valuedouble < INT_MIN || value->valuedouble > INT_MAX ||
	    value->valuedouble != value->valueint)
		return false;
	*result = value->valueint;
	return true;
}

static bool valid_offset(const wchar_t *text, int offset)
{
	const size_t length = wcslen(text);
	if (offset < 0 || (size_t)offset > length)
		return false;
	// Flutter offsets count UTF-16 units; never split a surrogate pair.
	return !offset || (size_t)offset == length || text[offset] < 0xdc00 || text[offset] > 0xdfff;
}

void flutter_text_input_init(flutter_text_input *input)
{
	memset(input, 0, sizeof(*input));
	input->client_id = -1;
}

void flutter_text_input_destroy(flutter_text_input *input)
{
	free(input->text);
	if (input->clipboard_window)
		DestroyWindow((HWND)input->clipboard_window);
	flutter_text_input_init(input);
}

char *flutter_text_input_handle(flutter_text_input *input, const char *message, size_t size)
{
	cJSON *call = message ? cJSON_ParseWithLength(message, size) : NULL;
	const cJSON *method = cJSON_GetObjectItemCaseSensitive(call, "method");
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "args");
	const char *response = "[null]";
	const char *invalid = "[\"bad_arguments\",\"Invalid text input arguments\",null]";
	if (!cJSON_IsString(method)) {
		response = invalid;
	} else if (json_string_is(method, "TextInput.setClient")) {
		int client;
		const cJSON *config = cJSON_GetArrayItem(args, 1);
		const cJSON *action = cJSON_GetObjectItemCaseSensitive(config, "inputAction");
		wchar_t *empty = NULL;
		if (!cJSON_IsArray(args) || !json_int(cJSON_GetArrayItem(args, 0), &client) || client < 0 ||
		    !cJSON_IsObject(config) ||
		    (cJSON_IsString(action) && strlen(action->valuestring) >= sizeof(input->action)) ||
		    !(empty = from_utf8(""))) {
			response = invalid;
		} else {
			free(input->text);
			input->text = empty;
			input->client_id = client;
			input->generation++;
			input->selection_base = input->selection_extent = 0;
			input->delta_model = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config, "enableDeltaModel"));
			const cJSON *type = cJSON_GetObjectItemCaseSensitive(config, "inputType");
			input->multiline = json_string_is(cJSON_GetObjectItemCaseSensitive(type, "name"),
							  "TextInputType.multiline");
			strcpy(input->action, cJSON_IsString(action) ? action->valuestring : "TextInputAction.done");
		}
	} else if (json_string_is(method, "TextInput.setEditingState")) {
		const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
		int base = 0, extent = 0;
		wchar_t *wide = cJSON_IsString(text) ? from_utf8(text->valuestring) : NULL;
		bool valid = input->client_id >= 0 && wide &&
			     json_int(cJSON_GetObjectItemCaseSensitive(args, "selectionBase"), &base) &&
			     json_int(cJSON_GetObjectItemCaseSensitive(args, "selectionExtent"), &extent);
		if (valid && base == -1 && extent == -1)
			base = extent = 0;
		if (!valid || !valid_offset(wide, base) || !valid_offset(wide, extent)) {
			free(wide);
			response = invalid;
		} else {
			free(input->text);
			input->text = wide;
			input->selection_base = base;
			input->selection_extent = extent;
		}
	} else if (json_string_is(method, "TextInput.clearClient")) {
		input->client_id = -1;
		input->generation++;
		free(input->text);
		input->text = NULL;
	} else if (json_string_is(method, "TextInput.show") || json_string_is(method, "TextInput.hide") ||
		   json_string_is(method, "TextInput.setEditableSizeAndTransform") ||
		   json_string_is(method, "TextInput.setCaretRect") ||
		   json_string_is(method, "TextInput.setMarkedTextRect") ||
		   json_string_is(method, "TextInput.setStyle") ||
		   json_string_is(method, "TextInput.finishAutofillContext")) {
		// Hardware keyboard only: there is no native IME view or soft keyboard.
	} else {
		response = NULL;
	}
	cJSON_Delete(call);
	return response ? copy_text(response) : NULL;
}

static void send_method(const char *method, cJSON *args, flutter_text_send send, void *user_data)
{
	cJSON *call = cJSON_CreateObject();
	cJSON_AddStringToObject(call, "method", method);
	cJSON_AddItemToObject(call, "args", args);
	char *json = cJSON_PrintUnformatted(call);
	if (json) {
		send(json, user_data);
		cJSON_free(json);
	}
	cJSON_Delete(call);
}

static void add_selection(cJSON *state, int cursor)
{
	cJSON_AddNumberToObject(state, "selectionBase", cursor);
	cJSON_AddNumberToObject(state, "selectionExtent", cursor);
	cJSON_AddStringToObject(state, "selectionAffinity", "TextAffinity.downstream");
	cJSON_AddBoolToObject(state, "selectionIsDirectional", false);
	cJSON_AddNumberToObject(state, "composingBase", -1);
	cJSON_AddNumberToObject(state, "composingExtent", -1);
}

static void insert_text(flutter_text_input *input, const wchar_t *text, flutter_text_send send, void *user_data)
{
	const int start = min(input->selection_base, input->selection_extent);
	const int end = max(input->selection_base, input->selection_extent);
	const size_t old_length = wcslen(input->text);
	const size_t inserted = wcslen(text);
	const size_t length = old_length - (size_t)(end - start) + inserted;
	if (length > INT_MAX)
		return;
	wchar_t *updated = malloc((length + 1) * sizeof(wchar_t));
	if (!updated)
		return;
	memcpy(updated, input->text, (size_t)start * sizeof(wchar_t));
	memcpy(updated + start, text, inserted * sizeof(wchar_t));
	memcpy(updated + start + inserted, input->text + end, (old_length - end + 1) * sizeof(wchar_t));
	char *new_text = to_utf8(updated);
	char *old_text = to_utf8(input->text);
	char *delta_text = to_utf8(text);
	if (!new_text || !old_text || !delta_text) {
		free(updated);
		free(new_text);
		free(old_text);
		free(delta_text);
		return;
	}
	free(input->text);
	input->text = updated;
	input->selection_base = input->selection_extent = start + (int)inserted;
	cJSON *args = cJSON_CreateArray();
	cJSON_AddItemToArray(args, cJSON_CreateNumber(input->client_id));
	cJSON *state = cJSON_CreateObject();
	add_selection(state, input->selection_extent);
	if (input->delta_model) {
		cJSON_AddStringToObject(state, "oldText", old_text);
		cJSON_AddStringToObject(state, "deltaText", delta_text);
		cJSON_AddNumberToObject(state, "deltaStart", start);
		cJSON_AddNumberToObject(state, "deltaEnd", end);
		cJSON *container = cJSON_CreateObject();
		cJSON *deltas = cJSON_AddArrayToObject(container, "deltas");
		cJSON_AddItemToArray(deltas, state);
		cJSON_AddItemToArray(args, container);
	} else {
		cJSON_AddStringToObject(state, "text", new_text);
		cJSON_AddItemToArray(args, state);
	}
	send_method(input->delta_model ? "TextInputClient.updateEditingStateWithDeltas"
				       : "TextInputClient.updateEditingState",
		    args, send, user_data);
	free(new_text);
	free(old_text);
	free(delta_text);
}

void flutter_text_input_key(flutter_text_input *input, uint32_t virtual_key, uint32_t modifiers, const char *text,
			    flutter_text_send send, void *user_data)
{
	if (input->client_id < 0 || !input->text)
		return;
	const bool control = (modifiers & (FLUTTER_KEY_CONTROL_LEFT | FLUTTER_KEY_CONTROL_RIGHT)) != 0;
	const bool alt = (modifiers & (FLUTTER_KEY_ALT_LEFT | FLUTTER_KEY_ALT_RIGHT)) != 0;
	const bool altgr = control && (modifiers & FLUTTER_KEY_ALT_RIGHT);
	if ((modifiers & (FLUTTER_KEY_META_LEFT | FLUTTER_KEY_META_RIGHT)) || ((control || alt) && !altgr))
		return;
	if (virtual_key == VK_RETURN) {
		if (input->multiline && strcmp(input->action, "TextInputAction.newline") == 0)
			insert_text(input, L"\n", send, user_data);
		cJSON *args = cJSON_CreateArray();
		cJSON_AddItemToArray(args, cJSON_CreateNumber(input->client_id));
		cJSON_AddItemToArray(args, cJSON_CreateString(input->action));
		send_method("TextInputClient.performAction", args, send, user_data);
		return;
	}
	wchar_t *wide = from_utf8(text);
	if (!wide)
		return;
	bool printable = wide[0] != 0;
	for (const wchar_t *p = wide; *p; ++p)
		if (*p < 0x20 || (*p >= 0x7f && *p <= 0x9f))
			printable = false;
	if (printable)
		insert_text(input, wide, send, user_data);
	free(wide);
}

char *flutter_clipboard_handle(flutter_text_input *input, const char *message, size_t size)
{
	cJSON *call = message ? cJSON_ParseWithLength(message, size) : NULL;
	const cJSON *method = cJSON_GetObjectItemCaseSensitive(call, "method");
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(call, "args");
	const bool set = json_string_is(method, "Clipboard.setData");
	const bool get = json_string_is(method, "Clipboard.getData");
	const bool has = json_string_is(method, "Clipboard.hasStrings");
	char *response = NULL;
	if (!set && !get && !has)
		goto done;
	response = copy_text("[\"clipboard_error\",\"Unable to access the Windows clipboard\",null]");
	if (!input->clipboard_window)
		input->clipboard_window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
							  GetModuleHandleW(NULL), NULL);
	if (!input->clipboard_window || !OpenClipboard((HWND)input->clipboard_window))
		goto done;
	cJSON *envelope = cJSON_CreateArray();
	bool success = false;
	if (set) {
		const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
		wchar_t *wide = cJSON_IsString(text) ? from_utf8(text->valuestring) : NULL;
		if (wide) {
			const size_t bytes = (wcslen(wide) + 1) * sizeof(wchar_t);
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
			void *buffer = memory ? GlobalLock(memory) : NULL;
			if (buffer) {
				memcpy(buffer, wide, bytes);
				GlobalUnlock(memory);
				if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
					success = true;
					memory = NULL; // Ownership transferred to Windows.
				}
			}
			if (memory)
				GlobalFree(memory);
			free(wide);
		}
		cJSON_AddItemToArray(envelope, cJSON_CreateNull());
	} else if (has) {
		cJSON *value = cJSON_CreateObject();
		cJSON_AddBoolToObject(value, "value", IsClipboardFormatAvailable(CF_UNICODETEXT) != 0);
		cJSON_AddItemToArray(envelope, value);
		success = true;
	} else if (!json_string_is(args, "text/plain") || !IsClipboardFormatAvailable(CF_UNICODETEXT)) {
		cJSON_AddItemToArray(envelope, cJSON_CreateNull());
		success = true;
	} else {
		HANDLE memory = GetClipboardData(CF_UNICODETEXT);
		const wchar_t *wide = memory ? GlobalLock(memory) : NULL;
		// External clipboard providers may supply unterminated data.
		const size_t capacity = memory ? GlobalSize(memory) / sizeof(wchar_t) : 0;
		char *text = wide && wmemchr(wide, 0, capacity) ? to_utf8(wide) : NULL;
		if (text) {
			cJSON *value = cJSON_CreateObject();
			cJSON_AddStringToObject(value, "text", text);
			cJSON_AddItemToArray(envelope, value);
			success = true;
			free(text);
		}
		if (wide)
			GlobalUnlock(memory);
	}
	CloseClipboard();
	if (success) {
		free(response);
		response = cJSON_PrintUnformatted(envelope);
	}
	cJSON_Delete(envelope);
done:
	cJSON_Delete(call);
	return response;
}
