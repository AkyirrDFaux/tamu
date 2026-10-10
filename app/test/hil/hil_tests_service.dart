/// Service tests: one device and one service (Docs/HIL Tests.md).
///
/// Ported from the legacy `hil_*` / `hardware_*` / `dyn_flow_*` files; each test
/// carries the capability bits its service needs, so an unmet prerequisite is a
/// skip. Every test isolates its scratch state (a scratch file, a scratch script
/// slot, a scratch dynamic block index) and restores it, so the example setup
/// (scripts 0-4, dynamic blocks 0-2) is never left disturbed.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_backup.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';

/// The core, reachable through the app interface (the rig's entry condition).
const HilPrereq _core =
    HilPrereq(isCore: true, capabilities: [Capability.appInterface]);

/// The dynamic-memory service.
const HilPrereq _dynamic =
    HilPrereq(capabilities: [Capability.dynamicMemory]);

/// The storage file service.
/// The core carries the larger filesystem; the 250-byte round trip is sized for
/// it. Storage files are universal since the old StorageFiles capability bit was
/// dropped (every device has storage files), so the device class is the only
/// usable discriminator.
const HilPrereq _storage = HilPrereq(isCore: true);

/// A node's filesystem is small, so the node variant uses a 16-byte file.
const HilPrereq _nodeStorage = HilPrereq(isCore: false);

/// Dynamic persistence writes both the block table and the file table.
const HilPrereq _dynamicStorage =
    HilPrereq(capabilities: [Capability.dynamicMemory]);

/// The script service stores its `SCR_XXX` images in the file service.
const HilPrereq _script = HilPrereq(capabilities: [Capability.scripts]);

/// The requester subscription table is persisted into the file service.
const HilPrereq _subscription =
    HilPrereq(capabilities: [Capability.subscriptionRequest]);

/// Scratch script slots, clear of the example setup's slots (0-4).
const int _scrSlotBase = 40;

/// A scratch dynamic block index, clear of the example setup's blocks (0-2).
const int _scrDyn = 32;

/// The two hex digits the `.DT_XX` / `.DV_XX` files use for a global index.
String _dynSuffix(int global) =>
    global.toRadixString(16).toUpperCase().padLeft(2, '0');

/// Uploads [draft] into [slot] and loads it; false when either step fails.
Future<bool> _uploadScript(
    ScriptClient c, StorageClient st, int slot, ScriptDraft draft) async {
  if (await c.readState(slot) != null) await c.unload(slot);
  final name = scriptFileName(slot);
  await st.deleteFile(name);
  if (!await st.writeFile(name, draft.toImage())) return false;
  return c.load(slot, slot);
}

/// Unloads [slot] and removes its scratch file.
Future<void> _removeScript(ScriptClient c, StorageClient st, int slot) async {
  await c.unload(slot);
  await st.deleteFile(scriptFileName(slot));
}

/// Polls a loaded script's state until it reaches [want] (or the tries run out).
Future<int> _waitState(ScriptClient c, int slot, int want,
    {int tries = 40}) async {
  for (var i = 0; i < tries; i++) {
    final s = (await c.readState(slot))?.state;
    if (s == want) return s!;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  return (await c.readState(slot))?.state ?? -1;
}

/// The current file names of a device, normalized.
Future<Set<String>> _fileNames(StorageClient st) async {
  final table = await st.readFileTable();
  if (table == null) return <String>{};
  return table.map((f) => normalizeFileName(f.name)).toSet();
}

final List<HilTest> serviceTests = <HilTest>[
  // ---------------------------------------------------------------- Register

  HilTest(
    'register answers the block enumeration and the System block reads',
    category: HilCategory.service,
    prereq: _core,
    body: (ctx) async {
      for (final d in ctx.devices(_core)) {
        final reg = RegisterClient(deviceId: d.id);

        // CID 0 (empty request): a stream of packed `(type << 6) | maxInstance` words.
        final types = await reg.enumerateBlockTypes();
        expect(types, isNotNull, reason: 'device ${d.id} answers enumeration');
        expect(types!, isNotEmpty, reason: 'device ${d.id} has static blocks');
        expect(types.first.type, 0, reason: 'device ${d.id} System block leads');
        ctx.note('${d.label}: ${types.length} block type(s), '
            'first 0x${types.first.type.toRadixString(16)}');

        // System DeviceType (type 0, inst 0, field 0).
        final devType =
            await reg.request(RegisterCid.read, payload: blockInfoBytes(0, 0, 0, 0));
        expect(devType, isNotNull, reason: 'device ${d.id} reads DeviceType');
        expect(devType!.length, greaterThanOrEqualTo(8),
            reason: 'DeviceType reply carries ValueInfo + value');

        // System SN (type 0, inst 0, field 1, key 0xFF): a 14-byte value.
        final sn = await reg.request(
            RegisterCid.read, payload: blockInfoBytes(0, 0, 1, 0xFF));
        expect(sn, isNotNull, reason: 'device ${d.id} reads the SN');
        expect(sn!.length, greaterThanOrEqualTo(8 + 14),
            reason: 'SN reply carries a 14-byte serial');
      }
    },
  ),

  HilTest(
    'dynamic memory page flow: create, append, edit, delete, refresh',
    category: HilCategory.service,
    prereq: _dynamic,
    body: (ctx) async {
      for (final d in ctx.devices(_dynamic)) {
        final dyn = RegisterClient(deviceId: d.id);

        // Scratch block only - never the example setup's blocks.
        await dyn.deleteDynamic(block: _scrDyn);

        expect(await dyn.createDynamicBlock('DPROBE', index: _scrDyn), isNotNull,
            reason: 'create a scratch dynamic block');
        var b = (await dyn.readDynamicBlocks())!
            .firstWhere((x) => x.index == _scrDyn);
        expect(b.fieldCount, 0, reason: 'a fresh block has no fields');

        final e1 = await dyn.appendDynamicEntry(b,
            ValueInfo(type: DataType.number.value, size: 4), numberToBytes(1.0));
        expect(e1, isNotNull, reason: 'append an entry');
        b = (await dyn.readDynamicBlocks())!.firstWhere((x) => x.index == _scrDyn);
        expect(b.fieldCount, 1);

        final bOpen = (await dyn.readDynamicBlockMeta(_scrDyn))!;
        final f0 = await dyn.readDynamicField(bOpen, 0);
        expect(f0, isNotNull);
        expect(numberFromBytes(f0!.value), closeTo(1.0, 0.001));

        final edit = await dyn.writeDynamicField(bOpen, f0, numberToBytes(2.5));
        expect(edit, isNotNull, reason: 'edit an entry');
        expect(numberFromBytes(edit!), closeTo(2.5, 0.001));

        expect(await dyn.deleteDynamic(block: _scrDyn, field: 0), isTrue);
        b = (await dyn.readDynamicBlocks())!.firstWhere((x) => x.index == _scrDyn);
        expect(b.fieldCount, 0);
        expect(await dyn.readDynamicField(b, 0), isNull);

        final e2 = await dyn.appendDynamicEntry(b,
            ValueInfo(type: DataType.number.value, size: 4), numberToBytes(3.0),
            index: 0);
        expect(e2, isNotNull, reason: 'fill a None slot');
        b = (await dyn.readDynamicBlocks())!.firstWhere((x) => x.index == _scrDyn);
        final f0c = await dyn.readDynamicField(b, 0);
        expect(f0c, isNotNull);
        expect(numberFromBytes(f0c!.value), closeTo(3.0, 0.001));

        // A read-only entry rejects both an external value write and a delete.
        final e3 = await dyn.appendDynamicEntry(
            b,
            ValueInfo(
                type: DataType.number.value, flags: ValueFlags.readOnly, size: 4),
            numberToBytes(9.0),
            index: 1);
        expect(e3, isNotNull, reason: 'append a read-only entry');
        b = (await dyn.readDynamicBlocks())!.firstWhere((x) => x.index == _scrDyn);
        final ro = await dyn.readDynamicField(b, 1);
        expect(ro, isNotNull);
        expect(ro!.meta.readOnly, isTrue);
        expect(await dyn.writeDynamicField(b, ro, numberToBytes(10.0)), isNull,
            reason: 'a read-only entry rejects writes');
        expect(await dyn.deleteDynamic(block: _scrDyn, field: 1), isFalse,
            reason: 'a read-only entry rejects deletes');

        // Restore: drop the scratch block and compact.
        await dyn.deleteDynamic(block: _scrDyn);
        await dyn.saveAll();
      }
    },
  ),

  HilTest(
    'a write whose ValueInfo.Size exceeds the payload is clamped',
    category: HilCategory.service,
    prereq: _dynamic,
    body: (ctx) async {
      for (final d in ctx.devices(_dynamic)) {
        final reg = RegisterClient(deviceId: d.id);
        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();
        await reg.createDynamicBlock('CLAMP', index: _scrDyn);
        final b = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: dynamicTypeForIndex(_scrDyn), size: 1),
            name: 'CLAMP');

        // Declare size 16 but send only 4 value bytes: the device must not read
        // past the frame.
        final payload = <int>[
          ...blockInfoBytes(dynamicTypeForIndex(_scrDyn),
              dynamicInstanceForIndex(_scrDyn), 0, 0),
          ...ValueInfo(type: DataType.number.value, key: 0, size: 16).toBytes(),
          ...numberToBytes(1.5),
        ];
        final reply = await reg.request(RegisterCid.write, payload: payload);
        expect(reply, isNotNull, reason: 'oversized write got a reply');

        final e = await reg.readDynamicField(b, 0);
        ctx.note('clamp entry size=${e?.meta.size} value=${e?.value}');
        expect(e, isNotNull);
        expect(e!.meta.size, 4, reason: 'the oversized write is clamped to the payload');
        expect(e.value.length, 4);

        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();
      }
    },
  ),

  HilTest(
    'Save All writes the per-block DT/DV files and cleans them on delete',
    category: HilCategory.service,
    prereq: _dynamicStorage,
    body: (ctx) async {
      for (final d in ctx.devices(_dynamicStorage)) {
        final reg = RegisterClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final suffix = _dynSuffix(_scrDyn);
        final dtName = '.DT_$suffix';
        final dvName = '.DV_$suffix';

        // Clean slate for the scratch block only.
        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();

        // RENDER at the scratch index: persistent (0,0)=42, volatile (0,1)=7.
        await reg.createDynamicBlock('RENDER', index: _scrDyn);
        final b = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: dynamicTypeForIndex(_scrDyn), size: 1),
            name: 'RENDER');
        await reg.writeDynamicEntry(
            b,
            0,
            0,
            ValueInfo(
                type: DataType.number.value, flags: ValueFlags.persistent, key: 0),
            numberToBytes(42.0));
        await reg.writeDynamicEntry(b, 0, 1,
            ValueInfo(type: DataType.number.value, key: 1), numberToBytes(7.0));
        expect(await reg.saveAll(), isTrue);

        var files = await _fileNames(st);
        expect(files.contains(dtName) && files.contains(dvName), isTrue,
            reason: '$dtName/$dvName missing after Save All');

        // The persistent value is in .DV_XX (read through the .DT_/.DV_ files).
        final dt = await st.readFile(dtName);
        final dv = await st.readFile(dvName);
        final table = dt == null ? null : decodeDynamicTable(dt);
        expect(table, isNotNull, reason: '$dtName decodes');
        expect(dv, isNotNull, reason: '$dvName reads');
        final backup = decodeDynamicValues(_scrDyn, table!, dv!)
            .where((e) => e.field == 0 && e.key == 0)
            .firstOrNull;
        expect(backup, isNotNull, reason: 'the persistent entry decodes');
        expect(numberFromBytes(backup!.value), closeTo(42.0, 0.01));

        // Delete + Save All removes the files; recreate at the same index works.
        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();
        files = await _fileNames(st);
        expect(files.contains(dtName) || files.contains(dvName), isFalse,
            reason: 'tombstoned files are cleaned on Save All');
        expect(await reg.createDynamicBlock('RENDER', index: _scrDyn), _scrDyn,
            reason: 'recreate at the scratch index');
        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();
      }
    },
  ),

  HilTest(
    'dynamic persistence uses the .DT_/.DV_ files in the documented format',
    category: HilCategory.service,
    prereq: _dynamicStorage,
    body: (ctx) async {
      for (final d in ctx.devices(_dynamicStorage)) {
        final reg = RegisterClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final suffix = _dynSuffix(_scrDyn);
        final dtName = '.DT_$suffix';
        final dvName = '.DV_$suffix';

        await reg.deleteDynamic(block: _scrDyn);
        await reg.createDynamicBlock('DYNCHK', index: _scrDyn);
        final blk = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: BlockType.dynamic.value, size: 1),
            name: 'DYNCHK');
        await reg.writeDynamicEntry(
            blk,
            0,
            0,
            ValueInfo(
                type: DataType.number.value, flags: ValueFlags.persistent, size: 4),
            numberToBytes(1.0));
        expect(await reg.saveAll(), isTrue);

        final names =
            (await st.readFileTable() ?? []).map((r) => normalizeFileName(r.name)).toSet();
        expect(names.contains(dtName), isTrue, reason: '$dtName exists after Save All');
        expect(names.contains(dvName), isTrue, reason: '$dvName exists after Save All');

        // Name (16 chars, NUL-padded), u16 entry_count, u16 reserved, then 8 B per
        // entry (Field&Key, MemoryOffset, ValueInfo).
        final bytes = await st.readFile(dtName, size: 64);
        expect(bytes, isNotNull);
        expect(String.fromCharCodes(bytes!.sublist(0, 6)), 'DYNCHK');
        expect(bytes[16] | (bytes[17] << 8), 1, reason: 'entry_count');
        expect(bytes[20] | (bytes[21] << 8), 0, reason: 'field 0 / key 0');
        expect(bytes[22] | (bytes[23] << 8), 0, reason: 'memory offset');
        expect((bytes[24] | (bytes[25] << 8)) & 0x3FF, DataType.number.value);
        expect(bytes[27] & ValueFlags.persistent, isNot(0));
        expect(bytes[26], 4, reason: 'entry size');

        await reg.deleteDynamic(block: _scrDyn);
        await reg.saveAll();
      }
    },
  ),

  // ----------------------------------------------------------------- Storage

  HilTest(
    'storage create/read/write/delete round-trips',
    category: HilCategory.service,
    prereq: _storage,
    body: (ctx) async {
      for (final d in ctx.devices(_storage)) {
        final storage = StorageClient(deviceId: d.id);
        const name = 'TESTFIL1';

        await storage.deleteFile(name);
        expect(await storage.createFile(name, 250), isTrue,
            reason: 'device ${d.id} creates a file');

        final data = List<int>.generate(250, (i) => i & 0xFF);
        expect(await storage.writeFile(name, data), isTrue,
            reason: 'device ${d.id} writes the file');
        expect(await storage.readFile(name, size: 250), equals(data),
            reason: 'device ${d.id} reads the same bytes back');
        expect(await storage.deleteFile(name), isTrue,
            reason: 'device ${d.id} deletes the file');
      }
    },
  ),

  HilTest(
    'storage round-trips a small file on a node',
    category: HilCategory.service,
    prereq: _nodeStorage,
    body: (ctx) async {
      for (final d in ctx.devices(_nodeStorage)) {
        final storage = StorageClient(deviceId: d.id);
        const name = 'NODEFIL1';

        await storage.deleteFile(name);
        expect(await storage.createFile(name, 16), isTrue,
            reason: 'node ${d.id} creates a 16-byte file');
        final data = List<int>.generate(16, (i) => (i * 7) & 0xFF);
        expect(await storage.writeFile(name, data), isTrue,
            reason: 'node ${d.id} writes the file');
        expect(await storage.readFile(name, size: 16), equals(data),
            reason: 'node ${d.id} reads the same bytes back');
        expect(await storage.deleteFile(name), isTrue,
            reason: 'node ${d.id} deletes the file');
      }
    },
  ),

  HilTest(
    'the file table never holds two files with the same name',
    category: HilCategory.service,
    prereq: _subscription,
    body: (ctx) async {
      for (final d in ctx.devices(_subscription)) {
        final storage = StorageClient(deviceId: d.id);
        final subs = SubscriptionClient(deviceId: d.id);

        // Create is idempotently refused while the file exists.
        await storage.deleteFile('BKDUP');
        expect(await storage.createFile('BKDUP', 16), isTrue);
        expect(await storage.createFile('BKDUP', 16), isFalse,
            reason: 'creating an existing name must fail');
        await storage.deleteFile('BKDUP');

        // Create/delete and rename cycles must not leave superseded records.
        for (var i = 0; i < 3; i++) {
          await storage.createFile('BKDUP', 16);
          await storage.deleteFile('BKDUP');
        }
        await storage.deleteFile('RENAMED');
        await storage.createFile('TMPNAME', 16);
        expect(await storage.renameFile('TMPNAME', 'RENAMED'), isTrue);
        await storage.deleteFile('RENAMED');

        // Each requester-table save writes .SUBREQ through a temp+rename. Use a
        // scratch entry (its TRID is allocated by the client) and cancel it after,
        // so the example setup's subscriptions are left untouched.
        final before =
            (await subs.getRequesterSubscriptions()).map((s) => s.trid).toSet();

        // The requester table writes .SUBREQ through a temp+rename: the file exists
        // while the table holds an entry and is removed when the table empties.
        RequesterSubscription scratch() => RequesterSubscription(
              index: 0,
              providerAddr: d.id, // self, so the provider CID completes quickly
              trid: 0,
              sourceReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
              targetReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
              trigger: TriggerType.periodic,
              periodMs: 500,
              minTimeMs: 100,
            );

        Future<Map<String, int>> fileCounts() async {
          final table = await storage.readFileTable() ?? const <FileRecord>[];
          final counts = <String, int>{};
          for (final record in table) {
            final name = normalizeFileName(record.name);
            counts[name] = (counts[name] ?? 0) + 1;
          }
          return counts;
        }

        // Churn: set and cancel three times; the table ends where it started.
        for (var i = 0; i < 3; i++) {
          await subs.setRequesterSubscription(0, entry: scratch());
          final added = (await subs.getRequesterSubscriptions())
              .where((s) => !before.contains(s.trid))
              .toList();
          for (final s in added) {
            await subs.cancelRequesterSubscription(s.trid);
          }
        }
        await Future<void>.delayed(const Duration(milliseconds: 300));

        var counts = await fileCounts();
        ctx.note('files after churn: $counts');
        for (final e in counts.entries) {
          expect(e.value, 1, reason: 'duplicate file "${e.key}"');
        }
        if (before.isEmpty) {
          expect(counts.containsKey('.SUBREQ'), isFalse,
              reason: '.SUBREQ is removed when the requester table empties');
        }

        // Hold one entry: .SUBREQ exists while the table is non-empty.
        await subs.setRequesterSubscription(0, entry: scratch());
        await Future<void>.delayed(const Duration(milliseconds: 300));
        counts = await fileCounts();
        expect(counts.containsKey('.SUBREQ'), isTrue,
            reason: '.SUBREQ exists while the requester table holds an entry');

        // Cancel it again and the file goes away.
        final heldAgain = (await subs.getRequesterSubscriptions())
            .where((s) => !before.contains(s.trid))
            .toList();
        for (final s in heldAgain) {
          await subs.cancelRequesterSubscription(s.trid);
        }
        await Future<void>.delayed(const Duration(milliseconds: 300));
        counts = await fileCounts();
        if (before.isEmpty) {
          expect(counts.containsKey('.SUBREQ'), isFalse,
              reason: '.SUBREQ is removed again once the table empties');
        }
        expect(counts.containsKey('DYNMEM'), isFalse,
            reason: 'the obsolete DYNMEM file must be gone');
      }
    },
  ),

  // ------------------------------------------------------------------ Script

  HilTest(
    'script load, register exposure and unload',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final reg = RegisterClient(deviceId: d.id);
        final slot = _scrSlotBase;
        final name = scriptFileName(slot);

        // Clean scratch slot only (the example setup owns slots 0-4).
        if (await c.readState(slot) != null) await c.unload(slot);
        await st.deleteFile(name);

        final image = ScriptFileBuilder(
          properties: ScriptProperties.loadOnBoot,
          inputs: [ValueInfo(type: DataType.number.value, size: 4)],
          outputs: [ValueInfo(type: DataType.number.value, size: 4)],
          variables: [
            ValueInfo(type: DataType.number.value, size: 4),
            ValueInfo(type: DataType.number.value, size: 4),
          ],
          constants: [ValueInfo(type: DataType.number.value, size: 4)],
          inputDefaults: [numberToBytes(3.5)],
          constantValues: [numberToBytes(9.0)],
          instructions: const [
            // While (true) { } -> stays Running (yields each tick via the loop guard).
            0x00, 0x02, 0x01, 0x00, // instruction: Flow/While
            0x06, 0x05, 0x01, 0x00, // operand: predefine Bool true
            0x05, 0x00, 0x00, 0x00, // Endline
            0x00, 0x02, 0x02, 0x00, // instruction: Flow/End block
            0x05, 0x00, 0x00, 0x00, // Endline
          ],
          functionName: 'Blink',
        ).build();

        expect(await st.writeFile(name, image), isTrue, reason: 'upload $name');
        expect(await c.load(slot, slot), isTrue, reason: 'load $name');
        expect((await c.loadedScripts()).contains(slot), isTrue,
            reason: 'script $slot is listed');

        // Block meta (name + field count) - used by the list page.
        final blockMeta = await c.readBlockMeta(slot);
        expect(blockMeta, isNotNull);
        expect(blockMeta!.name, 'Blink');
        expect(blockMeta.meta.size, ScriptField.count);

        // The script block is visible through the Register service.
        final sType = scriptTypeForIndex(slot);
        final sInst = scriptInstanceForIndex(slot);
        final blocks = await reg.readBlocks(scriptSlots: [slot]);
        expect(blocks, isNotNull);
        expect(
            blocks!.any((b) =>
                b != null && isScriptType(b.type) && b.inst == slot),
            isTrue,
            reason: 'script block listed by readBlocks()');
        final inKeys = await reg.getBlockKeys(sType, sInst, ScriptField.input);
        expect(inKeys, isNotNull);
        expect(inKeys!.length, 1, reason: 'script input keys');
        final inEntry =
            await reg.readBlockField(sType, sInst, ScriptField.input, 0);
        expect(inEntry, isNotNull);
        expect(inEntry!.meta.dataType, DataType.number);
        final outEntry =
            await reg.readBlockField(sType, sInst, ScriptField.output, 0);
        expect(outEntry, isNotNull);
        expect(outEntry!.meta.readOnly, isTrue, reason: 'script output is RO');

        // State: a fresh load is Stopped; set/read round-trips.
        expect((await c.readState(slot))!.state, ScriptState.stopped);
        expect(await c.setState(slot, ScriptState.running), isTrue);
        expect((await c.readState(slot))!.state, ScriptState.running);
        expect((await c.readState(slot))!.error, 0,
            reason: 'setState clears the error code');

        // File metadata lives in SCR_XXX; the Register exposes only the I/O.
        final parsed = ScriptFileData.parse((await st.readFile(name))!);
        expect(parsed.properties, ScriptProperties.loadOnBoot);
        expect(parsed.inputs.length, 1);
        expect(parsed.variables.length, 2);
        expect((numberFromBytes(parsed.constantValue(0)) - 9.0).abs() <= 0.001,
            isTrue, reason: 'constant value');

        expect((await c.enumerateKeys(slot, ScriptField.input)).length, 1);
        expect(await c.enumerateKeys(slot, ScriptField.variable), isEmpty,
            reason: 'variables are not part of the register');
        expect(await c.enumerateKeys(slot, ScriptField.constant), isEmpty,
            reason: 'constants are not part of the register');

        final input0 = await c.readEntry(slot, ScriptField.input, 0);
        expect(input0, isNotNull);
        expect((numberFromBytes(input0!.value) - 3.5).abs() <= 0.001, isTrue,
            reason: 'input default from the file');
        final output0 = await c.readEntry(slot, ScriptField.output, 0);
        expect(output0, isNotNull);
        expect(output0!.meta.readOnly, isTrue, reason: 'output is read-only');

        // Variables live in the script RAM: CID 7 write + CID 5 read (sliced by size).
        final internal1 = await c.readInternalState(slot);
        expect(internal1, isNotNull);
        expect(internal1!.variables.length, greaterThanOrEqualTo(8));
        expect(numberFromBytes(internal1.variables, 0), closeTo(0.0, 0.001));
        expect(await c.writeVariable(slot, 0, numberToBytes(1.25)), isTrue);
        expect(await c.writeVariable(slot, 1, numberToBytes(2.5)), isTrue);
        final internal2 = await c.readInternalState(slot);
        expect(internal2, isNotNull);
        expect((numberFromBytes(internal2!.variables, 0) - 1.25).abs() <= 0.001,
            isTrue);
        expect((numberFromBytes(internal2.variables, 4) - 2.5).abs() <= 0.001,
            isTrue);

        // Editor debug: stop, move to a line, read the internal state (IC is a line index).
        await c.setState(slot, ScriptState.stopped);
        expect(await c.moveToInstruction(slot, 1), isTrue);
        final internal = await c.readInternalState(slot);
        expect(internal, isNotNull);
        expect(internal!.instructionCounter, 1);

        // Inputs are writable through Register.
        expect(
            await c.writeEntry(
                slot, ScriptField.input, 0, input0.meta, numberToBytes(7.5)),
            isTrue);
        final input2 = await c.readEntry(slot, ScriptField.input, 0);
        expect(input2, isNotNull);
        expect((numberFromBytes(input2!.value) - 7.5).abs() <= 0.001, isTrue);

        // Unload clears the scratch slot.
        expect(await c.unload(slot), isTrue);
        expect((await c.loadedScripts()).contains(slot), isFalse,
            reason: 'the unloaded slot is gone from the list');

        await st.deleteFile(name);
      }
    },
  ),

  HilTest(
    'a created script is stored but not loaded',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 1;
        final name = scriptFileName(slot);

        if (await c.readState(slot) != null) await c.unload(slot);
        await st.deleteFile(name);

        final image = ScriptFileBuilder(functionName: 'Empty').build();
        expect(await st.writeFile(name, image), isTrue, reason: 'create $name');

        // Writing the file must NOT load it.
        expect((await c.loadedScripts()).contains(slot), isFalse,
            reason: 'a new script is not auto-loaded');

        expect(await c.load(slot, slot), isTrue, reason: 'load $name');
        final meta = await c.readBlockMeta(slot);
        expect(meta, isNotNull);
        expect(meta!.name, 'Empty');
        expect(meta.meta.size, ScriptField.count);

        expect(await c.unload(slot), isTrue);
        await st.deleteFile(name);
      }
    },
  ),

  HilTest(
    'edit a stored (not loaded) script',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 2;
        final name = scriptFileName(slot);

        if (await c.readState(slot) != null) await c.unload(slot);
        await st.deleteFile(name);

        // Build an edited draft through the same model the editor uses.
        final draft =
            ScriptDraft(functionName: 'Edited', properties: ScriptProperties.loadOnBoot)
              ..inputs.add(ScriptDraftValue(
                  name: 'Speed',
                  type: DataType.number,
                  size: 4,
                  value: numberToBytes(1.5)))
              ..outputs.add(ScriptDraftValue(
                  name: 'Result', type: DataType.number, size: 4))
              ..variables.add(
                  ScriptDraftValue(name: 'Tmp', type: DataType.number, size: 4))
              ..constants.add(ScriptDraftValue(
                  name: 'Gain',
                  type: DataType.number,
                  size: 4,
                  value: numberToBytes(2.0)))
              ..lines.add(ScriptLine(
                  destinations: [ScriptSymbol.variable(0)],
                  instruction: ScriptSymbol.instruction(catMath, 0),
                  operands: [ScriptSymbol.constant(0)]))
              ..lines.add(ScriptLine(
                  destinations: [ScriptSymbol.output(0)],
                  instruction: ScriptSymbol.instruction(catMath, 0),
                  operands: [ScriptSymbol.input(0)]));

        expect(await st.writeFile(name, draft.toImage()), isTrue,
            reason: 'edit upload');
        expect((await c.loadedScripts()).contains(slot), isFalse,
            reason: 'an edited stored script is not auto-loaded');

        expect(await c.load(slot, slot), isTrue, reason: 'load the edited script');
        final meta = await c.readBlockMeta(slot);
        expect(meta, isNotNull);
        expect(meta!.name, 'Edited', reason: 'the edited name is restored');
        final parsed = ScriptFileData.parse((await st.readFile(name))!);
        expect(parsed.variables.length, 1);
        final internal = await c.readInternalState(slot);
        expect(internal, isNotNull);
        expect(internal!.variables.length, 4);
        final input = await c.readEntry(slot, ScriptField.input, 0);
        expect(input, isNotNull);
        expect((numberFromBytes(input!.value) - 1.5).abs() <= 0.001, isTrue,
            reason: 'edited input default');

        expect(await c.unload(slot), isTrue);
        await st.deleteFile(name);
      }
    },
  ),

  HilTest(
    'a short write fills the rest of a script input',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 3;
        final name = scriptFileName(slot);

        if (await c.readState(slot) != null) await c.unload(slot);
        await st.deleteFile(name);

        // One 16-byte String input.
        final draft = ScriptDraft(functionName: 'Pad')
          ..inputs.add(
              ScriptDraftValue(name: 'Name', type: DataType.text, size: 16));
        expect(await st.writeFile(name, draft.toImage()), isTrue,
            reason: 'upload $name');
        expect(await c.load(slot, slot), isTrue, reason: 'load $name');

        final entry = await c.readEntry(slot, ScriptField.input, 0);
        expect(entry, isNotNull);
        expect(entry!.meta.size, 16, reason: 'string input size');

        // A shorter String write declares its real length; the device must fill the
        // rest of the fixed-size input with NULs rather than leaving stale bytes.
        final meta = ValueInfo(
            type: entry.meta.type, flags: entry.meta.flags, key: 0, size: 3);
        expect(await c.writeEntry(slot, ScriptField.input, 0, meta, 'abc'.codeUnits),
            isTrue,
            reason: 'short string write');
        final back = await c.readEntry(slot, ScriptField.input, 0);
        expect(back, isNotNull);
        expect(back!.value.length, 16, reason: 'string input size after short write');
        expect(String.fromCharCodes(back.value), 'abc${'\x00' * 13}',
            reason: 'the short string is null-padded');

        expect(await c.unload(slot), isTrue);
        await st.deleteFile(name);
      }
    },
  ),

  HilTest(
    'script VM: arithmetic and halt',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 8;

        // A = 2; B = A + A; Out = B; halt
        final draft = ScriptDraft(functionName: 'Arith')
          ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
          ..variables.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4))
          ..variables.add(ScriptDraftValue(name: 'B', type: DataType.number, size: 4))
          ..constants.add(ScriptDraftValue(
              name: 'Two',
              type: DataType.number,
              size: 4,
              value: numberToBytes(2.0)))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.variable(0)],
              instruction: ScriptSymbol.instruction(catMath, 0),
              operands: [ScriptSymbol.constant(0)]))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.variable(1)],
              instruction: ScriptSymbol.instruction(catMath, 0),
              operands: [
                ScriptSymbol.variable(0),
                ScriptSymbol.predefine(preMathOp, 0),
                ScriptSymbol.variable(0),
              ]))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.output(0)],
              instruction: ScriptSymbol.instruction(catMath, 0),
              operands: [ScriptSymbol.variable(1)]))
          ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

        expect(await _uploadScript(c, st, slot, draft), isTrue,
            reason: 'upload + load the arithmetic script');
        await c.setState(slot, ScriptState.running);
        expect(await _waitState(c, slot, ScriptState.finished),
            ScriptState.finished);

        final out = await c.readEntry(slot, ScriptField.output, 0);
        expect(out, isNotNull);
        expect(numberFromBytes(out!.value), closeTo(4.0, 0.001));

        // Variables live in the script RAM, not the Register.
        final ram = (await c.readInternalState(slot))!.variables;
        expect(numberFromBytes(ram, 0), closeTo(2.0, 0.001));
        expect(numberFromBytes(ram, 4), closeTo(4.0, 0.001));

        await _removeScript(c, st, slot);
      }
    },
  ),

  HilTest(
    'script VM: while loop and end block',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 9;

        // A=0; While A < 3 { A = A + 1 }; halt -> A == 3.
        final draft = ScriptDraft(functionName: 'Loop')
          ..variables.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.variable(0)],
              instruction: ScriptSymbol.instruction(catMath, 0),
              operands: [ScriptSymbol.predefine(preIndex, 0)]))
          ..lines.add(ScriptLine(
              instruction: ScriptSymbol.instruction(catFlow, 1),
              operands: [
                ScriptSymbol.variable(0),
                ScriptSymbol.predefine(preMathOp, 14),
                ScriptSymbol.predefine(preIndex, 3),
              ]))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.variable(0)],
              instruction: ScriptSymbol.instruction(catMath, 0),
              operands: [
                ScriptSymbol.variable(0),
                ScriptSymbol.predefine(preMathOp, 0),
                ScriptSymbol.predefine(preIndex, 1),
              ]))
          ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 2)))
          ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

        expect(await _uploadScript(c, st, slot, draft), isTrue,
            reason: 'upload + load the loop script');
        await c.setState(slot, ScriptState.running);
        final state = await _waitState(c, slot, ScriptState.finished);
        expect(state, ScriptState.finished,
            reason: 'error code ${(await c.readState(slot))?.error}');
        final ram = (await c.readInternalState(slot))!.variables;
        expect(numberFromBytes(ram, 0), closeTo(3.0, 0.001));

        await _removeScript(c, st, slot);
      }
    },
  ),

  HilTest(
    'script VM: a type error halts with the Error state and code',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 10;

        // Modulo(Out, Vec, 1): a Vector operand is not a scalar -> operand error.
        final draft = ScriptDraft(functionName: 'Bad')
          ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
          ..variables.add(ScriptDraftValue(name: 'Vec', type: DataType.vector, size: 8))
          ..constants.add(ScriptDraftValue(
              name: 'One',
              type: DataType.number,
              size: 4,
              value: numberToBytes(1)))
          ..lines.add(ScriptLine(
              destinations: [ScriptSymbol.output(0)],
              instruction: ScriptSymbol.instruction(catMath, 5),
              operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(0)]))
          ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

        expect(await _uploadScript(c, st, slot, draft), isTrue,
            reason: 'upload + load the faulty script');
        await c.setState(slot, ScriptState.running);
        expect(await _waitState(c, slot, ScriptState.error), ScriptState.error);
        expect((await c.readState(slot))?.error, 3, reason: 'SCRIPT_ERR_OPERAND');

        await _removeScript(c, st, slot);
      }
    },
  ),

  HilTest(
    'script log and nop execute without error',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 11;

        final draft = ScriptDraft(functionName: 'Log')
          ..constants.add(ScriptDraftValue(
              name: 'Code',
              type: DataType.number,
              size: 4,
              value: numberToBytes(7)))
          ..lines.add(ScriptLine(
              instruction: ScriptSymbol.instruction(catService, 0),
              operands: [ScriptSymbol.constant(0)])) // Log
          ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 4))) // Nop
          ..lines
              .add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6))); // Halt

        expect(await _uploadScript(c, st, slot, draft), isTrue,
            reason: 'upload + load the log script');
        await c.setState(slot, ScriptState.running);
        expect(await _waitState(c, slot, ScriptState.finished),
            ScriptState.finished);
        expect((await c.readState(slot))?.error, 0,
            reason: 'the Log/Nop path does not fault');

        await _removeScript(c, st, slot);
      }
    },
  ),

  // --------------------------------------------------- Subscriptions (table)

  HilTest(
    'the requester subscription table round-trips and persists',
    category: HilCategory.service,
    // The subscription table is network-wide state (Docs/HIL Tests.md), so run
    // coupled: one at a time, after the independent set.
    runClass: HilRunClass.coupled,
    prereq: _subscription,
    body: (ctx) async {
      for (final d in ctx.devices(_subscription)) {
        final subs = SubscriptionClient(deviceId: d.id);
        final storage = StorageClient(deviceId: d.id);

        // Snapshot the setup's table so the scratch entry can be removed without
        // disturbing it.
        final before =
            (await subs.getRequesterSubscriptions()).map((s) => s.trid).toSet();

        expect(
            await subs.setRequesterSubscription(
                0,
                entry: RequesterSubscription(
                  index: 0,
                  providerAddr: d.id, // self, so the provider side completes
                  trid: 0,
                  sourceReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
                  targetReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
                  trigger: TriggerType.periodic,
                  periodMs: 500,
                  minTimeMs: 100,
                )),
            isTrue,
            reason: 'set a requester subscription');

        final added = (await subs.getRequesterSubscriptions())
            .where((s) => !before.contains(s.trid))
            .toList();
        expect(added, hasLength(1),
            reason: 'the scratch entry appears in the requester table');
        final s = added.single;
        expect(s.trigger, TriggerType.periodic);
        expect(s.periodMs, 500);
        expect(s.minTimeMs, 100);
        expect(s.blockFieldS, 6, reason: 'the source register BlockInfo round-trips');

        // The table is persisted into the .SUBREQ file.
        final names = await _fileNames(storage);
        expect(names.contains('.SUBREQ'), isTrue,
            reason: 'the requester table file exists');

        // Restore: cancel only the scratch entry.
        expect(await subs.cancelRequesterSubscription(s.trid), isTrue,
            reason: 'cancel the scratch entry');
        final restored =
            (await subs.getRequesterSubscriptions()).map((x) => x.trid).toSet();
        expect(restored, before, reason: 'the requester table is restored');
      }
    },
  ),
];
