# OBS Flutter Source

> An OBS Studio source plugin that embeds the [Flutter Engine](https://docs.flutter.dev/) on Windows, enabling rich, interactive UIs and logic built in Dart to be rendered directly in OBS.

![plot](./images/obs.png)

## Features

- **Embed Flutter UI in OBS:**  
  Run your Flutter apps as a native OBS source, leveraging the full power of Flutter’s rendering, animations, and Dart logic inside streaming scenes.

- **Full Video and Audio Support:**
    - Renders Flutter output as video with dynamic resolution and pixel ratio.
    - Plays back audio from Flutter using a robust integration with [miniaudio](https://github.com/mackron/miniaudio).
    - Supports loading, playing, stopping, and volume control for up to 256 simultaneous sounds.

- **High-Performance Architecture:**
    - Runs the Flutter Engine on a dedicated worker thread.
    - Uses a custom task runner so all Flutter platform messages and engine tasks execute on the same thread, preventing concurrency issues.
    - Efficient, lock-free command queues for both engine and audio commands.

- **OBS Studio Integration:**
    - Native video and audio source for OBS.
    - Exposes width, height, and pixel ratio properties for easy configuration.
    - Dynamically responds to OBS property changes (resolution, pixel ratio) and updates Flutter engine metrics live.

- **Asset Management:**
    - Automatically locates assets, ICU data, and AOT libraries adjacent to the plugin DLL.
    - Supports both absolute and relative asset paths for loading resources.

- **Dart–OBS Communication:**
    - Bi-directional messaging: Dart code can send commands (e.g., to play audio) via platform channels, and the C plugin acknowledges or responds.
    - Sample platform channel included for sending audio commands as JSON.

## How It Works

- **Worker Thread:**  
  The Flutter engine runs on a background thread dedicated to processing engine tasks and messages, keeping UI updates smooth and isolated from OBS’s main thread.

- **Software Rendering:**  
  The plugin uses the Flutter Engine’s software renderer. It reads rendered pixel data into a texture and displays it in OBS as a standard source.

- **Audio Integration:**  
  miniaudio is embedded for low-latency audio playback, managed entirely from Dart via platform messages.

- **Lifecycle Management:**  
  Sources are reference-counted. The engine and worker thread exist only when at least one Flutter source is active.

## Typical Use Cases

- Stream overlays with custom Dart/Flutter logic.
- Interactive widgets, animations, or games built with Flutter and controlled live from OBS.
- Audio-reactive visuals and overlays.

## Example: Playing Audio from Dart

To play audio from Dart code running in your Flutter app, send a message like:
```dart
const message = {
  "cmd": "play",
  "id": 0,
  "asset": "sounds/notification.wav",
  "volume": 0.8,
  "loop": false
};
// Send via MethodChannel('obs_audio').invokeMethod('play', message)