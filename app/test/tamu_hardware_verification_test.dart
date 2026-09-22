@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  // HIL: core ping and Register System block fields
  test('HIL: core ping and Register System block fields', skip: skipReason, () async {
    expect(ConnectionManager.instance.isConnected, isTrue);
    final db = DeviceDatabase.instance;
    expect(await db.pingCore(), isTrue);

    // Register System block type 0 reads via Register 01.01
    final link = ConnectionManager.instance;
    Future<List<int>?> regRead(int field, int key) async {
      final payload = blockInfoBytes(0, 0, field, key);
      return await link.request(1, ServiceType.register, 1, payload: payload);
    }
    final type = await regRead(0, 0);
    expect(type, isNotNull, reason: 'Register System DeviceType');
    expect(type!.length, greaterThan(4));
    final cap = await regRead(0, 1);
    expect(cap, isNotNull);
    final sn = await regRead(1, 0xFF);
    expect(sn, isNotNull);
    expect(sn!.length, greaterThanOrEqualTo(14));
    final devName = await regRead(6, 0xFF);
    expect(devName, isNotNull);
    expect(devName, isNotNull);
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: Tamu SystemMemory blocks 6 and flags via Register
  test('HIL: Tamu SystemMemory blocks 6 and flags via Register', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: 1);
    // Enumerate block types (Enum 0)
    final types = await reg.enumerateBlockTypes();
    expect(types, isNotNull);
    // Count static blocks by iterating instances
    int totalBlocks = 0;
    if (types != null) {
      for (final t in types) {
        final count = await reg.getInstanceCount(t);
        if (count != null) totalBlocks += count;
      }
    }
    expect(totalBlocks, 6, reason: 'Tamu v2.0A should have 6 static blocks');
    // LEDButton (type 3) layout per Docs/Modules and blocks/Buttons & LEDS.md:
    // field 0 = Button raw state (RO), field 3 = LEDState (TR).
    final btnState = await reg.readBlockField(BlockType.ledButton.value, 0, 0, 0xFF);
    expect(btnState, isNotNull);
    expect(btnState!.meta.readOnly, isTrue,
        reason: 'LEDButton field 0 (Button raw state) must be read-only');
    final ledState = await reg.readBlockField(BlockType.ledButton.value, 0, 3, 0xFF);
    expect(ledState, isNotNull);
    expect(ledState!.meta.flags & FieldFlags.trigger, isNot(0),
        reason: 'LEDButton field 3 (LEDState) must be a trigger field');
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: storage 112 frag create/read/write/delete
  test('HIL: storage 112 frag create/read/write/delete', skip: skipReason, () async {
    final storage = StorageClient(deviceId: 1);
    final table1 = await storage.readFileTable();
    expect(table1, isNotNull);
    final name = 'TST112  ';
    await storage.deleteFile(name);
    expect(await storage.createFile(name, 250), isTrue);
    final data = List<int>.generate(250, (i) => i & 0xFF);
    expect(await storage.writeFile(name, data), isTrue);
    final read = await storage.readFile(name, size: 250);
    expect(read, equals(data));
    expect(await storage.deleteFile(name), isTrue);
    final table2 = await storage.readFileTable();
    expect(table2, isNotNull);
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: Device 00.0x/00.1x SNDB and TimeSync
  test('HIL: Device 00.0x/00.1x SNDB and TimeSync', skip: skipReason, () async {
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();
    final core = db.byId(1);
    expect(core, isNotNull);
    expect(core!.capabilities & Capability.core, isNot(0));
    final entries = await db.sndbEntries();
    expect(entries.any((e) => e.$1 == 1), isTrue);
    // TimeSync via Register uptime
    await db.refreshRuntime(1);
    expect(db.byId(1)!.uptimeMs, greaterThan(0));
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: Register Enumerate and BlockInfo
  test('HIL: Register Enumerate and BlockInfo', skip: skipReason, () async {
    final link = ConnectionManager.instance;
    // Enumerate block types via Register 01.00 Enum 0 (enum_level + BlockInfo).
    final reply = await link.request(1, ServiceType.register, 0,
        payload: [0, 0, 0, 0, 0]);
    expect(reply.length, greaterThanOrEqualTo(5), reason: 'BlockInfo echo + types');
    expect(reply[4], greaterThan(0), reason: 'Tamu has static blocks');
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: System block NetID (field 7) write. Docs: "applies only after reboot", so the
  // write must be accepted and stored without changing the live NetID (re-addressing the
  // core mid-session would break the app/bus link to it).
  test('HIL: System NetID write is accepted and not applied live', skip: skipReason,
      () async {
    final reg = RegisterClient(deviceId: 1);
    final before = await reg.readField(7, 0);
    expect(before, isNotNull);
    final current = before!.value.first;
    final next = current == 0x30 ? 0x31 : 0x30;
    Future<List<int>?> write(int v) => reg.writeBlockField(0, 0, 7, 0,
        BlockMeta(
            flagsAndType: DataType.id.value | FieldFlags.persistent, size: 1),
        [v]);

    // Out-of-range values are rejected (0 = unassigned, 0x3F = all nets).
    expect(await write(0), isNull);
    expect(await write(0x3F), isNull);
    // A valid value is accepted...
    expect(await write(next), isNotNull);
    // ...but the live NetID is unchanged until the next boot.
    expect((await reg.readField(7, 0))!.value.first, current);
    // Restore the original stored value.
    expect(await write(current), isNotNull);
  }, timeout: const Timeout(Duration(seconds: 60)));

  // HIL: DAS has its intended blocks (Docs/Devices.md: Resistive measurement x2, Button,
  // LED), enumerated through the bus via the Tamu core relay.
  test('HIL: DAS static blocks', skip: skipReason, () async {
    final db = DeviceDatabase.instance;
    await db.refreshRuntime(0);
    await Future.delayed(const Duration(seconds: 2));
    await db.refreshRuntime(0);
    DeviceEntry? das;
    for (final d in db.all) {
      if (d.type == DeviceType.dualAnalogSensor) {
        das = d;
        break;
      }
    }
    if (das == null) {
      print('DAS not found on the bus - skipping block assertions');
      return;
    }
    final reg = RegisterClient(deviceId: das.id);
    final types = await reg.enumerateBlockTypes();
    expect(types, isNotNull);
    int totalBlocks = 0;
    final typeSet = <int>{};
    if (types != null) {
      for (final t in types) {
        typeSet.add(t);
        final count = await reg.getInstanceCount(t);
        if (count != null) totalBlocks += count;
      }
    }
    expect(totalBlocks, 4, reason: 'DAS should have 4 static blocks (Meas1, Meas2, Button, LED)');
    expect(typeSet.contains(BlockType.resistiveMeasure.value), isTrue);
    expect(typeSet.contains(BlockType.button.value), isTrue);
    expect(typeSet.contains(BlockType.led.value), isTrue);
  }, timeout: const Timeout(Duration(seconds: 60)));
}