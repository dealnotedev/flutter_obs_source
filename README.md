# OBS Flutter Source

Windows OBS Studio source plugin that embeds a Flutter Engine and renders a
Flutter AOT application as an OBS video/audio source.

OBS's Add Source menu provides two independent types:

- **Freydis Overlay (Software)** uses Flutter's CPU rasterizer. Its existing
  `flutter_source` ID is preserved, so previously saved scenes keep working.
- **Freydis Overlay (GPU)** uses Flutter OpenGL ES through ANGLE on the same
  GPU adapter as OBS. Its ID is `flutter_source_gpu`. It requires OBS's Direct3D
  11 backend and a GPU supporting D3D feature level 11.0.

Both types use the same application, settings, input handling and audio code,
and can coexist in a scene. Each instance still owns its own Flutter Engine.

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
    angle/                   # Required only for the GPU source
      libEGL.dll
      libGLESv2.dll
      d3dcompiler_47.dll
      vulkan-1.dll            # Optional Vulkan backend; this source uses D3D11
```

The application directory is intentionally named `flutter_obs_source`. The
plugin resolves it relative to `flutter_obs_source.dll` and refuses to create a
source when `app.so`, `icudtl.dat`, or `flutter_assets` is missing.

Keep the ANGLE DLLs in the nested `flutter_obs_source/angle` directory; do not
replace OBS's copies in `obs-plugins/64bit`. The GPU renderer loads its EGL/GLES
pair by absolute path and resolves functions from those handles. This separates
the pair from obs-browser's ANGLE. Windows can still reuse common dependencies
loaded by name, such as `d3dcompiler_47.dll`; this is not a separate DLL namespace.

## Threading model

Every OBS source instance owns one deadline-aware event loop. It is registered
as both the Flutter platform runner and UI runner, enabling Flutter's merged
platform/UI thread model. Raster and IO threads remain engine-managed.

Flutter API calls, platform messages, window metrics, lifecycle updates, and
engine shutdown are serialized through this runner. Rendered software frames
are transferred through three frame slots so the Flutter raster thread never
writes a buffer while OBS uploads it. GPU frames use three shared D3D11 textures
and keyed mutexes. Flutter renders on a private device, and OBS copies the newest
completed frame into its retained display texture. There is no CPU pixel
readback or upload in the GPU path. GPU waits on the OBS graphics thread are
nonblocking; the last displayed frame persists while Flutter is idle.

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

Build ANGLE's shared libraries from that engine checkout when not already present:

```powershell
ninja -C E:/flutter_engine_20206/flutter/engine/src/out/host_release libEGL libGLESv2
```

`FLUTTER_ANGLE_DIR` defaults to `FLUTTER_ENGINE_DIR`. Set it to the directory
containing the matching `libEGL.dll`/`libGLESv2.dll` pair when using a separate
ANGLE build. `FLUTTER_ANGLE_INCLUDE_DIR` defaults to the engine checkout's
`flutter/third_party/angle/include`. CMake packages ANGLE and its available
compiler/Vulkan dependencies into `flutter_obs_source/angle`.

ANGLE loads only when a GPU source is created. Missing DLLs produce a GPU
initialization error in the OBS log; the software source remains available.
The GPU source does not silently switch to software rendering.

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

When an OBS SDK is already installed, `-DOBS_USE_EXISTING_DEPS=ON` reuses the
SDK provided through `CMAKE_PREFIX_PATH` without rebuilding OBS on every
configure. `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` can be overridden, for example
with `Embedded` if the installed PDB server is incompatible.

## Renderer verification

The native transfer test loads the plugin into libobs and checks both registered
source names, pixel colors, orientation, premultiplied transparency, padded
software rows, resize, dropped frames, concurrent raster/graphics work, the
shared IO context and shutdown. It runs on a real D3D11 GPU:

```powershell
cmake -S . -B cmake-build-release-visual-studio `
  "-DOBS_RENDERER_TEST_BIN=C:/Program Files/obs-studio/bin/64bit" `
  "-DOBS_RENDERER_TEST_DATA=C:/Program Files/obs-studio/data/libobs"
cmake --build cmake-build-release-visual-studio --config Release
ctest --test-dir cmake-build-release-visual-studio -C Release --output-on-failure
```

Set `OBS_RENDERER_TEST_ANGLE_DIR` to the installed OBS directory containing
`libEGL.dll` and `libGLESv2.dll` to run the coexistence test. It preloads OBS's
libraries before creating our renderers, checks that GLES entry points are
distinct, verifies rendering and shutdown, and checks that the preloaded ANGLE
resolver remains usable. With the offline fixture configured, it also runs both
actual Flutter sources in the same process.

Compile the offline Dart fixture with the matching local engine to also verify
both actual OBS sources together, rendered pixels after resize and shutdown:

```powershell
Push-Location tests/flutter
flutter pub get
flutter --local-engine-src-path=E:/flutter_engine_20206/flutter/engine/src `
  --local-engine=host_release --local-engine-host=host_release assemble `
  -o build/renderer_fixture "-dTargetPlatform=windows-x64" "-dBuildMode=release" `
  "-dTargetFile=lib/renderer_fixture.dart" release_bundle_windows-x64_assets
Copy-Item build/renderer_fixture/windows/app.so build/renderer_fixture/app.so
Copy-Item E:/flutter_engine_20206/flutter/engine/src/out/host_release/icudtl.dat build/renderer_fixture/icudtl.dat
Pop-Location
cmake -S . -B cmake-build-release-visual-studio `
  "-DFLUTTER_RENDERER_TEST_BUNDLE=$PWD/tests/flutter/build/renderer_fixture"
cmake --build cmake-build-release-visual-studio --config Release
ctest --test-dir cmake-build-release-visual-studio -C Release --output-on-failure
```

The integration test stages a separate fixture runtime in the build directory;
it does not run or overwrite the production application bundle.

## Current limitations

- The software source uploads a dynamic OBS texture.
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
