/// Generic tests: one device at a time (Docs/HIL Tests.md, "Test Categories").
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';

/// Every device the scan reaches. The app interface is the rig's entry
/// condition (the core reaches the nodes for us), so it is not a per-device
/// prerequisite.
const HilPrereq _any = HilPrereq();

/// The core, reachable through the app interface.
const HilPrereq _core =
    HilPrereq(isCore: true, capabilities: [Capability.appInterface]);

final List<HilTest> genericTests = [
  HilTest(
    'core answers a ping',
    category: HilCategory.generic,
    prereq: _core,
    body: (ctx) async {
      expect(await DeviceDatabase.instance.pingCore(), isTrue,
          reason: 'the core answers a ping');
    },
  ),
  HilTest(
    'each device reports its type',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        ctx.note('${d.label}: type 0x${d.type.value.toRadixString(16)}');
        expect(d.type, isNot(DeviceType.unknown),
            reason: 'device ${d.id} reports a known type');
      }
    },
  ),
  HilTest(
    'each device lists the System block first',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final types = await RegisterClient(deviceId: d.id).enumerateBlockTypes();
        expect(types, isNotNull, reason: 'device ${d.id} answers enumeration');
        expect(types!, isNotEmpty,
            reason: 'device ${d.id} has at least the System block');
        expect(types.first.type, 0, reason: 'device ${d.id} System block leads');
      }
    },
  ),
  HilTest(
    'each device reports a non-zero uptime',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        await DeviceDatabase.instance.refreshRuntime(d.id);
        final uptime = DeviceDatabase.instance.byId(d.id)?.uptimeMs;
        expect(uptime, isNotNull, reason: 'device ${d.id} reports uptime');
        expect(uptime!, greaterThan(0), reason: 'device ${d.id} uptime > 0');
      }
    },
  ),
  HilTest(
    'each device reports a software version',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final read = await RegisterClient(deviceId: d.id).readField(0, 2);
        expect(read, isNotNull, reason: 'device ${d.id} software version');
        expect(read!.value.length, greaterThanOrEqualTo(4),
            reason: 'device ${d.id} version is 32bit (YY:MM:DD:II)');
        final word = uint32FromBytes(read.value);
        ctx.note('${d.label}: version 0x${word.toRadixString(16)}');
        expect(word, isNot(0), reason: 'device ${d.id} version is stamped');
      }
    },
  ),
  HilTest(
    'each device reports a serial number',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final sn = await RegisterClient(deviceId: d.id).readField(1, 0xFF);
        expect(sn, isNotNull, reason: 'device ${d.id} serial number');
        expect(sn!.value.length, greaterThanOrEqualTo(14),
            reason: 'device ${d.id} serial number length');
      }
    },
  ),
  HilTest(
    'each device advertises a capability set naming its role',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final read = await RegisterClient(deviceId: d.id).readField(0, 1);
        expect(read, isNotNull, reason: 'device ${d.id} capability');
        final bits = uint32FromBytes(read!.value);
        ctx.note('${d.label}: capability 0x${bits.toRadixString(16)}');
        expect(bits, isNot(0), reason: 'device ${d.id} capability is non-zero');
        if (d.isCore) {
          expect(bits & Capability.core, isNot(0),
              reason: 'device ${d.id} is a core, so it says so');
        } else {
          expect(bits & Capability.node, isNot(0),
              reason: 'device ${d.id} is a node, so it says so');
        }
      }
    },
  ),
  HilTest(
    'each device reports the type discovery learned',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final read = await RegisterClient(deviceId: d.id).readField(0, 0);
        expect(read, isNotNull, reason: 'device ${d.id} device type');
        expect(uint32FromBytes(read!.value), d.type.value,
            reason: 'device ${d.id} type agrees with discovery');
      }
    },
  ),
  HilTest(
    'each device reports a name',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final read = await RegisterClient(deviceId: d.id).readField(6, 0xFF);
        expect(read, isNotNull, reason: 'device ${d.id} name');
        expect(read!.value, isNotEmpty, reason: 'device ${d.id} name is non-empty');
        expect(read.value.length, lessThanOrEqualTo(16),
            reason: 'device ${d.id} name fits the 16-byte clamp');
      }
    },
  ),
  HilTest(
    'each device reports RAM use within its total',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final reg = RegisterClient(deviceId: d.id);
        final used = await reg.readField(4, 0);
        final total = await reg.readField(4, 1);
        expect(used, isNotNull, reason: 'device ${d.id} used RAM');
        expect(total, isNotNull, reason: 'device ${d.id} total RAM');
        final u = uint32FromBytes(used!.value);
        final t = uint32FromBytes(total!.value);
        ctx.note('${d.label}: RAM $u/$t bytes');
        expect(t, greaterThan(0), reason: 'device ${d.id} total RAM is non-zero');
        expect(u, lessThanOrEqualTo(t),
            reason: 'device ${d.id} used RAM fits in the total');
      }
    },
  ),
  HilTest(
    'each device clock does not run backwards',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      for (final d in ctx.devices(_any)) {
        final reg = RegisterClient(deviceId: d.id);
        final first = await reg.readField(3, 0);
        expect(first, isNotNull, reason: 'device ${d.id} uptime');
        await Future<void>.delayed(const Duration(milliseconds: 200));
        final second = await reg.readField(3, 0);
        expect(second, isNotNull, reason: 'device ${d.id} uptime');
        final a = uint32FromBytes(first!.value);
        final b = uint32FromBytes(second!.value);
        ctx.note('${d.label}: uptime $a -> $b ms');
        expect(b, greaterThanOrEqualTo(a),
            reason: 'device ${d.id} uptime is monotonic');
      }
    },
  ),
  HilTest(
    'the bus holds at least one core',
    category: HilCategory.generic,
    prereq: _any,
    body: (ctx) async {
      final cores = ctx.devices(const HilPrereq(isCore: true));
      ctx.note('cores: ${cores.map((d) => d.label).join(", ")}');
      // More than one core is legitimate (a core TimeSyncs to the longest running one).
      expect(cores, isNotEmpty, reason: 'the bus holds at least one core');
    },
  ),
  HilTest(
    'the core reports a NetID',
    category: HilCategory.generic,
    prereq: _core,
    body: (ctx) async {
      for (final d in ctx.devices(_core)) {
        final read = await RegisterClient(deviceId: d.id).readField(7, 0);
        expect(read, isNotNull, reason: 'device ${d.id} NetID');
        expect(read!.value, isNotEmpty, reason: 'device ${d.id} NetID is set');
        // NetID is documented uint8 (Docs/Services/System Block and Device Commands.md).
        expect(read.value.length, 1, reason: 'device ${d.id} NetID is one byte');
        expect(read.value.first, isNot(0), reason: 'device ${d.id} NetID is non-zero');
        ctx.note('${d.label}: NetID ${read.value.first}');
      }
    },
  ),
];
