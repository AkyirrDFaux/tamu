import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

import 'support/block_reads.dart';
import 'support/single_block_client.dart';

/// The Register page's live rows for the ResistiveMeasure rail flags (fields 5/6, firmware
/// Core/Devices/Measuring.h): the two flags must be named and shown as a Bool with no edit
/// control, exactly like the Button block's read-only Bool, and the resistive Measured Value
/// special case (field 3, sensor-derived unit) must keep working on the 7-field block.
///
/// Served by a fake client so the page renders off-device; the metas mirror the firmware's
/// ResistiveMeasEntries table (Bool + ReadOnly for fields 5/6, Number + ReadOnly for 3/4).
class _RailFlagRegisterClient extends SingleBlockRegisterClient {
  _RailFlagRegisterClient({required super.deviceId});

  @override
  int get blockType => BlockType.resistiveMeasure.value;

  @override
  String get blockName => 'Meas';

  @override
  int get fieldCount => 7;

  @override
  Future<BlockRead?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    const persistent = ValueFlags.persistent;
    const readOnly = ValueFlags.readOnly;
    return switch (field) {
      0 => fieldBytes(
        DataType.number.value,
        numberToBytes(100),
        flags: persistent,
      ),
      1 => fieldBytes(DataType.enum_.value, const [3], flags: persistent),
      2 => fieldBytes(
        DataType.number.value,
        numberToBytes(0.5),
        flags: persistent,
      ),
      3 => fieldBytes(
        DataType.number.value,
        numberToBytes(1500),
        flags: readOnly,
      ),
      4 => fieldBytes(
        DataType.number.value,
        numberToBytes(330),
        flags: readOnly,
      ),
      5 => fieldBytes(DataType.bool_.value, const [1], flags: readOnly),
      6 => fieldBytes(DataType.bool_.value, const [0], flags: readOnly),
      _ => null,
    };
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  Finder tileOf(String label) => find
      .ancestor(of: find.text(label), matching: find.byType(ListTile))
      .first;

  /// Asserts one field row renders [label] with [value], and that its edit affordance
  /// matches [editable] (the read-only rows carry no trailing menu and no onTap).
  void expectRow(
    WidgetTester tester,
    String label,
    String value, {
    required bool editable,
  }) {
    final tile = tileOf(label);
    expect(tile, findsOneWidget, reason: 'row "$label" is rendered');
    expect(
      find.descendant(of: tile, matching: find.text(value)),
      findsOneWidget,
      reason: 'row "$label" shows "$value"',
    );
    final widget = tester.widget<ListTile>(tile);
    expect(
      widget.trailing,
      editable ? isNotNull : isNull,
      reason: 'row "$label" edit affordance',
    );
    expect(widget.onTap, editable ? isNotNull : isNull);
  }

  testWidgets('the rail flags render named, as a Bool, and read-only', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: RegisterPage(
          deviceId: 1,
          isConnected: () => true,
          clientFactory: (_) => _RailFlagRegisterClient(deviceId: 1),
        ),
      ),
    );
    await tester.pump(); // the initial topology load
    await tester.tap(find.text('Meas')); // expand the block
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(tester.takeException(), isNull);

    // Fields 0-2 are the writable settings.
    expectRow(tester, 'Sampling Rate', '100.0', editable: true);
    expectRow(tester, 'Sensor Type', 'LDR 10K', editable: true);

    // The resistive special cases on the 7-field block: the sensor-derived unit at field 3.
    expectRow(tester, 'Measured Value', '1500.0 lux · Bright', editable: false);

    // The rail flags: named (not "Field 5"/"Field 6"), a true/false value, no edit control.
    expectRow(tester, 'Open Circuit', 'true', editable: false);
    expectRow(tester, 'Short Circuit', 'false', editable: false);

    // No row falls back to the unnamed label.
    expect(find.textContaining('Field 5'), findsNothing);
    expect(find.textContaining('Field 6'), findsNothing);

    await tester.pumpWidget(const SizedBox()); // dispose the auto-refresh timer
  });
}
