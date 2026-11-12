/// The device a script restore talks to, shared by the three suites that drive one.
///
/// `backup_restore_bringup_test`, `backup_restore_script_slots_test` and
/// `backup_restore_script_files_test` each carried their own `_FakeCore extends FakeBus` - the same
/// slot map, the same load/unload commands, the same write fragments and the same rename, written
/// three times and drifted apart in depth: the bring-up fake stopped at the slot map, the slots one
/// added the per-slot state read and a refused unload, the files one added the file table and its
/// create/delete. A drift there is not cosmetic: two suites would then model two different devices,
/// and a restore test's "the device holds X afterwards" claim would depend on which copy it got.
///
/// So there is ONE device model here, and the three suites switch on the parts each of them
/// measures with the flags below. Every flag defaults to the shallowest behaviour, which is what
/// the bring-up suite drives, so a suite that turns nothing on behaves exactly as its own copy did.
library;

import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

import 'fake_bus.dart';
import 'fake_core.dart';

/// A core that answers the service requests a restore sends, without a rig.
///
/// It models the slot map, the load/unload/action commands, the storage write fragments and the
/// rename - the things every restore run does - plus, behind flags, the three deeper things the
/// script suites measure: the per-slot state read, the file table (create/delete) and a file read.
class RestoreCore extends FakeBus {
  RestoreCore({
    Map<int, int> loaded = const {},
    Map<String, int> table = const {},
    this.answersScriptState = false,
    this.modelsFileTable = false,
    this.answersFileReads = false,
    this.throwOnUnknown = false,
  }) : loaded = {...loaded},
       table = {...table};

  /// loaded slot -> SCR file id. The load/unload commands mutate it, so a test can read the device
  /// state the restore left behind.
  final Map<int, int> loaded;

  /// The bytes of each stored file, keyed by the name the writes landed them under: what a read
  /// returns, and (when [modelsFileTable] is off) what the `.TABLE` reply lists.
  final Map<String, List<int>> files = {};

  /// The device's file table: name -> recorded size. Only maintained when [modelsFileTable] is on;
  /// a create adds a name, a delete removes it and a rename moves it.
  final Map<String, int> table;

  /// Answer the per-slot state read (Script CID 3) with a real state record.
  ///
  /// Off means the read is answered with nothing at all - a core that does not say what a slot
  /// runs, which is what the bring-up suite's device did (its reload falls back to the archive's
  /// `Run on load`).
  final bool answersScriptState;

  /// Model storage create (CID 1) and delete (CID 2) against [table], and answer a `.TABLE` read
  /// from it. Off means those commands are accepted without changing anything, and `.TABLE` lists
  /// the files this run staged.
  final bool modelsFileTable;

  /// Answer a whole-file read (Storage CID 5) with the stored bytes, per-fragment echo included.
  /// Off means every file read but `.TABLE` fails, as a device with no readable files does.
  final bool answersFileReads;

  /// Throw for a request the flags do not cover, instead of answering an empty payload - the file
  /// suite's device, where an unexpected request should surface rather than look like an answer.
  final bool throwOnUnknown;

  /// Slots the device refuses to unload (it answers FAIL), as a stuck script would.
  final refuseUnload = <int>{};

  /// Script file ids the device refuses to load, as a damaged file would.
  final refuseLoad = <int>{};

  /// Names the device refuses to delete (it answers FAIL), as a stuck record would.
  final refuseDelete = <String>{};

  /// The state each loaded slot reports (Docs CID 3). Absent means `Running`, which is what a rig
  /// left by an earlier setup reports.
  final states = <int, int>{};

  /// Never answer the slot-map read (CID 0), as a timeout does.
  bool slotMapTimesOut = false;

  /// Never answer the report record (CID 3), as a device that cannot say what it runs.
  bool statesUnreadable = false;

  /// Fail the file-table read, as a device that cannot say what it stores.
  bool tableUnreadable = false;

  @override
  Future<List<int>> plain(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async {
    if (service == ServiceType.script && functionCid == 0) {
      if (slotMapTimesOut) throw StateError('no reply');
      return scriptSlotMap(loaded);
    }
    if (service == ServiceType.script && functionCid == 3) {
      if (statesUnreadable) throw StateError('no reply');
      if (!answersScriptState) {
        if (throwOnUnknown) {
          throw StateError('$service/$functionCid not answered');
        }
        return const [];
      }
      final state = states[payload[0]] ?? ScriptState.running;
      return [state, 0, 0, 0]; // state, depth, line (u16)
    }
    if (service == ServiceType.storage && functionCid == 6) {
      // A write fragment: frag info (4), the file name (8), then its slice of the content.
      final name = StorageClient.unpadName(payload.sublist(4, 12));
      files.putIfAbsent(name, () => []).addAll(payload.sublist(12));
      if (modelsFileTable) table[name] = files[name]!.length;
      return const [0, 0]; // the sequence the device accepted
    }
    if (service == ServiceType.storage && functionCid == 5) {
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      if (name == '.TABLE') {
        if (tableUnreadable) throw StateError('no reply');
        final names = (modelsFileTable ? table.keys : files.keys).toList()
          ..sort();
        return tableReply(
          names,
          (name) => modelsFileTable ? table[name]! : files[name]!.length,
        );
      }
      if (!answersFileReads) throw StateError('no such file');
      // Answer it the way the device does - a per-fragment name echo - or FAIL it when no such
      // file exists, which is what "the device would not return it" looks like. A name the table
      // records but this run never wrote reads as its recorded length of zeroes.
      final content =
          files[name] ??
          (modelsFileTable ? List<int>.filled(table[name] ?? 0, 0) : null);
      if (content == null) throw StateError('no such file');
      final out = <int>[];
      for (var off = 0; off < content.length; off += 64) {
        final end = off + 64 > content.length ? content.length : off + 64;
        out
          ..addAll(StorageClient.padName(name))
          ..addAll(content.sublist(off, end));
      }
      return out;
    }
    if (throwOnUnknown) {
      throw StateError('$service/$functionCid not answered');
    }
    return const [];
  }

  @override
  Future<PacketResponse> flagged(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async {
    if (service == ServiceType.script) {
      switch (functionCid) {
        case 1: // load: file id (u16), slot (u8)
          final fileId = payload[0] | (payload[1] << 8);
          if (refuseLoad.contains(fileId)) return busRefused;
          final slot = payload.length > 2 ? payload[2] : -1;
          if (slot >= 0) loaded[slot] = fileId;
        case 2: // unload: slot (u8)
          if (refuseUnload.contains(payload[0])) return busRefused;
          loaded.remove(payload[0]);
      }
    }
    if (service == ServiceType.storage) {
      switch (functionCid) {
        case 1: // create: name (8), size (u32)
          if (modelsFileTable) {
            final name = StorageClient.unpadName(payload.sublist(0, 8));
            table[name] = uint32FromBytes(payload, 8);
            files[name] = [];
          }
        case 2: // delete: name (8)
          if (modelsFileTable) {
            final name = StorageClient.unpadName(payload.sublist(0, 8));
            if (refuseDelete.contains(name)) return busRefused;
            table.remove(name);
            files.remove(name);
          }
        case 4: // rename: old (8), new (8)
          // A rename lands the staged content under the new name.
          final from = StorageClient.unpadName(payload.sublist(0, 8));
          final to = StorageClient.unpadName(payload.sublist(8, 16));
          final size = table.remove(from);
          final staged = files.remove(from);
          if (modelsFileTable && size != null) table[to] = size;
          if (staged != null) files[to] = staged;
      }
    }
    return busAccepted;
  }

  /// The script commands the restore sent (`cid` followed by its payload), in order.
  List<List<int>> get scriptCommands => [
    for (final c in sent)
      if (c.service == ServiceType.script && c.cid != 0) [c.cid, ...c.payload],
  ];

  /// The slots the restore asked to UNLOAD, in order.
  List<int> get unloads => [
    for (final c in scriptCommands)
      if (c.first == 2) c[1],
  ];

  /// The slots the restore asked to LOAD, in order.
  List<int> get loads => [
    for (final c in scriptCommands)
      if (c.first == 1) c[3],
  ];

  /// The `(slot, action)` pairs the restore asked for, in order (CID 4).
  List<(int, int)> get actions => [
    for (final c in scriptCommands)
      if (c.first == 4) (c[1], c[2]),
  ];

  /// The `SCR_XXX` files the restore asked the device to DELETE, in order. The staging name a
  /// write uses (`~SCR_00`) is not one of them.
  List<String> get scriptDeletes => [
    for (final c in sent)
      if (c.service == ServiceType.storage && c.cid == 2)
        if (scriptFileId(StorageClient.unpadName(c.payload.sublist(0, 8))) !=
            null)
          StorageClient.unpadName(c.payload.sublist(0, 8)),
  ];
}
