/// Hardware-in-the-loop test: drives the app's REAL core stack (UsbTransport ->
/// ConnectionManager -> DeviceDatabase / SystemMemoryClient / StorageClient)
/// against the live Tamu + DAS network.
///
/// Run (requires the Tamu core on a USB port and the bundled native lib):
/// ```
/// LIBSERIALPORT_PATH=build/linux/x64/debug/bundle/lib/libserialport.so \
/// TAMU_HIL=/dev/ttyACM0 flutter test test/hil_live_test.dart
/// ```
/// Skipped automatically when TAMU_HIL is not set, so plain `flutter test`
/// stays green on machines without hardware.
library;

import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/sysmem.dart';
import 'package:tamuapp/core/types.dart';

const int dasId = 2;
/// The core's BLE MAC (matches the Bluetooth MAC printed at boot).
const String kCoreBleMacAddress = 'E4:B0:63:C8:20:72';
final Timeout hilTimeout = const Timeout(Duration(seconds: 75));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  // Route universal_ble to its pure-Dart BlueZ implementation under flutter
  // test (the default target platform would otherwise select a plugin channel
  // that does not exist outside a real app shell).
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  // TAMU_HIL selects the run mode:
  //   unset            -> skip (CI machines stay green)
  //   /dev/ttyACMx     -> USB transport
  //   ble              -> BLE transport (scans for a device named "Tamu")
  final hilTarget = Platform.environment['TAMU_HIL'];
  final useBle = hilTarget == 'ble';
  final skipReason = hilTarget == null ? 'TAMU_HIL not set' : false;

  Future<void> connectApp() async {
    final mgr = ConnectionManager.instance;
    if (useBle) {
      mgr.source = LinkSource.ble;
      await mgr.setAutoRefresh(false);
      // BLE scan results can be sparse right after a session close (BlueZ
      // suppression + the device restarting advertising), so retry the scan.
      DiscoveredLink? link;
      for (var round = 0; round < 3 && link == null; round++) {
        await mgr.refresh();
        final deadline = DateTime.now().add(const Duration(seconds: 12));
        while (link == null && DateTime.now().isBefore(deadline)) {
          await Future<void>.delayed(const Duration(milliseconds: 500));
          final bleCandidates = mgr.discoveredLinks
              .where((l) => l.type == LinkType.ble)
              .toList();
          link = bleCandidates
              .where((l) => l.id == kCoreBleMacAddress)
              .firstOrNull;
        }
        await mgr.stopScan();
      }
      if (link == null) fail('Tamu not found during BLE scan');
      final err = await mgr.connectTo(link);
      if (err != null) fail('BLE connect failed: $err');
    } else {
      final err = await mgr.connectTo(DiscoveredLink(
          id: hilTarget!, type: LinkType.usb, name: 'Tamu core'));
      if (err != null) fail('connect failed: $err');
    }
    // Opening/closing the port pulses DTR/RTS which resets the ESP32-C3, so a
    // fresh session may start while the core is still booting: wait until it
    // actually answers before letting a test proceed.
    for (var i = 0; i < 20; i++) {
      if (await DeviceDatabase.instance.pingCore()) return;
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    fail('core did not answer ping within settle window');
  }

  Future<void> disconnectApp() => ConnectionManager.instance.disconnect();

  test('HIL: link comes up and core answers ping', () async {
    await connectApp();
    addTearDown(disconnectApp);
    expect(ConnectionManager.instance.isConnected, isTrue);

    final db = DeviceDatabase.instance;
    final ok = await db.pingCore();
    expect(ok, isTrue, reason: 'core did not answer Device/ping');
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: network discovery finds core + DAS with correct identity', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();

    final core = db.byId(1);
    expect(core, isNotNull, reason: 'core missing from database');
    expect(core!.type, DeviceType.tamuV20A,
        reason: 'core type mismatch: ${core.type}');
    expect(core.serialNumber, isNotNull);
    expect(core.capabilities & Capability.core, Capability.core,
        reason: 'core capability bit missing');

    final das = db.byId(dasId);
    expect(das, isNotNull, reason: 'DAS not discovered via SNDB');
    expect(das!.type, DeviceType.dualAnalogSensor,
        reason: 'DAS type mismatch: ${das.type}');
    expect(das.serialNumber, isNotNull);
    expect(das.name, contains('DAS'));
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: runtime probes return sane values', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final db = DeviceDatabase.instance;
    await db.refreshDevice(1);
    await db.refreshRuntime(1);
    final core = db.byId(1)!;
    expect(core.uptimeMs, greaterThan(0));
    expect(core.avgLoopTimeMs, greaterThan(0));
    expect(core.timeOffsetMs, isNotNull);

    await db.refreshDevice(dasId);
    await db.refreshRuntime(dasId);
    final das = db.byId(dasId)!;
    expect(das.uptimeMs, greaterThan(0),
        reason: 'DAS uptime should be positive after sync');
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: system memory walk on both devices', () async {
    await connectApp();
    addTearDown(disconnectApp);

    final coreMem = SystemMemoryClient(deviceId: 1);
    final coreBlocks = await coreMem.readBlocks();
    expect(coreBlocks, isNotNull);
    expect(coreBlocks!.length, 6, reason: 'core block count');
    // Fan duty write round-trip (duty is in percent).
    final fanBlock = coreBlocks.firstWhere((b) => b.index == 1);
    final duty = await coreMem.readField(fanBlock, 1);
    expect(duty, isNotNull);
    final restored = numberToBytes(0.0);
    final written = await coreMem.writeField(fanBlock, duty!, restored);
    expect(written, isNotNull, reason: 'fan duty write rejected');
    final readBack = await coreMem.readField(fanBlock, 1);
    expect(numberFromBytes(readBack!.value, 0), closeTo(0.0, 0.001));

    final dasMem = SystemMemoryClient(deviceId: dasId);
    final dasBlocks = await dasMem.readBlocks();
    expect(dasBlocks, isNotNull);
    expect(dasBlocks!.length, 2, reason: 'DAS block count (Meas1/Meas2)');
    final meas1 = dasBlocks.firstWhere((b) => b.index == 0);
    final coeff = await dasMem.readField(meas1, 1);
    expect(coeff, isNotNull, reason: 'FilterCoeff field read');
    // FilterCoeff is bounded to >= 0 (higher values average more samples, per
    // Devices/DAS_v0.1/Measuring.h); a negative write clamps to 0, a positive one
    // is stored verbatim.
    await dasMem.writeField(meas1, coeff!, numberToBytes(2.5));
    final stored = await dasMem.readField(meas1, 1);
    expect(numberFromBytes(stored!.value, 0), closeTo(2.5, 0.001),
        reason: 'FilterCoeff > 0 stored verbatim');
    await dasMem.writeField(meas1, coeff, numberToBytes(-0.5));
    final clampedNeg = await dasMem.readField(meas1, 1);
    expect(numberFromBytes(clampedNeg!.value, 0), closeTo(0.0, 0.001),
        reason: 'FilterCoeff negative clamps to 0');
    // Restore defaults and persist.
    await dasMem.writeField(meas1, coeff, numberToBytes(0.5));
    final rate = await dasMem.readField(meas1, 0);
    await dasMem.writeField(meas1, rate!, numberToBytes(10.0));
    expect(await dasMem.save(), isTrue, reason: 'DAS save failed');
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: storage client table/create/duplicate/delete', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final storage = StorageClient(deviceId: dasId);

    final table = await storage.readFileTable();
    expect(table, isNotNull);
    expect(table!.any((f) => f.name == '.TABLE'), isTrue,
        reason: 'file table self-entry missing');

    final del = await storage.deleteFile('TSAPP'); // clean any leftovers
    // ignore: avoid_print
    print('pre-delete TSAPP: $del');
    final created = await storage.createFile('TSAPP', 64);
    // ignore: avoid_print
    print('create TSAPP -> $created');
    expect(created, isTrue, reason: 'create TSAPP');
    expect(await storage.createFile('TSAPP', 32), isFalse,
        reason: 'duplicate create must fail');
    final after = await storage.readFileTable();
    final recs = after!.where((f) => f.name == 'TSAPP').toList();
    expect(recs.length, 1, reason: 'exactly one TSAPP record expected');
    expect(await storage.deleteFile('TSAPP'), isTrue);
    final cleaned = await storage.readFileTable();
    expect(cleaned!.any((f) => f.name == 'TSAPP'), isFalse);

    // writeFile (CID 7 FRAG stream) -> readFile round trip on the CORE (the DAS's
    // compact node frames cap incoming payloads below the standard 256-byte write
    // chunk; the app never writes DAS files today). Spans several 256-byte
    // fragments so the last fragment's padding is exercised.
    final coreStorage = StorageClient(deviceId: 1);
    final payload = List<int>.generate(600, (i) => i & 0xFF);
    expect(await coreStorage.writeFile('TSW', payload), isTrue,
        reason: 'write file stream');
    final back = await coreStorage.readFile('TSW', size: payload.length);
    expect(back, payload, reason: 'read-back matches written payload');
    expect(await coreStorage.deleteFile('TSW'), isTrue);
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: transaction layer handles concurrent requests', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final db = DeviceDatabase.instance;
    final pings =
        List.generate(8, (_) => db.pingCore());
    final results = await Future.wait(pings);
    for (final ok in results) {
      expect(ok, isTrue, reason: 'one of 8 concurrent pings lost');
    }
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: request to absent device times out', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final sw = Stopwatch()..start();
    try {
      await ConnectionManager.instance
          .request(9, ServiceType.device, 1, timeout: const Duration(seconds: 2));
      fail('request to absent device must time out');
    } on Exception {
      sw.stop();
      expect(sw.elapsedMilliseconds, lessThan(4000),
          reason: 'timeout took too long');
    }
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: SNDB dump matches discovered devices', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final db = DeviceDatabase.instance;
    final entries = await db.sndbEntries();
    expect(entries.map((e) => e.$1), containsAll([1, dasId]));
    for (final (id, sn) in entries) {
      expect(sn.length, 28, reason: 'SNDB entry $id SN hex length');
      final viaLookup = await db.serialNumberOf(id);
      expect(viaLookup, sn, reason: 'SN lookup mismatch for id $id');
    }
  }, timeout: hilTimeout, skip: skipReason);
}
