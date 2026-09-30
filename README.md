# OBS Flutter Source

Windows OBS Studio source plugin that embeds a Flutter Engine and renders a
Flutter AOT application as an OBS video/audio source.

## Runtime layout

Install the files under the OBS plugin directory with this layout:

```text
obs-plugins/64bit/
  flutter_obs_source.dll
  flutter_engine.dll
  flutter_obs_source/
    app.so
    icudtl.dat
    flutter_assets/
      AssetManifest.bin
      FontManifest.json
      ...
```

The application directory is intentionally named `flutter_obs_source`. The
plugin resolves it relative to `flutter_obs_source.dll` and refuses to create a
source when `app.so`, `icudtl.dat`, or `flutter_assets` is missing.

## Threading model

Every OBS source instance owns one deadline-aware event loop. It is registered
as both the Flutter platform runner and UI runner, enabling Flutter's merged
platform/UI thread model. Raster and IO threads remain engine-managed.

Flutter API calls, platform messages, window metrics, lifecycle updates, and
engine shutdown are serialized through this runner. Rendered software frames
are transferred through three frame slots so the Flutter raster thread never
writes a buffer while OBS uploads it.

Audio commands and miniaudio state are isolated per source. Audio callbacks are
prevented from overlapping, and sample timestamps advance on a fixed 48 kHz
clock.

## Mouse and keyboard interaction

Select the Flutter source in OBS and click **Interact** to open OBS's interaction
window. The Flutter application receives mouse hover and movement, left, middle,
and right button presses, dragging, and horizontal or vertical wheel scrolling.
Pointer focus and leave transitions are forwarded as well, so pressed buttons do
not remain stuck when the Interact window loses focus.

The same window forwards keyboard presses, releases, repeats, modifiers,
navigation keys, function keys, and the numeric keypad. Use Flutter's
`KeyboardListener`, `Focus.onKeyEvent`, `HardwareKeyboard`, or `Shortcuts` to
handle them. Pressed keys are released when the window loses focus.

Standard `TextField`/`TextFormField` widgets also accept Unicode text, including
Cyrillic. Flutter handles selection, navigation, Backspace/Delete and editing
shortcuts; the plugin provides text updates and Windows clipboard access for
copy, cut and paste. Enter submits the configured action, or inserts a newline
in a multiline field configured with `TextInputAction.newline`. Both ordinary
and delta text editing clients are supported. A shortcut that Flutter handles
does not also insert text. These built-in Flutter channels require no custom
Dart integration.

Keyboard input sends `FlutterEngineSendKeyEvent` followed by the Windows
`flutter/keyevent` companion message required for Flutter to dispatch the key.
Text insertion waits for both responses, so handled shortcuts do not type.
Text editing uses Flutter's
[`flutter/textinput` protocol](https://api.flutter.dev/flutter/services/SystemChannels/textInput-constant.html).
Prefer the modern keyboard APIs listed above in application code. OBS does not expose IME
composition callbacks through the source interaction API, so native IME
composition/candidate windows are not implemented.

Interaction is intentionally limited to the Interact window. Clicking or
dragging the source directly in the main OBS preview still manipulates the OBS
scene rather than the Flutter application.

## Dart channels

The native side uses raw UTF-8 strings. Use `BasicMessageChannel<String>` with
`StringCodec`; `MethodChannel` is not wire-compatible with this protocol.

```dart
import 'dart:convert';
import 'package:flutter/services.dart';

const configChannel = BasicMessageChannel<String>(
  'obs_config',
  StringCodec(),
);

const audioChannel = BasicMessageChannel<String>(
  'obs_audio',
  StringCodec(),
);

const audioEventsChannel = BasicMessageChannel<String>(
  'obs_audio_events',
  StringCodec(),
);

Future<Map<String, dynamic>> readObsConfig() async {
  final response = await configChannel.send('get_dart_config');
  return jsonDecode(response ?? '{}') as Map<String, dynamic>;
}

Future<void> loadAndPlayAudio() async {
  await audioChannel.send(jsonEncode({
    'cmd': 'load',
    'id': 0,
    'asset': 'assets/sounds/notification.wav',
  }));
  await audioChannel.send(jsonEncode({
    'cmd': 'play',
    'id': 0,
    'volume': 0.8,
    'loop': false,
  }));
}
```

Supported commands are `load`, `play`, `pause`, `resume`, `seek`, `stop`,
`volume`, and `release`. `seek` accepts a non-negative `position_ms` value.
IDs must be in the range 0–255. Relative paths are resolved below
`flutter_assets`; use the same asset path stored in the Flutter bundle
(commonly `assets/...`).

Accepted audio commands respond with `{"ok":true,"events":true}`. Loading and
playback then report `loaded`, `started`, `ended`, or `error` JSON messages on
`obs_audio_events`. Pass the same optional `session_id` with `load` and `play`
to correlate those asynchronous events with a particular playback attempt.

## Build

Configure `FLUTTER_ENGINE_DIR` with the matching Flutter Engine release build.
Set `FLUTTER_APP_BUNDLE_DIR` to a directory containing `app.so`, `icudtl.dat`,
and `flutter_assets` to create a complete runnable output directory.

```powershell
cmake -G "Visual Studio 17 2022" `
  -S . `
  -B cmake-build-release-visual-studio `
  -DFLUTTER_ENGINE_DIR=E:/flutter_engine_20206/flutter/engine/src/out/host_release `
  -DFLUTTER_APP_BUNDLE_DIR=D:/path/to/flutter/runtime

cmake --build cmake-build-release-visual-studio --config Release
ctest --test-dir cmake-build-release-visual-studio -C Release --output-on-failure
```

If `FLUTTER_APP_BUNDLE_DIR` is omitted, the DLLs are still built, but the
application runtime is not copied automatically.

## Current limitations

- Rendering uses Flutter's software renderer and uploads a dynamic OBS texture.
  This is reliable across OBS graphics backends but remains CPU/bandwidth heavy
  at high resolutions and frame rates.
- Accessibility, IME composition, and application-controlled system cursor
  shapes are not implemented. Mouse and keyboard input are available through
  the OBS Interact window, not through the main OBS preview or global hotkeys.
- Each source owns a Flutter Engine for isolation. Many simultaneous sources
  therefore have a significant memory and thread cost.

## Checking keyboard input

The `keyboard_input_tests` CTest target checks key identity, repeat and release
ordering, modifier/lock synchronization, focus cleanup, Unicode selection
replacement, delta updates, and Enter actions. It does not open OBS or change
the system clipboard.

The integration tests in `tests/flutter` replay messages generated by the C
keyboard translator through Flutter's real key event manager into a `TextField`.
They check all arrow directions, Home/End, Backspace/Delete (including emoji),
Shift selection, Ctrl+A, repeat events, and the handled response. This catches
protocol omissions that key-code-only unit tests cannot detect.

```powershell
cmake --build cmake-build-release-visual-studio --config Release --target keyboard_input_tests
Push-Location tests/flutter
flutter pub get
flutter test --no-pub
Pop-Location
```

Set `OBS_KEYBOARD_TEST_EXE` to the absolute path of `keyboard_input_tests.exe` if
using a different build directory.

For an end-to-end check after installing the rebuilt DLL, open **Interact** on
an application with a `TextField` and a `Focus.onKeyEvent`/`Shortcuts` handler:

1. Type Latin and Cyrillic text, hold a key to repeat, and try both Enter keys.
2. Move/select with arrows and Shift, then use Backspace/Delete and Ctrl+A/C/X/V.
3. Check Tab/Shift+Tab between fields and Enter in single-line/multiline fields.
4. Hold Ctrl or Shift, switch away from Interact, release it, and return. The
   modifier must not remain pressed. Repeat with CapsLock and NumLock enabled
   before opening Interact.
5. Trigger an application shortcut and verify it does not type into a field.

Flutter key mapping data is distributed under the BSD license in
`src/third_party/flutter/LICENSE.txt`.
