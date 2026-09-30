import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

Future<bool> sendNativeKey(
  WidgetTester tester,
  int virtualKey,
  int scanCode, {
  int modifiers = 0,
  int repeats = 0,
}) async {
  final executable =
      Platform.environment['OBS_KEYBOARD_TEST_EXE'] ??
      '../../cmake-build-release-visual-studio/Release/keyboard_input_tests.exe';
  final result = Process.runSync(executable, [
    '--key-trace',
    '$virtualKey',
    '$scanCode',
    '$modifiers',
    '$repeats',
  ]);
  expect(result.exitCode, 0, reason: result.stderr.toString());
  final events = jsonDecode(result.stdout as String) as List<dynamic>;
  var handled = false;
  for (final event in events.cast<Map<String, dynamic>>()) {
    if (event.containsKey('raw')) {
      final response = await ServicesBinding.instance.keyEventManager
          .handleRawKeyMessage(event['raw']);
      handled = (response['handled'] as bool) || handled;
      await tester.pump();
      continue;
    }
    // Replay the actual embedder output, not Flutter test's keyboard simulator:
    // the simulator supplies extra platform messages our plugin may omit.
    ServicesBinding.instance.keyEventManager.handleKeyData(
      ui.KeyData(
        timeStamp: Duration.zero,
        type: switch (event['type']) {
          1 => ui.KeyEventType.up,
          2 => ui.KeyEventType.down,
          _ => ui.KeyEventType.repeat,
        },
        physical: event['physical'] as int,
        logical: event['logical'] as int,
        character: event['character'] as String?,
        synthesized: event['synthesized'] as bool,
      ),
    );
    await tester.pump();
  }
  return handled;
}

void main() {
  for (final (
        name,
        vk,
        scan,
        text,
        cursor,
        modifiers,
        repeats,
        expectedText,
        base,
        extent,
      )
      in [
        ('left arrow', 0x25, 0x14b, 'abcd', 2, 0, 0, 'abcd', 1, 1),
        ('right arrow', 0x27, 0x14d, 'abcd', 2, 0, 0, 'abcd', 3, 3),
        ('up arrow', 0x26, 0x148, 'ab\ncd', 4, 0, 0, 'ab\ncd', 1, 1),
        ('down arrow', 0x28, 0x150, 'ab\ncd', 1, 0, 0, 'ab\ncd', 4, 4),
        ('backspace', 0x08, 0x0e, 'abcd', 2, 0, 0, 'acd', 1, 1),
        ('delete', 0x2e, 0x153, 'abcd', 2, 0, 0, 'abd', 2, 2),
        ('backspace emoji', 0x08, 0x0e, 'A😀Б', 3, 0, 0, 'AБ', 1, 1),
        ('delete emoji', 0x2e, 0x153, 'A😀Б', 1, 0, 0, 'AБ', 1, 1),
        ('home', 0x24, 0x147, 'abcd', 2, 0, 0, 'abcd', 0, 0),
        ('end', 0x23, 0x14f, 'abcd', 2, 0, 0, 'abcd', 4, 4),
        ('shift left selects', 0x25, 0x14b, 'abcd', 2, 1, 0, 'abcd', 2, 1),
        ('control A selects all', 0x41, 0x1e, 'abcd', 2, 4, 0, 'abcd', 0, 4),
        ('held left repeats', 0x25, 0x14b, 'abcd', 3, 0, 2, 'abcd', 0, 0),
      ]) {
    testWidgets('$name edits a real TextField through native key messages', (
      tester,
    ) async {
      final controller = TextEditingController(text: text);
      addTearDown(controller.dispose);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: TextField(
              controller: controller,
              autofocus: true,
              maxLines: null,
            ),
          ),
        ),
      );
      await tester.pump();
      controller.selection = TextSelection.collapsed(offset: cursor);
      await tester.pump();

      expect(
        tester
            .widget<EditableText>(find.byType(EditableText))
            .focusNode
            .hasFocus,
        isTrue,
      );

      final handled = await sendNativeKey(
        tester,
        vk,
        scan,
        modifiers: modifiers,
        repeats: repeats,
      );

      expect(controller.text, expectedText);
      expect(controller.selection.baseOffset, base);
      expect(controller.selection.extentOffset, extent);
      // The native text fallback must see the final handled result.
      expect(handled, isTrue);
      await tester.pumpWidget(const SizedBox());
    });
  }
}
