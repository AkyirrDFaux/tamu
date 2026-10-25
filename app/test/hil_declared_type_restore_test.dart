// Card t_ef2b1461: a created dynamic row carries its DECLARED type and size.
//
// Measured on the rig (2026-10-25, read-only before and after):
//
//   Dynamic[0] f4 k0 before: type=0x0006 (Number) size=1   <- the archive declares Bool, 1 byte
//   Dynamic[0] f4 k0 after : type=0x0004 (Bool)   size=1
//
// The row is the one the emote selector reads at line 27: a 1-byte `Number` cannot be converted to
// a Bool destination, so the read lands as SCRIPT_ERR_TYPE. The repair is a restore that re-creates
// the row in the archive's shape - a dynamic row carries its OWN type and length, so the write
// carries the declared type and size together instead of the live row's.
//
// Destructive-ish by nature (it is a restore): it writes ONE row, the one the archive declares
// differently, plus an eye value round-trip that is put back. It saves the device afterwards, which
// is what keeps the repair across a power cycle. Tagged `hil`, so `./test.sh` never runs it.
//
// PRECONDITION: the rig must hold `Dynamic[0] f4 k0` as a 1-byte `Number` (the card's damaged
// shape), because that is what the re-create is measured against. The test does NOT assume the rig
// was left that way by hand - a previous run of this file leaves the row repaired, and a repaired
// row cannot be re-created. So the row is read first, and when it is not `Number`/1 it is written
// that way with one `writeDynamicEntry` (the same single register write as
// `.dart_tool/hermes/set_row.py 0x0006 1`) before the archive is captured. The re-create path is
// therefore exercised on every run, on any rig state, and `report.recreated` is never empty.
//
//   TAMU_HIL=/dev/ttyACM0 flutter test --tags hil test/hil_declared_type_restore_test.dart
@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';

import 'hil/hil_env.dart';

/// `Dynamic[0]` - the block the emote selector's register read addresses.
const int _ownerBlock = 0;
const int _ownerField = 4;
const int _ownerKey = 0;

/// `Dynamic[1]` - the left eye, two keys per field: the value round-trip's address.
const int _eyeBlock = 1;
const int _eyeField = 1;
const int _eyeKey = 1;

String _hex(List<int> bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

String _metaLabel(ValueInfo meta) =>
    '[${_hex(meta.toBytes())}] type=0x'
    '${meta.type.toRadixString(16).padLeft(4, '0')} size=${meta.size} '
    'flags=0x${meta.flags.toRadixString(16).padLeft(2, '0')}';

Future<DynField?> _readRow(int device, int global, int field, int key) =>
    RegisterClient(deviceId: device).readDynamicField(
      DynBlock(
        index: global,
        meta: ValueInfo(type: dynamicTypeForIndex(global)),
        name: '',
      ),
      field,
      key,
    );

/// Every loaded script slot's report record: `state/depth/line` and, on a fault, the instruction
/// and its error. A script whose read addressed the damaged row reports SCRIPT_ERR_TYPE here.
Future<List<String>> _scriptStates(int device) async {
  final client = ScriptClient(deviceId: device);
  final slots = await client.loadedSlotMap();
  final out = <String>[];
  for (final entry in slots.entries) {
    final report = await client.readState(entry.key);
    out.add(
      report == null
          ? 'slot ${entry.key} (${scriptFileName(entry.value)}): no report'
          : 'slot ${entry.key} (${scriptFileName(entry.value)}): state '
                '${report.state} line ${report.line} '
                '${report.hasError ? 'ERROR instruction 0x${report.instruction.toRadixString(16)} '
                          'error ${report.error}' : 'no fault'}',
    );
  }
  return out;
}

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('a restore re-creates Dynamic[0] f4 k0 as the declared Bool', () async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    final core = DeviceDatabase.instance.byId(coreId)?.id ?? coreId;

    // ---- read-only BEFORE -------------------------------------------------
    final asFound = await _readRow(core, _ownerBlock, _ownerField, _ownerKey);
    expect(asFound, isNotNull, reason: 'the device carries Dynamic[0] f4 k0');
    // ignore: avoid_print
    print('RIG as found: ${_metaLabel(asFound!.meta)} '
        'value=${_hex(asFound.value)}');

    // ---- the precondition: this run needs the card's damaged shape ---------
    // A restore leaves f4 k0 repaired as `Bool`/1, so a second run of this file on the same rig
    // would find nothing to re-create. Put the row back in the shape the card measured (a 1-byte
    // `Number`, one register write - `set_row.py 0x0006 1`) instead of requiring a hand-prepared
    // rig: the re-create path is then exercised on every run, on any rig state.
    final holdsDamaged =
        asFound.meta.type == DataType.number.value && asFound.meta.size == 1;
    if (!holdsDamaged) {
      final seeded = await RegisterClient(deviceId: core).writeDynamicEntry(
        DynBlock(
          index: _ownerBlock,
          meta: ValueInfo(type: dynamicTypeForIndex(_ownerBlock)),
          name: '',
        ),
        _ownerField,
        _ownerKey,
        ValueInfo(type: DataType.number.value, size: 1),
        const [0x00],
      );
      expect(seeded, isNotNull, reason: 'the damaged precondition row was set');
      final reseeded = await _readRow(core, _ownerBlock, _ownerField, _ownerKey);
      expect(reseeded, isNotNull);
      expect(reseeded!.meta.type, DataType.number.value);
      expect(reseeded.meta.size, 1);
      // ignore: avoid_print
      print('SETUP: wrote the card\'s shape (Number, 1 byte) into f4 k0');
    }

    final before = await _readRow(core, _ownerBlock, _ownerField, _ownerKey);
    expect(before, isNotNull);
    // ignore: avoid_print
    print(
      'BEFORE Dynamic[0].f4.k0: ${_metaLabel(before!.meta)} '
      'value=${_hex(before.value)}',
    );
    final statesBefore = await _scriptStates(core);
    for (final line in statesBefore) {
      // ignore: avoid_print
      print('BEFORE $line');
    }

    // ---- the archive: a capture, with the row declared as the archive declares it -------
    final capture = await captureDevice(core, includeFiles: false);
    expect(capture, isNotNull, reason: 'the core capturable');
    final archive = capture!;
    final blockAt = archive.blocks.indexWhere(
      (b) => b.typeIndex == dynamicTypeForIndex(_ownerBlock),
    );
    expect(blockAt, isNot(-1), reason: 'the archive carries Dynamic[0]');
    final source = archive.blocks[blockAt];
    final rowAt = source.entries.indexWhere(
      (e) => e.fieldIndex == _ownerField && e.keyIndex == _ownerKey,
    );
    expect(rowAt, isNot(-1), reason: 'the archive carries f4 k0');
    final captured = source.entries[rowAt];
    // ignore: avoid_print
    print(
      'ARCHIVE f4 k0 as captured: type=${captured.type} size=${captured.size} '
      'value=${captured.value}',
    );
    // The owner's archive declares this row a `Bool` of one byte. A capture taken from THIS device
    // would carry the damage instead (the row it reads is what the device holds), so the declared
    // shape is set here - that is the condition the card describes, not an assumption about it.
    final declared = BackupEntry(
      field: captured.field,
      fieldIndex: captured.fieldIndex,
      key: captured.key,
      keyIndex: captured.keyIndex,
      type: dataTypeWord(DataType.bool_),
      flags: captured.flags,
      value: true,
      size: 1,
    );
    final entries = List<BackupEntry>.of(source.entries);
    entries[rowAt] = declared;
    archive.blocks[blockAt] = BackupBlock(
      type: source.type,
      typeIndex: source.typeIndex,
      instance: source.instance,
      name: source.name,
      isDynamic: source.isDynamic,
      entries: entries,
    );
    // ignore: avoid_print
    print('ARCHIVE f4 k0 as declared: Bool, 1 byte');

    final plan = await buildRestorePlan([archive]);
    // Only the row under test is applied: this is a measurement of ONE repair, not a full mirror.
    for (final item in plan.items) {
      item.selected =
          item.kind == RestoreKind.entry &&
          item.block?.instance == _ownerBlock &&
          item.entry?.fieldIndex == _ownerField &&
          item.entry?.keyIndex == _ownerKey;
    }
    final report = await applyRestorePlan(plan);
    // ignore: avoid_print
    print('--- report ---\n${report.text}\n--- end ---');
    expect(report.failed, 0, reason: report.text);

    // ---- read-only AFTER ---------------------------------------------------
    final after = await _readRow(core, _ownerBlock, _ownerField, _ownerKey);
    expect(after, isNotNull);
    // ignore: avoid_print
    print(
      'AFTER  Dynamic[0].f4.k0: ${_metaLabel(after!.meta)} '
      'value=${_hex(after.value)}',
    );
    expect(after.meta.type, DataType.bool_.value);
    expect(after.meta.size, 1);
    expect(after.value, hasLength(1));

    // The run says WHICH row it repaired, and what the device held before.
    expect(report.recreated, isNotEmpty, reason: report.text);
    final repaired = report.recreated.firstWhere(
      (r) => r.subject.contains('field $_ownerField key $_ownerKey'),
      orElse: () => report.recreated.first,
    );
    expect(repaired.shape, startsWith('Bool, 1 byte'));
    // ignore: avoid_print
    print('REPORT ${repaired.line}');

    // ---- the eye round-trip: a value written is the value read back ---------
    final eyeBefore = await _readRow(core, _eyeBlock, _eyeField, _eyeKey);
    expect(eyeBefore, isNotNull, reason: 'the device carries the eye row');
    final eyeMeta = eyeBefore!.meta;
    final original = [...eyeBefore.value];
    final probe = [for (final b in original) b ^ 0x01];
    final reg = RegisterClient(deviceId: core);
    final block = DynBlock(
      index: _eyeBlock,
      meta: ValueInfo(type: dynamicTypeForIndex(_eyeBlock)),
      name: '',
    );
    final written = await reg.writeDynamicEntry(
      block,
      _eyeField,
      _eyeKey,
      ValueInfo(
        type: eyeMeta.type,
        flags: eyeMeta.flags,
        key: _eyeKey,
        size: probe.length,
      ),
      probe,
    );
    expect(written, isNotNull, reason: 'the eye write landed');
    final eyeAfter = await _readRow(core, _eyeBlock, _eyeField, _eyeKey);
    expect(eyeAfter, isNotNull);
    // ignore: avoid_print
    print(
      'EYE Dynamic[$_eyeBlock].f$_eyeField.k$_eyeKey: '
      'wrote ${_metaLabel(eyeMeta)} value=${_hex(probe)} '
      'read back ${_metaLabel(eyeAfter!.meta)} value=${_hex(eyeAfter.value)}',
    );
    expect(eyeAfter.meta.type, eyeMeta.type);
    expect(eyeAfter.value, probe);
    // Put the eye back where it was: the rig is left as found.
    final restored = await reg.writeDynamicEntry(
      block,
      _eyeField,
      _eyeKey,
      ValueInfo(
        type: eyeMeta.type,
        flags: eyeMeta.flags,
        key: _eyeKey,
        size: original.length,
      ),
      original,
    );
    expect(restored, isNotNull, reason: 'the eye value was put back');
    final eyeSettled = await _readRow(core, _eyeBlock, _eyeField, _eyeKey);
    // ignore: avoid_print
    print('EYE put back: value=${_hex(eyeSettled!.value)}');
    expect(eyeSettled.value, original);

    final statesAfter = await _scriptStates(core);
    for (final line in statesAfter) {
      // ignore: avoid_print
      print('AFTER $line');
    }

    // ---- the emote selector: the fault the damaged row caused ---------------
    // Slot 4 (SCR_004) is the script the card names: its report record is an error at line 27 with
    // error 2 (`SCRIPT_ERR_TYPE`) - the register read of the row that was a 1-byte Number. An Error
    // state is LATCHED: the program stopped on that instruction and does not retry, so the repair
    // only shows once it runs again. Restart it the way the Scripts page does (stop, then start)
    // and read the record again.
    final scripts = ScriptClient(deviceId: core);
    final slots = await scripts.loadedSlotMap();
    int? emote;
    for (final entry in slots.entries) {
      if (entry.value == 4) emote = entry.key;
    }
    if (emote == null) {
      // ignore: avoid_print
      print(
        'SLOT 4: no SCR_004 is loaded, so the restart check does not apply',
      );
    } else {
      final latched = await scripts.readState(emote);
      expect(latched, isNotNull);
      // ignore: avoid_print
      print(
        'SLOT $emote (SCR_004) before restart: state ${latched!.state} '
        '(${ScriptState.label(latched.state)}) line ${latched.line} '
        'instruction 0x${latched.instruction.toRadixString(16)} '
        'error ${latched.error}',
      );
      await scripts.setAction(emote, ScriptAction.stop);
      await Future<void>.delayed(const Duration(milliseconds: 400));
      await scripts.setAction(emote, ScriptAction.start);
      await Future<void>.delayed(const Duration(milliseconds: 2000));
      final running = await scripts.readState(emote);
      expect(running, isNotNull);
      // ignore: avoid_print
      print(
        'SLOT $emote (SCR_004) after restart : state ${running!.state} '
        '(${ScriptState.label(running.state)}) line ${running.line} '
        '${running.hasError ? 'instruction 0x${running.instruction.toRadixString(16)} '
                  'error ${running.error}' : 'no fault'}',
      );
      expect(
        running.state,
        isNot(ScriptState.error),
        reason:
            'the emote selector must not be in Error once the row it '
            'reads is the declared Bool',
      );
      expect(running.hasError, isFalse);
    }
  });
}
