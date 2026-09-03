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

## Mouse interaction

Select the Flutter source in OBS and click **Interact** to open OBS's interaction
window. The Flutter application receives mouse hover and movement, left, middle,
and right button presses, dragging, and horizontal or vertical wheel scrolling.
Pointer focus and leave transitions are forwarded as well, so pressed buttons do
not remain stuck when the Interact window loses focus.

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

Supported commands are `load`, `play`, `pause`, `resume`, `stop`, and `volume`.
IDs must be in the range 0–255. Relative paths are resolved below
`flutter_assets`; use the same asset path stored in the Flutter bundle
(commonly `assets/...`).

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
- Keyboard, accessibility, IME, and application-controlled system cursor shapes
  are not implemented. Mouse input is available through the OBS Interact window,
  not through the main OBS preview.
- Each source owns a Flutter Engine for isolation. Many simultaneous sources
  therefore have a significant memory and thread cost.
