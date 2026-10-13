/// Service tests: one device and one service (Docs/HIL Tests.md).
///
/// Ported from the legacy `hil_*` / `hardware_*` / `dyn_flow_*` files; each test
/// carries the capability bits its service needs, so an unmet prerequisite is a
/// skip. Every test isolates its scratch state (a scratch file, a scratch script
/// slot, a scratch dynamic block index) and restores it, so the deployed scenario
/// (scripts 0-4, dynamic blocks 0-2) is never left disturbed.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_backup.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_functions.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';

/// The core, reachable through the app interface (the rig's entry condition).
const HilPrereq _core = HilPrereq(
  isCore: true,
  capabilities: [Capability.appInterface],
);

/// The dynamic-memory service.
const HilPrereq _dynamic = HilPrereq(capabilities: [Capability.dynamicMemory]);

/// The storage file service.
/// The core carries the larger filesystem; the 250-byte round trip is sized for
/// it. Storage files are universal since the old StorageFiles capability bit was
/// dropped (every device has storage files), so the device class is the only
/// usable discriminator.
const HilPrereq _storage = HilPrereq(isCore: true);

/// A node's filesystem is small, so the node variant uses a 16-byte file.
const HilPrereq _nodeStorage = HilPrereq(isCore: false);

/// Dynamic persistence writes both the block table and the file table.
const HilPrereq _dynamicStorage = HilPrereq(
  capabilities: [Capability.dynamicMemory],
);

/// The script service stores its `SCR_XXX` images in the file service.
const HilPrereq _script = HilPrereq(capabilities: [Capability.scripts]);

/// The requester subscription table is persisted into the file service.
const HilPrereq _subscription = HilPrereq(
  capabilities: [Capability.subscriptionRequest],
);

/// Scratch script slots, clear of the deployed scenario's slots (0-4).
const int _scrSlotBase = 40;

/// A scratch dynamic block index, clear of the deployed scenario's blocks (0-2).
const int _scrDyn = 32;

/// A scratch function-library file id, clear of the deployed scenario's scripts and the other
/// scratch scripts. A function library is a `SCR_XXX` file carrying the Function-library
/// Properties flag (Docs/Services/Script.md "Functions and Macros"), named by the same scheme
/// the firmware links against (`ScriptService::FileName`, three hex digits).
const int _scrLibFileId = _scrSlotBase + 20;

/// The two hex digits the `.DT_XX` / `.DV_XX` files use for a global index.
String _dynSuffix(int global) =>
    global.toRadixString(16).toUpperCase().padLeft(2, '0');

/// Uploads [draft] into [slot] and loads it; false when either step fails.
Future<bool> _uploadScript(
  ScriptClient c,
  StorageClient st,
  int slot,
  ScriptDraft draft,
) async {
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
Future<int> _waitState(
  ScriptClient c,
  int slot,
  int want, {
  int tries = 40,
}) async {
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
        expect(
          types.first.type,
          0,
          reason: 'device ${d.id} System block leads',
        );
        ctx.note(
          '${d.label}: ${types.length} block type(s), '
          'first 0x${types.first.type.toRadixString(16)}',
        );

        // System DeviceType (type 0, inst 0, field 0).
        final devType = await reg.request(
          RegisterCid.read,
          payload: blockInfoBytes(0, 0, 0, 0),
        );
        expect(devType, isNotNull, reason: 'device ${d.id} reads DeviceType');
        expect(
          devType!.length,
          greaterThanOrEqualTo(8),
          reason: 'DeviceType reply carries ValueInfo + value',
        );

        // System SN (type 0, inst 0, field 1, key 0): a 14-byte value. The specification exposes
        // the System struct as a whole at key 0 (the firmware answers 0xFF as well, but 0 is the
        // specified key and the app uses it).
        final sn = await reg.request(
          RegisterCid.read,
          payload: blockInfoBytes(0, 0, 1, 0),
        );
        expect(sn, isNotNull, reason: 'device ${d.id} reads the SN');
        expect(
          sn!.length,
          greaterThanOrEqualTo(8 + 14),
          reason: 'SN reply carries a 14-byte serial',
        );
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
        try {
          // Scratch block only - never the deployed scenario's blocks.
          await dyn.deleteDynamic(block: _scrDyn);

          expect(
            await dyn.createDynamicBlock('DPROBE', index: _scrDyn),
            isNotNull,
            reason: 'create a scratch dynamic block',
          );
          var b = (await dyn.readDynamicBlocks())!.firstWhere(
            (x) => x.index == _scrDyn,
          );
          expect(b.fieldCount, 0, reason: 'a fresh block has no fields');

          final e1 = await dyn.appendDynamicEntry(
            b,
            ValueInfo(type: DataType.number.value, size: 4),
            numberToBytes(1.0),
          );
          expect(e1, isNotNull, reason: 'append an entry');
          b = (await dyn.readDynamicBlocks())!.firstWhere(
            (x) => x.index == _scrDyn,
          );
          expect(b.fieldCount, 1);

          final bOpen = (await dyn.readDynamicBlockMeta(_scrDyn))!;
          final f0 = await dyn.readDynamicField(bOpen, 0);
          expect(f0, isNotNull);
          expect(numberFromBytes(f0!.value), closeTo(1.0, 0.001));

          final edit = await dyn.writeDynamicField(
            bOpen,
            f0,
            numberToBytes(2.5),
          );
          expect(edit, isNotNull, reason: 'edit an entry');
          expect(numberFromBytes(edit!), closeTo(2.5, 0.001));

          expect(await dyn.deleteDynamic(block: _scrDyn, field: 0), isTrue);
          b = (await dyn.readDynamicBlocks())!.firstWhere(
            (x) => x.index == _scrDyn,
          );
          expect(b.fieldCount, 0);
          expect(await dyn.readDynamicField(b, 0), isNull);

          final e2 = await dyn.appendDynamicEntry(
            b,
            ValueInfo(type: DataType.number.value, size: 4),
            numberToBytes(3.0),
            index: 0,
          );
          expect(e2, isNotNull, reason: 'fill a None slot');
          b = (await dyn.readDynamicBlocks())!.firstWhere(
            (x) => x.index == _scrDyn,
          );
          final f0c = await dyn.readDynamicField(b, 0);
          expect(f0c, isNotNull);
          expect(numberFromBytes(f0c!.value), closeTo(3.0, 0.001));

          // A read-only entry rejects both an external value write and a delete.
          final e3 = await dyn.appendDynamicEntry(
            b,
            ValueInfo(
              type: DataType.number.value,
              flags: ValueFlags.readOnly,
              size: 4,
            ),
            numberToBytes(9.0),
            index: 1,
          );
          expect(e3, isNotNull, reason: 'append a read-only entry');
          b = (await dyn.readDynamicBlocks())!.firstWhere(
            (x) => x.index == _scrDyn,
          );
          final ro = await dyn.readDynamicField(b, 1);
          expect(ro, isNotNull);
          expect(ro!.meta.readOnly, isTrue);
          expect(
            await dyn.writeDynamicField(b, ro, numberToBytes(10.0)),
            isNull,
            reason: 'a read-only entry rejects writes',
          );
          expect(
            await dyn.deleteDynamic(block: _scrDyn, field: 1),
            isFalse,
            reason: 'a read-only entry rejects deletes',
          );
        } finally {
          // Restore: drop the scratch block and compact.
          await dyn.deleteDynamic(block: _scrDyn);
          await dyn.saveAll();
        }
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
        try {
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();
          await reg.createDynamicBlock('CLAMP', index: _scrDyn);
          final b = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: dynamicTypeForIndex(_scrDyn), size: 1),
            name: 'CLAMP',
          );

          // Declare size 16 but send only 4 value bytes: the device must not read
          // past the frame.
          final payload = <int>[
            ...blockInfoBytes(
              dynamicTypeForIndex(_scrDyn),
              dynamicInstanceForIndex(_scrDyn),
              0,
              0,
            ),
            ...ValueInfo(
              type: DataType.number.value,
              key: 0,
              size: 16,
            ).toBytes(),
            ...numberToBytes(1.5),
          ];
          final reply = await reg.request(RegisterCid.write, payload: payload);
          expect(reply, isNotNull, reason: 'oversized write got a reply');

          final e = await reg.readDynamicField(b, 0);
          ctx.note('clamp entry size=${e?.meta.size} value=${e?.value}');
          expect(e, isNotNull);
          expect(
            e!.meta.size,
            4,
            reason: 'the oversized write is clamped to the payload',
          );
          expect(e.value.length, 4);
        } finally {
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();
        }
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
        try {
          // Clean slate for the scratch block only.
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();

          // RENDER at the scratch index: persistent (0,0)=42, volatile (0,1)=7.
          await reg.createDynamicBlock('RENDER', index: _scrDyn);
          final b = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: dynamicTypeForIndex(_scrDyn), size: 1),
            name: 'RENDER',
          );
          await reg.writeDynamicEntry(
            b,
            0,
            0,
            ValueInfo(
              type: DataType.number.value,
              flags: ValueFlags.persistent,
              key: 0,
            ),
            numberToBytes(42.0),
          );
          await reg.writeDynamicEntry(
            b,
            0,
            1,
            ValueInfo(type: DataType.number.value, key: 1),
            numberToBytes(7.0),
          );
          expect(await reg.saveAll(), isTrue);

          var files = await _fileNames(st);
          expect(
            files.contains(dtName) && files.contains(dvName),
            isTrue,
            reason: '$dtName/$dvName missing after Save All',
          );

          // The persistent value is in .DV_XX (read through the .DT_/.DV_ files).
          final dt = await st.readFile(dtName);
          final dv = await st.readFile(dvName);
          final table = dt == null ? null : decodeDynamicTable(dt);
          expect(table, isNotNull, reason: '$dtName decodes');
          expect(dv, isNotNull, reason: '$dvName reads');
          final backup = decodeDynamicValues(
            _scrDyn,
            table!,
            dv!,
          ).where((e) => e.field == 0 && e.key == 0).firstOrNull;
          expect(backup, isNotNull, reason: 'the persistent entry decodes');
          expect(numberFromBytes(backup!.value), closeTo(42.0, 0.01));

          // Delete + Save All removes the files; recreate at the same index works.
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();
          files = await _fileNames(st);
          expect(
            files.contains(dtName) || files.contains(dvName),
            isFalse,
            reason: 'tombstoned files are cleaned on Save All',
          );
          expect(
            await reg.createDynamicBlock('RENDER', index: _scrDyn),
            _scrDyn,
            reason: 'recreate at the scratch index',
          );
        } finally {
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();
        }
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
        try {
          await reg.deleteDynamic(block: _scrDyn);
          await reg.createDynamicBlock('DYNCHK', index: _scrDyn);
          final blk = DynBlock(
            index: _scrDyn,
            meta: ValueInfo(type: BlockType.dynamic.value, size: 1),
            name: 'DYNCHK',
          );
          await reg.writeDynamicEntry(
            blk,
            0,
            0,
            ValueInfo(
              type: DataType.number.value,
              flags: ValueFlags.persistent,
              size: 4,
            ),
            numberToBytes(1.0),
          );
          expect(await reg.saveAll(), isTrue);

          final names = (await st.readFileTable() ?? [])
              .map((r) => normalizeFileName(r.name))
              .toSet();
          expect(
            names.contains(dtName),
            isTrue,
            reason: '$dtName exists after Save All',
          );
          expect(
            names.contains(dvName),
            isTrue,
            reason: '$dvName exists after Save All',
          );

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
        } finally {
          await reg.deleteDynamic(block: _scrDyn);
          await reg.saveAll();
        }
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
        var done = false;
        try {
          await storage.deleteFile(name);
          expect(
            await storage.createFile(name, 250),
            isTrue,
            reason: 'device ${d.id} creates a file',
          );

          final data = List<int>.generate(250, (i) => i & 0xFF);
          expect(
            await storage.writeFile(name, data),
            isTrue,
            reason: 'device ${d.id} writes the file',
          );
          expect(
            await storage.readFile(name, size: 250),
            equals(data),
            reason: 'device ${d.id} reads the same bytes back',
          );
          expect(
            await storage.deleteFile(name),
            isTrue,
            reason: 'device ${d.id} deletes the file',
          );
          done = true;
        } finally {
          if (!done) await storage.deleteFile(name);
        }
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
        var done = false;
        try {
          await storage.deleteFile(name);
          expect(
            await storage.createFile(name, 16),
            isTrue,
            reason: 'node ${d.id} creates a 16-byte file',
          );
          final data = List<int>.generate(16, (i) => (i * 7) & 0xFF);
          expect(
            await storage.writeFile(name, data),
            isTrue,
            reason: 'node ${d.id} writes the file',
          );
          expect(
            await storage.readFile(name, size: 16),
            equals(data),
            reason: 'node ${d.id} reads the same bytes back',
          );
          expect(
            await storage.deleteFile(name),
            isTrue,
            reason: 'node ${d.id} deletes the file',
          );
          done = true;
        } finally {
          if (!done) await storage.deleteFile(name);
        }
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
        Set<int>? beforeN;
        var done = false;
        try {
          // Create is idempotently refused while the file exists.
          await storage.deleteFile('BKDUP');
          expect(await storage.createFile('BKDUP', 16), isTrue);
          expect(
            await storage.createFile('BKDUP', 16),
            isFalse,
            reason: 'creating an existing name must fail',
          );
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
          // so the deployed scenario's subscriptions are left untouched.
          final before = (await subs.getRequesterSubscriptions())
              .map((s) => s.trid)
              .toSet();
          beforeN = before;

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
            expect(
              counts.containsKey('.SUBREQ'),
              isFalse,
              reason: '.SUBREQ is removed when the requester table empties',
            );
          }

          // Hold one entry: .SUBREQ exists while the table is non-empty.
          await subs.setRequesterSubscription(0, entry: scratch());
          await Future<void>.delayed(const Duration(milliseconds: 300));
          counts = await fileCounts();
          expect(
            counts.containsKey('.SUBREQ'),
            isTrue,
            reason: '.SUBREQ exists while the requester table holds an entry',
          );

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
            expect(
              counts.containsKey('.SUBREQ'),
              isFalse,
              reason: '.SUBREQ is removed again once the table empties',
            );
          }
          expect(
            counts.containsKey('DYNMEM'),
            isFalse,
            reason: 'the obsolete DYNMEM file must be gone',
          );
          done = true;
        } finally {
          if (!done) {
            final base = beforeN;
            if (base != null) {
              final extra = (await subs.getRequesterSubscriptions())
                  .where((s) => !base.contains(s.trid))
                  .toList();
              for (final s in extra) {
                await subs.cancelRequesterSubscription(s.trid);
              }
            }
            await storage.deleteFile('BKDUP');
            await storage.deleteFile('RENAMED');
            await storage.deleteFile('TMPNAME');
          }
        }
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
        var done = false;
        try {
          // Clean scratch slot only (the deployed scenario owns slots 0-4).
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

          expect(
            await st.writeFile(name, image),
            isTrue,
            reason: 'upload $name',
          );
          expect(await c.load(slot, slot), isTrue, reason: 'load $name');
          expect(
            (await c.loadedScripts()).contains(slot),
            isTrue,
            reason: 'script $slot is listed',
          );

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
            blocks!.any(
              (b) => b != null && isScriptType(b.type) && b.inst == slot,
            ),
            isTrue,
            reason: 'script block listed by readBlocks()',
          );
          final inKeys = await reg.getBlockKeys(
            sType,
            sInst,
            ScriptField.input,
          );
          expect(inKeys, isNotNull);
          expect(inKeys!.length, 1, reason: 'script input keys');
          final inEntry = await reg.readBlockField(
            sType,
            sInst,
            ScriptField.input,
            0,
          );
          expect(inEntry, isNotNull);
          expect(inEntry!.meta.dataType, DataType.number);
          final outEntry = await reg.readBlockField(
            sType,
            sInst,
            ScriptField.output,
            0,
          );
          expect(outEntry, isNotNull);
          expect(
            outEntry!.meta.readOnly,
            isTrue,
            reason: 'script output is RO',
          );

          // State: a fresh load is Stopped; set/read round-trips.
          expect((await c.readState(slot))!.state, ScriptState.stopped);
          expect(await c.setState(slot, ScriptState.running), isTrue);
          expect((await c.readState(slot))!.state, ScriptState.running);
          expect(
            (await c.readState(slot))!.error,
            0,
            reason: 'setState clears the error code',
          );
          expect(
            (await c.readState(slot))!.hasError,
            isFalse,
            reason: 'a clean run reports no error',
          );

          // File metadata lives in SCR_XXX; the Register exposes only the I/O.
          final parsed = ScriptFileData.parse((await st.readFile(name))!);
          expect(parsed.properties, ScriptProperties.loadOnBoot);
          expect(parsed.inputs.length, 1);
          expect(parsed.variables.length, 2);
          expect(
            (numberFromBytes(parsed.constantValue(0)) - 9.0).abs() <= 0.001,
            isTrue,
            reason: 'constant value',
          );

          expect((await c.enumerateKeys(slot, ScriptField.input)).length, 1);
          expect(
            await c.enumerateKeys(slot, ScriptField.variable),
            isEmpty,
            reason: 'variables are not part of the register',
          );
          expect(
            await c.enumerateKeys(slot, ScriptField.constant),
            isEmpty,
            reason: 'constants are not part of the register',
          );

          final input0 = await c.readEntry(slot, ScriptField.input, 0);
          expect(input0, isNotNull);
          expect(
            (numberFromBytes(input0!.value) - 3.5).abs() <= 0.001,
            isTrue,
            reason: 'input default from the file',
          );
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
          expect(
            (numberFromBytes(internal2!.variables, 0) - 1.25).abs() <= 0.001,
            isTrue,
          );
          expect(
            (numberFromBytes(internal2.variables, 4) - 2.5).abs() <= 0.001,
            isTrue,
          );

          // Editor debug: stop, move to a line. The current line is the report record's Line
          // (CID 3); CID 5 carries the fixed-size call stack, all zero here (no frames).
          await c.setState(slot, ScriptState.stopped);
          expect(await c.moveToInstruction(slot, 1), isTrue);
          final moved = await c.readState(slot);
          expect(moved, isNotNull);
          expect(moved!.line, 1);
          final internal = await c.readInternalState(slot);
          expect(internal, isNotNull);
          expect(
            internal!.callStack.length,
            scriptMaxCallDepth,
            reason: 'the call stack is a fixed size',
          );
          expect(
            internal.callStack.every((frame) => frame == 0),
            isTrue,
            reason: 'a flat script has no call frames',
          );

          // Inputs are writable through Register.
          expect(
            await c.writeEntry(
              slot,
              ScriptField.input,
              0,
              input0.meta,
              numberToBytes(7.5),
            ),
            isTrue,
          );
          final input2 = await c.readEntry(slot, ScriptField.input, 0);
          expect(input2, isNotNull);
          expect((numberFromBytes(input2!.value) - 7.5).abs() <= 0.001, isTrue);

          // Unload clears the scratch slot.
          expect(await c.unload(slot), isTrue);
          expect(
            (await c.loadedScripts()).contains(slot),
            isFalse,
            reason: 'the unloaded slot is gone from the list',
          );

          await st.deleteFile(name);
          done = true;
        } finally {
          if (!done) await _removeScript(c, st, slot);
        }
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
        var done = false;
        try {
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);

          final image = ScriptFileBuilder(functionName: 'Empty').build();
          expect(
            await st.writeFile(name, image),
            isTrue,
            reason: 'create $name',
          );

          // Writing the file must NOT load it.
          expect(
            (await c.loadedScripts()).contains(slot),
            isFalse,
            reason: 'a new script is not auto-loaded',
          );

          expect(await c.load(slot, slot), isTrue, reason: 'load $name');
          final meta = await c.readBlockMeta(slot);
          expect(meta, isNotNull);
          expect(meta!.name, 'Empty');
          expect(meta.meta.size, ScriptField.count);

          expect(await c.unload(slot), isTrue);
          await st.deleteFile(name);
          done = true;
        } finally {
          if (!done) await _removeScript(c, st, slot);
        }
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
        var done = false;
        try {
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);

          // Build an edited draft through the same model the editor uses.
          final draft =
              ScriptDraft(
                  functionName: 'Edited',
                  properties: ScriptProperties.loadOnBoot,
                )
                ..inputs.add(
                  ScriptDraftValue(
                    name: 'Speed',
                    type: DataType.number,
                    size: 4,
                    value: numberToBytes(1.5),
                  ),
                )
                ..outputs.add(
                  ScriptDraftValue(
                    name: 'Result',
                    type: DataType.number,
                    size: 4,
                  ),
                )
                ..variables.add(
                  ScriptDraftValue(name: 'Tmp', type: DataType.number, size: 4),
                )
                ..constants.add(
                  ScriptDraftValue(
                    name: 'Gain',
                    type: DataType.number,
                    size: 4,
                    value: numberToBytes(2.0),
                  ),
                )
                ..lines.add(
                  ScriptLine(
                    destinations: [ScriptSymbol.variable(0)],
                    instruction: ScriptSymbol.instruction(catMath, 0),
                    operands: [ScriptSymbol.constant(0)],
                  ),
                )
                ..lines.add(
                  ScriptLine(
                    destinations: [ScriptSymbol.output(0)],
                    instruction: ScriptSymbol.instruction(catMath, 0),
                    operands: [ScriptSymbol.input(0)],
                  ),
                );

          expect(
            await st.writeFile(name, draft.toImage()),
            isTrue,
            reason: 'edit upload',
          );
          expect(
            (await c.loadedScripts()).contains(slot),
            isFalse,
            reason: 'an edited stored script is not auto-loaded',
          );

          expect(
            await c.load(slot, slot),
            isTrue,
            reason: 'load the edited script',
          );
          final meta = await c.readBlockMeta(slot);
          expect(meta, isNotNull);
          expect(meta!.name, 'Edited', reason: 'the edited name is restored');
          final parsed = ScriptFileData.parse((await st.readFile(name))!);
          expect(parsed.variables.length, 1);
          // Editor debug: re-load the edited script and read its variable RAM (CID 5).
          final internal = await c.readInternalState(slot);
          expect(internal, isNotNull);
          expect(internal!.variables.length, 4);
          final input = await c.readEntry(slot, ScriptField.input, 0);
          expect(input, isNotNull);
          expect(
            (numberFromBytes(input!.value) - 1.5).abs() <= 0.001,
            isTrue,
            reason: 'edited input default',
          );

          expect(await c.unload(slot), isTrue);
          await st.deleteFile(name);
          done = true;
        } finally {
          if (!done) await _removeScript(c, st, slot);
        }
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
        var done = false;
        try {
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);

          // One 16-byte String input.
          final draft = ScriptDraft(functionName: 'Pad')
            ..inputs.add(
              ScriptDraftValue(name: 'Name', type: DataType.text, size: 16),
            );
          expect(
            await st.writeFile(name, draft.toImage()),
            isTrue,
            reason: 'upload $name',
          );
          expect(await c.load(slot, slot), isTrue, reason: 'load $name');

          final entry = await c.readEntry(slot, ScriptField.input, 0);
          expect(entry, isNotNull);
          expect(entry!.meta.size, 16, reason: 'string input size');

          // A shorter String write declares its real length; the device must fill the
          // rest of the fixed-size input with NULs rather than leaving stale bytes.
          final meta = ValueInfo(
            type: entry.meta.type,
            flags: entry.meta.flags,
            key: 0,
            size: 3,
          );
          expect(
            await c.writeEntry(
              slot,
              ScriptField.input,
              0,
              meta,
              'abc'.codeUnits,
            ),
            isTrue,
            reason: 'short string write',
          );
          final back = await c.readEntry(slot, ScriptField.input, 0);
          expect(back, isNotNull);
          expect(
            back!.value.length,
            16,
            reason: 'string input size after short write',
          );
          expect(
            String.fromCharCodes(back.value),
            'abc${'\x00' * 13}',
            reason: 'the short string is null-padded',
          );

          expect(await c.unload(slot), isTrue);
          await st.deleteFile(name);
          done = true;
        } finally {
          if (!done) await _removeScript(c, st, slot);
        }
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
        try {
          // A = 2; B = A + A; Out = B; halt
          final draft = ScriptDraft(functionName: 'Arith')
            ..outputs.add(
              ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
            )
            ..variables.add(
              ScriptDraftValue(name: 'A', type: DataType.number, size: 4),
            )
            ..variables.add(
              ScriptDraftValue(name: 'B', type: DataType.number, size: 4),
            )
            ..constants.add(
              ScriptDraftValue(
                name: 'Two',
                type: DataType.number,
                size: 4,
                value: numberToBytes(2.0),
              ),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.variable(0)],
                instruction: ScriptSymbol.instruction(catMath, 0),
                operands: [ScriptSymbol.constant(0)],
              ),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.variable(1)],
                instruction: ScriptSymbol.instruction(catMath, 0),
                operands: [
                  ScriptSymbol.variable(0),
                  ScriptSymbol.predefine(preMathOp, 0),
                  ScriptSymbol.variable(0),
                ],
              ),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.output(0)],
                instruction: ScriptSymbol.instruction(catMath, 0),
                operands: [ScriptSymbol.variable(1)],
              ),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            );

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the arithmetic script',
          );
          await c.setState(slot, ScriptState.running);
          expect(
            await _waitState(c, slot, ScriptState.finished),
            ScriptState.finished,
          );

          final out = await c.readEntry(slot, ScriptField.output, 0);
          expect(out, isNotNull);
          expect(numberFromBytes(out!.value), closeTo(4.0, 0.001));

          // Variables live in the script RAM, not the Register.
          final ram = (await c.readInternalState(slot))!.variables;
          expect(numberFromBytes(ram, 0), closeTo(2.0, 0.001));
          expect(numberFromBytes(ram, 4), closeTo(4.0, 0.001));
        } finally {
          await _removeScript(c, st, slot);
        }
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
        try {
          // A=0; While A < 3 { A = A + 1 }; halt -> A == 3.
          final draft = ScriptDraft(functionName: 'Loop')
            ..variables.add(
              ScriptDraftValue(name: 'A', type: DataType.number, size: 4),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.variable(0)],
                instruction: ScriptSymbol.instruction(catMath, 0),
                operands: [ScriptSymbol.predefine(preIndex, 0)],
              ),
            )
            ..lines.add(
              ScriptLine(
                instruction: ScriptSymbol.instruction(catFlow, 1),
                operands: [
                  ScriptSymbol.variable(0),
                  ScriptSymbol.predefine(preMathOp, 14),
                  ScriptSymbol.predefine(preIndex, 3),
                ],
              ),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.variable(0)],
                instruction: ScriptSymbol.instruction(catMath, 0),
                operands: [
                  ScriptSymbol.variable(0),
                  ScriptSymbol.predefine(preMathOp, 0),
                  ScriptSymbol.predefine(preIndex, 1),
                ],
              ),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 2)),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            );

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the loop script',
          );
          await c.setState(slot, ScriptState.running);
          final state = await _waitState(c, slot, ScriptState.finished);
          expect(
            state,
            ScriptState.finished,
            reason: 'error code ${(await c.readState(slot))?.error}',
          );
          final ram = (await c.readInternalState(slot))!.variables;
          expect(numberFromBytes(ram, 0), closeTo(3.0, 0.001));
        } finally {
          await _removeScript(c, st, slot);
        }
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
        try {
          // Modulo(Out, Vec, 1): a Vector operand is not a scalar -> operand error.
          final draft = ScriptDraft(functionName: 'Bad')
            ..outputs.add(
              ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
            )
            ..variables.add(
              ScriptDraftValue(name: 'Vec', type: DataType.vector, size: 8),
            )
            ..constants.add(
              ScriptDraftValue(
                name: 'One',
                type: DataType.number,
                size: 4,
                value: numberToBytes(1),
              ),
            )
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.output(0)],
                instruction: ScriptSymbol.instruction(catMath, 5),
                operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(0)],
              ),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            );

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the faulty script',
          );
          await c.setState(slot, ScriptState.running);
          expect(
            await _waitState(c, slot, ScriptState.error),
            ScriptState.error,
          );
          // The report record names the offending instruction - the Modulo (catMath, op 5) - and
          // carries an instruction-specific error code; the code catalogue is undecided, so the
          // value itself is not pinned, only that a fault is reported.
          final report = await c.readState(slot);
          expect(report, isNotNull);
          expect(report!.hasError, isTrue, reason: 'a fault is reported');
          expect(
            report.error,
            isNot(0),
            reason: 'instruction-specific error code',
          );
          expect(
            ScriptReport.decodeInstruction(report.instruction),
            (catMath, 5),
            reason: 'the offending instruction is the Modulo',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
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
        try {
          final draft = ScriptDraft(functionName: 'Log')
            ..constants.add(
              ScriptDraftValue(
                name: 'Code',
                type: DataType.number,
                size: 4,
                value: numberToBytes(7),
              ),
            )
            ..lines.add(
              ScriptLine(
                instruction: ScriptSymbol.instruction(catService, 0),
                operands: [ScriptSymbol.constant(0)],
              ),
            ) // Log
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 4)),
            ) // Nop
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the log script',
          );
          await c.setState(slot, ScriptState.running);
          expect(
            await _waitState(c, slot, ScriptState.finished),
            ScriptState.finished,
          );
          expect(
            (await c.readState(slot))?.error,
            0,
            reason: 'the Log/Nop path does not fault',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
      }
    },
  ),

  HilTest(
    'script CID 0 slot map and CID 5 internal state round-trip through fragmentation',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 4;
        final fileId = slot; // _uploadScript loads with load(slot, slot).
        final emptySlot =
            _scrSlotBase + 5; // no deployed-scenario or scratch script here.
        final name = scriptFileName(fileId);
        try {
          // Clean the scratch slot only (the deployed scenario owns slots 0-4).
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);

          // Two 4-byte variables, so CID 5's variable RAM is a known 8 bytes, and a
          // Halt so the script is a valid image.
          final draft = ScriptDraft(functionName: 'Stream')
            ..variables.add(
              ScriptDraftValue(name: 'A', type: DataType.number, size: 4),
            )
            ..variables.add(
              ScriptDraftValue(name: 'B', type: DataType.number, size: 4),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the streaming script',
          );

          // ---- CID 0: the streamed slot map ------------------------------------
          // The whole map streams through the app interface's fragmentation (Docs:
          // CIDs 0 and 5 both exceed one frame); slotMap() decodes the reassembled
          // payload as one uint16 per slot, with the slot count taken from the stream
          // length rather than an assumed 256.
          final map = await c.slotMap();
          expect(
            map,
            isNotEmpty,
            reason: 'device ${d.id} streams its slot map',
          );
          ctx.note(
            '${d.label}: slot map has ${map.length} slot(s)'
            '${map.length > 58 ? ' (> 58, so CID 0 fragmented past one frame)' : ''}',
          );

          // The per-slot uint16 map: the loaded scratch slot shows its file id, and
          // the slot count is at least past both scratch slots (the stream length is
          // what bounds the map, so indexing must not run off it).
          expect(
            map.length,
            greaterThan(emptySlot),
            reason: 'the streamed map covers the scratch slots',
          );
          expect(map.length, lessThanOrEqualTo(maxScripts));
          expect(
            map[slot],
            fileId,
            reason: 'the loaded slot shows its file id',
          );
          expect(
            map[emptySlot],
            scriptSlotEmpty,
            reason: 'an empty slot reads 0xFFFF',
          );

          // loadedSlotMap() is the same stream read as `slot -> file id`.
          final occupied = await c.loadedSlotMap();
          expect(occupied[slot], fileId);
          expect(occupied.containsKey(emptySlot), isFalse);
          expect(occupied.length, lessThanOrEqualTo(map.length));

          // ---- CID 5: fixed-size call stack followed by the variable RAM -------
          final internal = await c.readInternalState(slot);
          expect(internal, isNotNull, reason: 'device ${d.id} answers CID 5');
          expect(
            internal!.callStack.length,
            scriptMaxCallDepth,
            reason: 'the instruction-counter stack is a fixed size',
          );
          expect(
            internal.callStack.every((frame) => frame == 0),
            isTrue,
            reason: 'a flat script has no call frames',
          );
          expect(
            internal.variables.length,
            8,
            reason: 'the two 4-byte variables follow the fixed-size stack',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
      }
    },
  ),

  HilTest(
    'script VM: Flow Call runs a local function body and Return resumes it',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 12;
        try {
          // The main body calls a local function once; the body writes a variable and an
          // output, then Returns. Assembly lays the body out after the main program and each
          // use site's Call becomes the body's absolute line index (Docs/Services/Script.md
          // "Functions and Macros"; the app lays local functions out, the device does not).
          final draft = ScriptDraft(functionName: 'CallRet')
            ..outputs.add(
              ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
            )
            ..variables.add(
              ScriptDraftValue(name: 'A', type: DataType.number, size: 4),
            )
            ..lines.add(ScriptLine.callTo(0))
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ) // Halt
            ..functions.add(
              ScriptFunction(
                name: 'Body',
                body: [
                  ScriptLine(
                    destinations: [ScriptSymbol.variable(0)],
                    instruction: ScriptSymbol.instruction(catMath, 0), // Set
                    operands: [ScriptSymbol.predefine(preIndex, 42)],
                  ),
                  ScriptLine(
                    destinations: [ScriptSymbol.output(0)],
                    instruction: ScriptSymbol.instruction(catMath, 0), // Set
                    operands: [ScriptSymbol.variable(0)],
                  ),
                  ScriptLine(
                    instruction: ScriptSymbol.instruction(catFlow, 5),
                  ), // Return
                ],
              ),
            );

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the Call/Return script',
          );
          await c.setState(slot, ScriptState.running);
          final state = await _waitState(c, slot, ScriptState.finished);
          expect(
            state,
            ScriptState.finished,
            reason: 'error code ${(await c.readState(slot))?.error}',
          );

          // The body actually ran: its variable (script RAM) and the output it copied both carry
          // the value only the body wrote.
          final ram = (await c.readInternalState(slot))!.variables;
          expect(
            numberFromBytes(ram, 0),
            closeTo(42.0, 0.001),
            reason: 'the called body wrote its variable',
          );
          final out = await c.readEntry(slot, ScriptField.output, 0);
          expect(out, isNotNull);
          expect(
            numberFromBytes(out!.value),
            closeTo(42.0, 0.001),
            reason: 'the body set the output',
          );
          // Return restored the caller: the call depth is back to zero.
          expect(
            (await c.readState(slot))!.depth,
            0,
            reason: 'Return popped the call frame',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
      }
    },
  ),

  HilTest(
    'script Service Report info (14) logs and carries on to Finished',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 13;
        try {
          // Report info is a log-only action (Docs/Services/Script.md Service op 14): it must not
          // halt or fault. A Set AFTER it proves execution carried past it.
          final draft = ScriptDraft(functionName: 'ReportInfo')
            ..variables.add(
              ScriptDraftValue(name: 'A', type: DataType.number, size: 4),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 14)),
            ) // Report info
            ..lines.add(
              ScriptLine(
                destinations: [ScriptSymbol.variable(0)],
                instruction: ScriptSymbol.instruction(catMath, 0), // Set
                operands: [ScriptSymbol.predefine(preIndex, 5)],
              ),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the Report info script',
          );
          await c.setState(slot, ScriptState.running);
          expect(
            await _waitState(c, slot, ScriptState.finished),
            ScriptState.finished,
          );
          expect(
            (await c.readState(slot))?.error,
            0,
            reason: 'Report info does not fault',
          );
          final ram = (await c.readInternalState(slot))!.variables;
          expect(
            numberFromBytes(ram, 0),
            closeTo(5.0, 0.001),
            reason: 'execution carried on past Report info',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
      }
    },
  ),

  HilTest(
    'script Service Error (13) throws: state Error with the script-defined code',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 14;
        try {
          // Error is a custom throw (Docs/Services/Script.md Service op 13): it logs and halts,
          // carrying a script-defined code. The code is the script's own operand value - NOT a
          // catalogue code (that catalogue is undecided), so the test uses a distinctive literal.
          const code = 0x1234;
          final draft = ScriptDraft(functionName: 'Throw')
            ..lines.add(
              ScriptLine(
                instruction: ScriptSymbol.instruction(catService, 13), // Error
                operands: [ScriptSymbol.predefine(preIndex, code)],
              ),
            )
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt (never reached)

          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'upload + load the Error script',
          );
          await c.setState(slot, ScriptState.running);
          expect(
            await _waitState(c, slot, ScriptState.error),
            ScriptState.error,
          );

          final report = await c.readState(slot);
          expect(report, isNotNull);
          expect(report!.hasError, isTrue, reason: 'a throw is reported');
          expect(
            report.error,
            code,
            reason: 'the report carries the script-defined code',
          );
          expect(
            ScriptReport.decodeInstruction(report.instruction),
            (catService, 13),
            reason: 'the offending instruction is the Service Error op',
          );
        } finally {
          await _removeScript(c, st, slot);
        }
      }
    },
  ),

  HilTest(
    'script Service state verbs (Stop/Pause/Resume) do not fault',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      // Start (9) is deliberately absent: entered while Running (the only state the VM runs in),
      // it neither advances the counter nor resets it, so the loop guard re-runs it every tick
      // and the script never leaves Running - testing it would wedge the VM. Stop/Pause/Resume
      // all advance or terminate.
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        var done = false;
        try {
          // Stop (10): ends Stopped.
          final stopSlot = _scrSlotBase + 15;
          final stopDraft = ScriptDraft(functionName: 'StopVerb')
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 10)),
            ); // Stop
          expect(
            await _uploadScript(c, st, stopSlot, stopDraft),
            isTrue,
            reason: 'upload + load the Stop script',
          );
          await c.setState(stopSlot, ScriptState.running);
          expect(
            await _waitState(c, stopSlot, ScriptState.stopped),
            ScriptState.stopped,
          );
          expect(
            (await c.readState(stopSlot))?.error,
            0,
            reason: 'Stop does not fault',
          );
          await _removeScript(c, st, stopSlot);

          // Pause (11): ends Paused and resumes past the Pause on the next start.
          final pauseSlot = _scrSlotBase + 16;
          final pauseDraft = ScriptDraft(functionName: 'PauseVerb')
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 11)),
            ) // Pause
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt
          expect(
            await _uploadScript(c, st, pauseSlot, pauseDraft),
            isTrue,
            reason: 'upload + load the Pause script',
          );
          await c.setState(pauseSlot, ScriptState.running);
          expect(
            await _waitState(c, pauseSlot, ScriptState.paused),
            ScriptState.paused,
          );
          expect(
            (await c.readState(pauseSlot))?.error,
            0,
            reason: 'Pause does not fault',
          );
          await _removeScript(c, st, pauseSlot);

          // Resume (12): a no-op on a running script, so execution reaches the Halt.
          final resumeSlot = _scrSlotBase + 17;
          final resumeDraft = ScriptDraft(functionName: 'ResumeVerb')
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catService, 12)),
            ) // Resume
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt
          expect(
            await _uploadScript(c, st, resumeSlot, resumeDraft),
            isTrue,
            reason: 'upload + load the Resume script',
          );
          await c.setState(resumeSlot, ScriptState.running);
          expect(
            await _waitState(c, resumeSlot, ScriptState.finished),
            ScriptState.finished,
          );
          expect(
            (await c.readState(resumeSlot))?.error,
            0,
            reason: 'Resume does not fault',
          );
          await _removeScript(c, st, resumeSlot);
          done = true;
        } finally {
          if (!done) {
            await _removeScript(c, st, _scrSlotBase + 15);
            await _removeScript(c, st, _scrSlotBase + 16);
            await _removeScript(c, st, _scrSlotBase + 17);
          }
        }
      }
    },
  ),

  HilTest(
    'script load-failed: a missing function library refuses the load and holds the slot',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot = _scrSlotBase + 18;
        final name = scriptFileName(slot);
        try {
          // Clean the scratch slot and guarantee the referenced library is absent.
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);
          const missingLib =
              0xFFF; // a function-library file id that is not present
          await st.deleteFile(scriptFileName(missingLib));

          // A script linking one global function from an absent library: assembly keeps the
          // reservation, so the load reaches the link step. A referenced function that is absent
          // is a load error, detected before anything is allocated (Docs/Services/Script.md
          // "Load Contract"/"State": the slot holds only its slot record, state Load-failed 7).
          final draft = ScriptDraft(functionName: 'LinkFail')
            ..lines.add(ScriptLine.callTo(0))
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ) // Halt
            ..functions.add(
              ScriptFunction(
                name: 'Missing',
                kind: ScriptFunctionKind.global,
                globalFileId: missingLib,
              ),
            );
          expect(
            await st.writeFile(name, draft.toImage()),
            isTrue,
            reason: 'upload $name',
          );

          // CID 1 refuses the load (the library is absent) ...
          expect(
            await c.load(slot, slot),
            isFalse,
            reason: 'a script linking an absent library is refused',
          );
          // ... but the slot keeps its reservation and reports the Load-failed state via CID 3.
          final report = await c.readState(slot);
          expect(
            report,
            isNotNull,
            reason: 'a load-failed slot still answers CID 3',
          );
          expect(
            report!.state,
            ScriptState.loadFailed,
            reason: 'the slot reports state 7 (Load failed)',
          );
          // The slot appears in the CID 0 slot map as a reservation (its file id), not 0xFFFF.
          final map = await c.slotMap();
          expect(map.length, greaterThan(slot));
          expect(
            map[slot],
            slot,
            reason: 'the load-failed slot shows its file id in the slot map',
          );
          // The reservation is honoured: the occupied slot refuses a retry load.
          expect(
            await c.load(slot, slot),
            isFalse,
            reason: 'a load-failed slot counts as occupied',
          );
          // An explicit unload clears the reservation.
          expect(
            await c.unload(slot),
            isTrue,
            reason: 'unload clears the load-failed reservation',
          );
          expect(
            await c.readState(slot),
            isNull,
            reason: 'the slot is no longer reserved',
          );
          final cleared = await c.slotMap();
          expect(
            cleared[slot],
            scriptSlotEmpty,
            reason: 'the slot is empty again after the explicit unload',
          );
        } finally {
          await st.deleteFile(name);
        }
      }
    },
  ),

  HilTest(
    'script global function links at load and runs: the linked body executes and finishes',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        final slot =
            _scrSlotBase +
            19; // clear of the deployed scenario (0-4) and the others.
        final name = scriptFileName(slot);
        final libName = scriptFileName(_scrLibFileId);
        try {
          // Clean the scratch slot and the scratch library so a stale file cannot mask the link.
          if (await c.readState(slot) != null) await c.unload(slot);
          await st.deleteFile(name);
          await st.deleteFile(libName);

          // The function library: the script file format with the Function-library Properties flag.
          // Its instruction space IS the linked body (one body, so its offset is 0 for a
          // single-body library, Docs/Services/Script.md "Functions and Macros"). The body writes
          // its own variable (the parameter channel) and copies it out, then Returns; its variable
          // is relocated into the script's variable RAM at load.
          final libImage = ScriptFileBuilder(
            properties: ScriptProperties.functionLibrary,
            variables: [ValueInfo(type: DataType.number.value, size: 4)],
            instructions: encodeScriptLines([
              ScriptLine(
                destinations: [ScriptSymbol.variable(0)],
                instruction: ScriptSymbol.instruction(catMath, 0), // Set
                operands: [ScriptSymbol.predefine(preIndex, 42)],
              ),
              ScriptLine(
                destinations: [ScriptSymbol.output(0)],
                instruction: ScriptSymbol.instruction(catMath, 0), // Set
                operands: [ScriptSymbol.variable(0)],
              ),
              ScriptLine(
                instruction: ScriptSymbol.instruction(catFlow, 5),
              ), // Return
            ]),
            functionName: 'LibFn',
          ).build();

          // The script declares the global function as a reservation (file id + the body's two
          // offsets) and calls it; the Call operand carries the reservation index with bit 15 set
          // (the app's global-call operand). Assembly keeps the reservation, so the load reaches
          // the link step; a resolvable link places the body and patches the Call.
          final draft = ScriptDraft(functionName: 'GlobalLink')
            ..outputs.add(
              ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
            )
            ..lines.add(ScriptLine.callTo(0))
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ) // Halt
            ..functions.add(
              ScriptFunction(
                name: 'LibFn',
                kind: ScriptFunctionKind.global,
                globalFileId: _scrLibFileId,
                globalMemOffset: 0,
                globalInstrOffset: 0,
              ),
            );

          // The library must be present BEFORE the load: the link resolves at load.
          expect(
            await st.writeFile(libName, libImage),
            isTrue,
            reason: 'upload the function library $libName',
          );
          expect(
            await _uploadScript(c, st, slot, draft),
            isTrue,
            reason: 'the positive load resolves the link',
          );

          // The positive load must NOT report the load-failed state (the contrast with the
          // missing-library test): a resolvable link loads normally into Stopped.
          final loaded = await c.readState(slot);
          expect(loaded, isNotNull, reason: 'a linked script answers CID 3');
          expect(
            loaded!.state,
            isNot(ScriptState.loadFailed),
            reason: 'a resolvable link does not enter Load-failed',
          );
          expect(
            loaded.state,
            ScriptState.stopped,
            reason: 'the linked script loaded into Stopped',
          );

          // Run it: the caller's Call jumps into the linked body and Return resumes the caller.
          await c.setState(slot, ScriptState.running);
          final state = await _waitState(c, slot, ScriptState.finished);
          expect(
            state,
            ScriptState.finished,
            reason: 'error code ${(await c.readState(slot))?.error}',
          );
          final report = await c.readState(slot);
          expect(report!.error, 0, reason: 'the linked run does not fault');
          expect(report.depth, 0, reason: 'Return popped the linked body');

          // The linked body actually executed: its variable (relocated into the script variable
          // RAM) and the output it copied both carry the value only the body wrote.
          final ram = (await c.readInternalState(slot))!.variables;
          expect(
            numberFromBytes(ram, 0),
            closeTo(42.0, 0.001),
            reason: 'the linked body wrote its variable',
          );
          final out = await c.readEntry(slot, ScriptField.output, 0);
          expect(out, isNotNull);
          expect(
            numberFromBytes(out!.value),
            closeTo(42.0, 0.001),
            reason: 'the linked body set the output',
          );
        } finally {
          // Clean up both files and the slot.
          await _removeScript(c, st, slot);
          await st.deleteFile(libName);
        }
      }
    },
  ),

  HilTest(
    'script CID 1 loads into the lowest free slot when no slot is named',
    category: HilCategory.service,
    prereq: _script,
    body: (ctx) async {
      for (final d in ctx.devices(_script)) {
        final c = ScriptClient(deviceId: d.id);
        final st = StorageClient(deviceId: d.id);
        const fId =
            0x2AB; // a scratch script file id, distinct from any slot-named image
        final name = scriptFileName(fId);
        int? slotToRelease;
        var done = false;
        try {
          await st.deleteFile(name);

          // A minimal valid script image (a single Halt).
          final draft = ScriptDraft(functionName: 'LowFree')
            ..lines.add(
              ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
            ); // Halt
          expect(
            await st.writeFile(name, draft.toImage()),
            isTrue,
            reason: 'upload $name',
          );

          // The lowest slot that is currently free (Docs/Services/Script.md CID 1: "a load that
          // names no slot takes the lowest free one").
          final before = await c.slotMap();
          expect(before, isNotEmpty, reason: 'the device streams its slot map');
          final expected = before.indexOf(scriptSlotEmpty);
          expect(
            expected,
            isNot(-1),
            reason: 'the rig has at least one free slot',
          );

          // Load with the file id alone (no slot): the device picks the slot.
          expect(
            await c.loadLowestFree(fId),
            isTrue,
            reason: 'the lowest-free load succeeds',
          );
          slotToRelease = expected;
          final after = await c.slotMap();
          expect(after.length, before.length);
          expect(
            after[expected],
            fId,
            reason: 'the device chose the lowest free slot ($expected)',
          );
          // Only that slot changed: exactly one more occupied slot than before.
          expect(
            after.where((id) => id != scriptSlotEmpty).length,
            before.where((id) => id != scriptSlotEmpty).length + 1,
            reason: 'exactly one slot was taken',
          );

          // Restore.
          expect(await c.unload(expected), isTrue);
          await st.deleteFile(name);
          final restored = await c.slotMap();
          expect(
            restored[expected],
            scriptSlotEmpty,
            reason: 'the scratch slot is released',
          );
          done = true;
        } finally {
          if (!done) {
            if (slotToRelease != null) await c.unload(slotToRelease);
            await st.deleteFile(name);
          }
        }
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
        Set<int>? beforeN;
        var done = false;
        try {
          // Snapshot the setup's table so the scratch entry can be removed without
          // disturbing it.
          final before = (await subs.getRequesterSubscriptions())
              .map((s) => s.trid)
              .toSet();
          beforeN = before;

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
              ),
            ),
            isTrue,
            reason: 'set a requester subscription',
          );

          final added = (await subs.getRequesterSubscriptions())
              .where((s) => !before.contains(s.trid))
              .toList();
          expect(
            added,
            hasLength(1),
            reason: 'the scratch entry appears in the requester table',
          );
          final s = added.single;
          expect(s.trigger, TriggerType.periodic);
          expect(s.periodMs, 500);
          expect(s.minTimeMs, 100);
          expect(
            s.blockFieldS,
            6,
            reason: 'the source register BlockInfo round-trips',
          );

          // The table is persisted into the .SUBREQ file.
          final names = await _fileNames(storage);
          expect(
            names.contains('.SUBREQ'),
            isTrue,
            reason: 'the requester table file exists',
          );

          // Restore: cancel only the scratch entry.
          expect(
            await subs.cancelRequesterSubscription(s.trid),
            isTrue,
            reason: 'cancel the scratch entry',
          );
          final restored = (await subs.getRequesterSubscriptions())
              .map((x) => x.trid)
              .toSet();
          expect(restored, before, reason: 'the requester table is restored');
          done = true;
        } finally {
          if (!done) {
            final base = beforeN;
            if (base != null) {
              final extra = (await subs.getRequesterSubscriptions())
                  .where((s) => !base.contains(s.trid))
                  .toList();
              for (final s in extra) {
                await subs.cancelRequesterSubscription(s.trid);
              }
            }
          }
        }
      }
    },
  ),
];
