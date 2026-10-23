import 'dart:ui' as ui;
import 'dart:convert';
import 'dart:typed_data';

// Offline AOT fixture: exercises the real Flutter rasterizer without networking,
// plugins, or the production application's state.
void main(List<String> args) async {
  final dispatcher = ui.PlatformDispatcher.instance;
  var configured = false;
  void configure(ByteData? data) {
    if (data == null) return;
    final message = utf8.decode(
      data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes),
    );
    final config = jsonDecode(message) as Map<String, dynamic>;
    final expectedSourceId = config['expected_source_uuid'];
    if (expectedSourceId == null) return;
    configured = args.length == 1 && args.single == expectedSourceId;
    dispatcher.scheduleFrame();
  }

  dispatcher.onPlatformMessage = (channel, data, reply) {
    if (channel == 'obs_config') configure(data);
    reply?.call(null);
  };
  dispatcher.onBeginFrame = (_) {
    if (!configured) return;
    final view = dispatcher.implicitView!;
    final size = view.physicalSize;
    final recorder = ui.PictureRecorder();
    final canvas = ui.Canvas(recorder);
    canvas.drawRect(
      ui.Rect.fromLTWH(0, 0, size.width / 2, size.height / 2),
      ui.Paint()..color = const ui.Color(0xffff0000),
    );
    canvas.drawRect(
      ui.Rect.fromLTWH(
        size.width / 2,
        size.height / 2,
        size.width / 2,
        size.height / 2,
      ),
      ui.Paint()..color = const ui.Color(0x800000ff),
    );
    final picture = recorder.endRecording();
    final scene = (ui.SceneBuilder()..addPicture(ui.Offset.zero, picture))
        .build();
    view.render(scene);
    scene.dispose();
    picture.dispose();
  };
  dispatcher.onMetricsChanged = dispatcher.scheduleFrame;
  final request = Uint8List.fromList(utf8.encode('get_dart_config'));
  dispatcher.sendPlatformMessage(
    'obs_config',
    ByteData.sublistView(request),
    configure,
  );
}
