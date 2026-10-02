import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_backup.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/ui/register_backup_view.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// The Register page's Backup view (Docs/App/Service views/Register.md: "Current/Backup
/// view", "Save button (current) / Recall button (backup)").

/// Counts topology reads so the auto-refresh cadence can be asserted without a device.
class _CountingRegisterClient extends RegisterClient {
  _CountingRegisterClient({required super.deviceId});

  int readBlocksCalls = 0;

  @override
  Future<List<({int type, int inst, BlockMeta meta, String name})?>?> readBlocks(
      {List<int>? scriptSlots}) async {
    readBlocksCalls++;
    return [
      (type: 0, inst: 0, meta: const BlockMeta(flagsAndType: 0, size: 9), name: 'System'),
    ];
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

  List<int> statlogEntry(int blockIdx, int field, DataType type, List<int> value) {
    final out = <int>[
      blockIdx, field, 0xFF, 0,
      type.value & 0xFF, (type.value >> 8) & 0xFF, 0xFF, value.length,
      ...value,
    ];
    while (out.length % 4 != 0) {
      out.add(0);
    }
    return out;
  }

  /// One static block (resistive measure, 2 fields), one dynamic slot and a script block.
  final blocks = <({int type, int inst, BlockMeta meta, String name})?>[
    (type: 0, inst: 0, meta: const BlockMeta(flagsAndType: 0, size: 9), name: 'System'),
    (
      type: BlockType.resistiveMeasure.value,
      inst: 0,
      meta: BlockMeta(flagsAndType: DataType.number.value, size: 2),
      name: 'Meas'
    ),
    (
      type: BlockType.dynamic.value,
      inst: 3,
      meta: BlockMeta(flagsAndType: DataType.number.value, size: 4),
      name: 'Box'
    ),
    (
      type: BlockType.script.value,
      inst: 0,
      meta: BlockMeta(flagsAndType: DataType.number.value, size: 1),
      name: 'Eye'
    ),
  ];

  /// A backup with: a stored System Name, one stored static field (1 of 2), and a dynamic
  /// slot whose table has one persistent entry (stored) and one volatile entry.
  DeviceBackup backup() => DeviceBackup.decode(
        statlog: <int>[
          ...statlogEntry(statlogSystemBlock, systemNameField, DataType.string, 'Eye'.codeUnits),
          ...statlogEntry(0, 0, DataType.number, numberToBytes(5)),
          0xFF,
        ],
        staticRegistry: const [(type: 0x08, inst: 0)],
        dynamic: {
          3: (
            table: <int>[
              3, ...'Box'.codeUnits,
              ...u16(BlockType.dynamic.value),
              ...u16(2),
              ...u16(0), ...u16(DataType.number.value | FieldFlags.persistent), 4, 0,
              ...u16(0x0100), ...u16(DataType.number.value), 4, 0,
            ],
            values: numberToBytes(42),
          ),
        },
      );

  Future<void> pump(WidgetTester tester, {DeviceBackup? b,
      Future<void> Function(int, int, int, int)? onRecall}) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
        body: RegisterBackupView(
          backup: b ?? backup(),
          blocks: blocks,
          onRecall: onRecall,
        ),
      ),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'the backup view threw while rendering');
  }

  testWidgets('shows stored values and marks the ones that were never saved', (tester) async {
    await pump(tester);
    // Stored: the dynamic value, the static value and the System Name.
    expect(find.textContaining('42.0'), findsWidgets);
    expect(find.textContaining('5.0'), findsWidgets);
    expect(find.textContaining('Eye'), findsWidgets);
    // The static block's second field was never saved.
    expect(find.textContaining('not backed up'), findsWidgets);
  });

  testWidgets('a volatile dynamic entry is "not persisted", not "not backed up"', (tester) async {
    await pump(tester);
    expect(find.textContaining('not persisted'), findsOneWidget);
  });

  testWidgets('a script block is reported as not backed up', (tester) async {
    await pump(tester);
    expect(find.text('scripts are not backed up'), findsOneWidget);
  });

  testWidgets('an empty backup renders without throwing', (tester) async {
    await pump(tester, b: DeviceBackup.empty);
    expect(tester.takeException(), isNull);
    expect(find.textContaining('not backed up'), findsWidgets);
  });

  testWidgets('per-field recall is offered only for stored values', (tester) async {
    final recalled = <(int, int, int, int)>[];
    await pump(tester, onRecall: (bt, inst, field, key) async {
      recalled.add((bt, inst, field, key));
    });
    final buttons = find.widgetWithIcon(IconButton, Icons.restore);
    // System Name + static field 0 + dynamic (0, 0) = 3 stored rows.
    expect(buttons, findsNWidgets(3));
    await tester.tap(buttons.last);
    await tester.pump();
    expect(recalled, [(BlockType.dynamic.value, 3, 0, 0)]);
  });

  testWidgets('without a recall callback the rows are read-only', (tester) async {
    await pump(tester);
    expect(find.widgetWithIcon(IconButton, Icons.restore), findsNothing);
  });

  testWidgets('the 0.5 s auto-refresh re-reads values, not the topology', (tester) async {
    // The documented auto-refresh is about *values*; re-enumerating types/instances/metas every
    // 0.5 s cost ~8-12 bus round-trips for topology that only changes on an explicit edit.
    final client = _CountingRegisterClient(deviceId: 1);
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: RegisterPage(deviceId: 1, isConnected: () => true, clientFactory: (_) => client),
    ));
    await tester.pump(); // initState's first (full) refresh
    expect(client.readBlocksCalls, 1, reason: 'the initial load reads the topology');

    // Several auto-refresh ticks inside the topology interval must not re-enumerate.
    await tester.pump(const Duration(seconds: 2));
    await tester.pump(const Duration(seconds: 2));
    expect(client.readBlocksCalls, 1,
        reason: 'ticks inside the topology interval only re-read values');

    // Past the interval the topology is refreshed again (so a block added elsewhere shows up).
    await tester.pump(const Duration(seconds: 6));
    expect(client.readBlocksCalls, 2, reason: 'the topology is re-read periodically');

    await tester.pumpWidget(const SizedBox()); // dispose (cancels the auto-refresh timer)
  });

  testWidgets('the page toggles between the Current and Backup views', (tester) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: const RegisterPage(deviceId: 1),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull);
    // Current view: the appbar offers Save and the toggle reads "Current".
    expect(find.text('Current'), findsOneWidget);
    expect(find.byTooltip('Save all to backup'), findsOneWidget);
    expect(find.byTooltip('Recall all from backup'), findsNothing);

    await tester.tap(find.text('Current'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    expect(tester.takeException(), isNull);
    // Backup view: the action becomes Recall and the toggle reads "Backup".
    expect(find.text('Backup'), findsOneWidget);
    expect(find.byTooltip('Recall all from backup'), findsOneWidget);
    expect(find.byTooltip('Save all to backup'), findsNothing);

    // Toggling back restores the Current view.
    await tester.tap(find.text('Backup'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('Current'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
}
