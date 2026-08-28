import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/dynmem.dart';
import 'package:tamuapp/core/keyedmem.dart';
import 'package:tamuapp/core/types.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  test('storage probe', () async {
    if (Platform.environment['TAMU_HIL'] == null) return;
    final mgr = ConnectionManager.instance;
    await mgr.setAutoRefresh(false);
    final port = Platform.environment['TAMU_HIL']!;
    final err = await mgr.connectTo(
        DiscoveredLink(id: port, type: LinkType.usb, name: 'Tamu'));
    if (err != null) fail('connect failed: $err');
    addTearDown(mgr.disconnect);
    final db = DeviceDatabase.instance;
    var up = false;
    for (var i = 0; i < 10 && !up; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 300));
      up = await db.pingCore();
    }
    expect(up, isTrue, reason: 'no ping after connect');

    final st = StorageClient(deviceId: 1);
    final files = await st.readFileTable();
    // ignore: avoid_print
    print('[S] table: ${files?.map((f) => '${f.index}:${f.name}@${f.offset}(${f.size}B)').toList()}');
    if (files == null || files.isEmpty) {
      // ignore: avoid_print
      print('[S] no table / read failed');
      return;
    }
    final tableRec = files.first;
    // ignore: avoid_print
    print('[S] table record: name=${tableRec.name} isFiletable=${tableRec.isFiletable}');
    final data = await st.readFile(tableRec.name, size: tableRec.size);
    // ignore: avoid_print
    print('[S] readFile(.TABLE) bytes=${data?.length}');
    final data64 = await st.readFile(tableRec.name);
    // ignore: avoid_print
    print('[S] readFile(.TABLE) maxBytes=64 -> ${data64?.length}');
    // whole-file read (offset/bytes no longer part of the request)
    final data64o = await st.readFile(tableRec.name);
    // ignore: avoid_print
    print('[S] readFile(.TABLE) off=64 max64 -> ${data64o?.length}');
    if (data != null) {
      final first = data.take(16).toList();
      // ignore: avoid_print
      print('[S] first record: off=${uint32FromBytes(first, 0)} '
          'size=${uint32FromBytes(first, 4)} name=${String.fromCharCodes(first.sublist(8))}');
    }
    final snreg = files.where((f) => f.name == 'SNREG').firstOrNull;
    if (snreg != null) {
      final d2 = await st.readFile(snreg.name, size: snreg.size);
      // ignore: avoid_print
      print('[S] readFile(SNREG) bytes=${d2?.length}');
      if (d2 != null) {
        // decode first valid SNREG entries (14B SN + u16 id), skip 0xFF slots
        final recs = <String>[];
        for (var off = 0; off + 16 <= d2.length; off += 16) {
          final id = d2[off + 14] | (d2[off + 15] << 8);
          final allFf = d2.sublist(off, off + 16).every((b) => b == 0xFF);
          if (allFf) break;
          final sn = uint32FromBytes(d2, off).toRadixString(16).padLeft(8, '0');
          recs.add('id=$id sn0x=$sn');
        }
        // ignore: avoid_print
        print('[S] SNREG decoded entries: ${recs.take(4).toList()}');
      }
    }
    // decode the memory backup files (registry serialisation format)
    int u16(List<int> b, int o) => b[o] | (b[o + 1] << 8);
    for (final backupName in ['SYSMEM', 'DYNMEM', 'KEYMEM']) {
      final rec = files.where((f) => f.name == backupName).firstOrNull;
      if (rec == null) continue;
      final d = await st.readFile(rec.name, size: rec.size);
      if (d == null || d.length < 4) {
        // ignore: avoid_print
        print('[S] $backupName: read ${d?.length} bytes (unexpected)');
        continue;
      }
      final blockCount = u16(d, 0);
      if (backupName == 'SYSMEM') {
        var c = 2;
        final names = <String>[];
        for (var w = 0; w < blockCount && c + 4 <= d.length; w++) {
          final bIdx = u16(d, c);
          final fCount = u16(d, c + 2);
          c += 4;
          var valid = true;
          for (var f = 0; f < fCount && valid; f++) {
            if (c + 4 > d.length) { valid = false; break; }
            final vlen = u16(d, c + 2);
            c += 4;
            if (c + vlen > d.length) { valid = false; break; }
            c += vlen;
          }
          names.add('block$bIdx($fCount fields)${valid ? "" : " CORRUPT@"}');
        }
        // ignore: avoid_print
        print('[S] SYSMEM blocks: $names');
      } else {
        var c = 2;
        final names = <String>[];
        for (var i = 0; i < blockCount; i++) {
          final nameLen = d[c++];
          final name = String.fromCharCodes(d.sublist(c, c + nameLen));
          c += nameLen;
          final type = u16(d, c);
          final mapCount = u16(d, c + 2);
          c += 4 + mapCount * 4;
          final dataLen = u16(d, c);
          c += 2 + dataLen;
          names.add('$name(type=$type, $mapCount entries, $dataLen B)');
        }
        // ignore: avoid_print
        print('[S] $backupName blocks: $names');
      }
    }
    // populate dynamic + keyed memory so their backup files carry data, then
    // decode the backups
    final dyn = DynamicMemoryClient(deviceId: 1);
    for (final b in await dyn.readBlocks() ?? <DynBlock>[]) {
      await dyn.delete(block: b.index);
    }
    await dyn.createBlock(BlockType.undefined, 'DBAK');
    var dblock = (await dyn.readBlocks())!.first;
    await dyn.appendEntry(dblock,
        BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(2.5));
    dblock = (await dyn.readBlocks())!.first;
    await dyn.readBlockMeta(dblock.index);
    await dyn.appendEntry(dblock,
        BlockMeta(flagsAndType: DataType.string.value, size: 2), 'hi'.codeUnits);
    await dyn.save();
    final keyed = KeyedMemoryClient(deviceId: 1);
    for (final b in await keyed.readBlocks() ?? <KeyedBlock>[]) {
      await keyed.delete(block: b.index);
    }
    await keyed.createBlock(BlockType.undefined, 'KBAK');
    var kblock = (await keyed.readBlocks())!.first;
    await keyed.appendDict(kblock);
    await keyed.appendDict(kblock);
    kblock = (await keyed.readBlocks())!.first;
    await keyed.writeKeyValue(
        kblock, 0, 5, BlockMeta(flagsAndType: DataType.bool_.value, key: 5), [1]);
    await keyed.writeKeyValue(
        kblock, 1, 9, BlockMeta(flagsAndType: DataType.number.value, key: 9),
        numberToBytes(3.75));
    await keyed.save();
    // re-read the tables so offsets/sizes are fresh
    final freshFiles = await st.readFileTable();
    // ignore: avoid_print
    print('[S] files after backup: '
        '${freshFiles?.map((f) => '${f.name}(${f.size}B)').toList()}');
    for (final backupName in ['DYNMEM', 'KEYMEM']) {
      final rec = freshFiles?.where((f) => f.name == backupName).firstOrNull;
      if (rec == null) continue;
      final d = await st.readFile(rec.name, size: rec.size);
      if (d == null || d.length < 4) continue;
      final bc = u16(d, 0);
      var c = 2;
      final names = <String>[];
      for (var i = 0; i < bc && c < d.length; i++) {
        final nameLen = d[c++];
        if (c + nameLen > d.length) break;
        final name = String.fromCharCodes(d.sublist(c, c + nameLen));
        c += nameLen;
        if (c + 6 > d.length) break;
        final type = u16(d, c);
        final mapCount = u16(d, c + 2);
        c += 4 + mapCount * 4;
        if (c + 2 > d.length) break;
        final dataLen = u16(d, c);
        c += 2 + dataLen;
        names.add('$name(type=$type, $mapCount entries, $dataLen B)');
      }
      // ignore: avoid_print
      print('[S] $backupName blocks: $names');
    }
    // cleanup
    for (final b in await dyn.readBlocks() ?? <DynBlock>[]) {
      await dyn.delete(block: b.index);
    }
    await dyn.save();
    for (final b in await keyed.readBlocks() ?? <KeyedBlock>[]) {
      await keyed.delete(block: b.index);
    }
    await keyed.save();
    if (data != null) {
      final fromFile = <String>[];
      for (var off = 0; off + 16 <= data.length; off += 16) {
        final recOffset = uint32FromBytes(data, off);
        final fileSize = uint32FromBytes(data, off + 4);
        final unwritten = recOffset == 0xFFFFFFFF && fileSize == 0xFFFFFFFF;
        final invalidated = !unwritten && recOffset == 0;
        if (unwritten) continue;
        final name = String.fromCharCodes(data.sublist(off + 8, off + 16)).trim();
        fromFile.add('$name@$recOffset(${fileSize}B)${invalidated ? " DEL" : ""}');
      }
      final fromList = files.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList();
      // ignore: avoid_print
      print('[S] table-FILE records: $fromFile');
      // ignore: avoid_print
      print('[S] table-CID0 records:  $fromList');
      final agree = fromFile
          .where((e) => !e.contains(' DEL'))
          .map((e) => e.split('@').first)
          .toSet()
          .difference(fromList.map((e) => e.split('@').first).toSet());
      // ignore: avoid_print
      print('[S] in-file-but-not-CID0: ${agree.toList()}');
    }
  }, timeout: const Timeout(Duration(minutes: 2)));
}