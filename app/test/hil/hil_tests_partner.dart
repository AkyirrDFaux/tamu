/// Partner tests: two or more devices, coupled (Docs/HIL Tests.md).
///
/// Each test couples the core (the only device that advertises
/// `subscriptionRequest` and `appInterface`) with a node partner that provides a
/// value. They change network-wide state - the requester/provider subscription
/// tables and the bus addressing - so the runner is `HilRunClass.coupled`: one
/// at a time, after the independent set, and a skip (never a failure) on a
/// one-device rig via the `partner:` clause.
///
/// Ported from the legacy `hil_subscriptions_test.dart` end-to-end flows and the
/// `tamu_hardware_verification_test.dart` "SNDB and TimeSync" and DAS-clock
/// tests. The cross-device subscription assertions were observed failing on the
/// bench (the node's Measured Value not reaching the core target; the pushed
/// scalar not landing / the provider Hash not holding the last sent value); they
/// are kept as real assertions so the run documents the defect.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';

/// A node that can provide values: `subscriptionProvide` plus the measurement
/// blocks the cross-device flows read from.
const HilPrereq _nodeProvider = HilPrereq(
  isCore: false,
  capabilities: [Capability.subscriptionProvide],
  blocks: [BlockType.resistiveMeasure],
);

/// A node on the bus, paired with the core for the discovery/SNDB/TimeSync
/// tests. The address of a node is assigned by the core's discover broadcast.
const HilPrereq _node = HilPrereq(
  isCore: false,
  capabilities: [Capability.node],
);

/// The core as requester: it owns the requester table (only it advertises
/// `subscriptionRequest`) and is the app-interface entry point, coupled to a
/// provider node.
const HilPrereq _coreRequester = HilPrereq(
  isCore: true,
  capabilities: [Capability.appInterface, Capability.subscriptionRequest],
  partner: _nodeProvider,
);

/// The core, coupled to any node partner - the network-wide cases (discovery,
/// SNDB, TimeSync) that need a second device on the bus.
const HilPrereq _coreWithNode = HilPrereq(
  isCore: true,
  capabilities: [Capability.appInterface],
  partner: _node,
);

/// Creates a scratch dynamic block at [index] on [reg] holding a single entry
/// (field 0, key 0) of [type], initialised to [value] (or zeros). A dynamic
/// entry is a writable register, so it makes a subscription target.
Future<DynBlock> _scratchTarget(
  RegisterClient reg,
  int index,
  DataType type,
  int size, {
  List<int>? value,
}) async {
  await reg.deleteDynamic(block: index);
  await reg.createDynamicBlock('SUBTGT', index: index);
  final block = DynBlock(
    index: index,
    meta: ValueInfo(type: dynamicTypeForIndex(index), size: 1),
    name: 'SUBTGT',
  );
  await reg.writeDynamicEntry(
    block,
    0,
    0,
    ValueInfo(type: type.value, key: 0),
    value ?? List<int>.filled(size, 0),
  );
  return block;
}

/// The BlockInfo of a dynamic block addressed by its global index (the bank type
/// is derived from the index).
int _dynReg(int global, int field, [int key = 0]) => makeBlockInfo(
  dynamicTypeForIndex(global),
  dynamicInstanceForIndex(global),
  field,
  key,
);

/// Cancels every requester subscription on [client]: a deterministic start for a
/// coupled test.
Future<void> _clearRequesterTable(SubscriptionClient client) async {
  var subs = await client.getRequesterSubscriptions();
  while (subs.isNotEmpty) {
    await client.setRequesterSubscription(subs.first.index);
    subs = await client.getRequesterSubscriptions();
  }
}

/// Puts the requester table back the way [saved] recorded it, so a coupled test
/// never leaves the device's subscriptions changed.
Future<void> _restoreRequesterTable(
  SubscriptionClient client,
  List<RequesterSubscription> saved,
) async {
  await _clearRequesterTable(client);
  for (final s in saved) {
    await client.setRequesterSubscription(s.index, entry: s);
  }
}

/// The scratch subscription target's dynamic index, clear of the deployed
/// scenario's blocks (0-2).
const int _scrSubTgt = 32;

final List<HilTest> partnerTests = <HilTest>[
  HilTest(
    'subscription: node Measured Value reaches a core target',
    category: HilCategory.partner,
    runClass: HilRunClass.coupled,
    prereq: _coreRequester,
    body: (ctx) async {
      final core = ctx.device(_coreRequester);
      final node = ctx.rig.partnerFor(_nodeProvider, exclude: core);
      if (node == null) fail('the runner guaranteed a provider partner');
      ctx.note('requester ${core.label} <- provider ${node.label}');

      final reg = RegisterClient(deviceId: core.id);
      final dasReg = RegisterClient(deviceId: node.id);
      final client = SubscriptionClient(deviceId: core.id);
      DynBlock? target;
      List<RequesterSubscription>? saved;
      try {
        saved = await client.getRequesterSubscriptions();
        await _clearRequesterTable(client);
        final t = await _scratchTarget(reg, _scrSubTgt, DataType.number, 4);
        target = t;

        // Node Meas1 (ResistiveMeasure inst 0) Measured Value = field 3.
        final sourceReg = makeBlockInfo(
          BlockType.resistiveMeasure.value,
          0,
          3,
          0,
        );
        final targetReg = _dynReg(_scrSubTgt, 0, 0);

        // Read the provider value directly so we can compare after the flow.
        final direct = await dasReg.readBlockField(
          BlockType.resistiveMeasure.value,
          0,
          3,
          0,
        );
        final expected = direct == null ? 0.0 : numberFromBytes(direct.value);
        ctx.note('node Measured Value = $expected');

        final entry = RequesterSubscription(
          index: 0,
          providerAddr: node.id,
          trid: 0xFA00,
          targetReg: targetReg,
          sourceReg: sourceReg,
          trigger: TriggerType.periodic,
          periodMs: 200,
          minTimeMs: 50,
        );
        expect(
          await client.setRequesterSubscription(0, entry: entry),
          isTrue,
          reason: 'the core accepted the requester entry',
        );

        // The provider table on the node should now hold an entry.
        final provClient = SubscriptionClient(deviceId: node.id);
        await Future<void>.delayed(const Duration(milliseconds: 300));
        final provs = await provClient.getProviderSubscriptions();
        ctx.note('node provider entries: ${provs.length}');
        expect(
          provs,
          isNotEmpty,
          reason: 'the node registered the provider entry',
        );

        await Future<void>.delayed(const Duration(milliseconds: 800));
        final applied = await reg.readDynamicField(t, 0, 0);
        final value = applied == null ? 0.0 : numberFromBytes(applied.value);
        ctx.note('core target value = $value');
        // The applied value must track the provider (non-zero for a connected
        // sensor). This is the assertion observed failing on the bench.
        expect(
          value,
          isNot(0.0),
          reason: 'the node Measured Value must reach the core target',
        );
      } finally {
        if (target != null) await reg.deleteDynamic(block: target.index);
        if (saved != null) await _restoreRequesterTable(client, saved);
      }
    },
  ),
  HilTest(
    'subscription: node scalar delta provider keeps its last value',
    category: HilCategory.partner,
    runClass: HilRunClass.coupled,
    prereq: _coreRequester,
    body: (ctx) async {
      final core = ctx.device(_coreRequester);
      final node = ctx.rig.partnerFor(_nodeProvider, exclude: core);
      if (node == null) fail('the runner guaranteed a provider partner');
      ctx.note('requester ${core.label} <- provider ${node.label}');

      final reg = RegisterClient(deviceId: core.id);
      final dasReg = RegisterClient(deviceId: node.id);
      final client = SubscriptionClient(deviceId: core.id);
      DynBlock? target;
      ValueInfo? srcMeta;
      List<int>? coeffBefore;
      List<RequesterSubscription>? saved;
      try {
        saved = await client.getRequesterSubscriptions();
        await _clearRequesterTable(client);
        final t = await _scratchTarget(
          reg,
          _scrSubTgt,
          DataType.number,
          4,
          value: numberToBytes(-1),
        );
        target = t;

        // A *writable* source on the node: its ResistiveMeasure Filter
        // Coefficient, so the check is deterministic while still exercising the
        // node's provider table. (Measured Value drifts with room light, and a
        // fire-and-forget delta push can leave the provider's last-sent hash
        // ahead of the value that reached the target.)
        final sourceReg = makeBlockInfo(
          BlockType.resistiveMeasure.value,
          0,
          2,
          0,
        );
        final targetReg = _dynReg(_scrSubTgt, 0, 0);
        final before = await dasReg.readBlockField(
          BlockType.resistiveMeasure.value,
          0,
          2,
          0,
        );
        expect(before, isNotNull, reason: 'node FilterCoeff readable');
        coeffBefore = before!.value;
        srcMeta = ValueInfo(
          type: before.meta.type,
          flags: before.meta.flags,
          key: 0,
          size: before.value.length,
        );

        final entry = RequesterSubscription(
          index: 0,
          providerAddr: node.id,
          trid: 0xFC00,
          targetReg: targetReg,
          sourceReg: sourceReg,
          trigger: TriggerType.deltaPeriodic,
          periodMs: 60000, // long: only the change branch fires
          minTimeMs: 200,
          deadzone: 0.05,
        );
        expect(await client.setRequesterSubscription(0, entry: entry), isTrue);

        // The provider push is fire-and-forget; wait for the node to hold the
        // entry before checking its state.
        final provClient = SubscriptionClient(deviceId: node.id);
        bool registered(List<ProviderSubscription> provs) =>
            provs.any((p) => p.periodMs == 60000 && p.sourceReg == sourceReg);
        var ready = false;
        for (var attempt = 0; attempt < 10 && !ready; attempt++) {
          await Future<void>.delayed(const Duration(milliseconds: 400));
          ready = registered(await provClient.getProviderSubscriptions());
        }
        expect(
          ready,
          isTrue,
          reason: 'the node never received the provider entry',
        );

        // Drive the source to a known value, retrying the nudge if a push went
        // missing.
        var landed = false;
        var sent = 0.0;
        for (var attempt = 0; attempt < 6 && !landed; attempt++) {
          sent = 0.2 + 0.1 * attempt;
          await dasReg.writeBlockField(
            BlockType.resistiveMeasure.value,
            0,
            2,
            0,
            srcMeta,
            numberToBytes(sent),
          );
          for (var i = 0; i < 8 && !landed; i++) {
            await Future<void>.delayed(const Duration(milliseconds: 200));
            final cur = await reg.readDynamicField(t, 0, 0);
            landed =
                cur != null &&
                (numberFromBytes(cur.value) - sent).abs() < 0.005;
          }
        }
        expect(
          landed,
          isTrue,
          reason: 'the pushed value never reached the target',
        );

        // The provider's Hash/Hashlike is the last SENT scalar for a Number, so it
        // must equal the raw value that reached the target - not an FNV hash (a
        // stray confirmation used to overwrite it after every update, breaking the
        // scalar deadzone).
        final mine = (await provClient.getProviderSubscriptions())
            .where((p) => p.periodMs == 60000 && p.sourceReg == sourceReg)
            .toList();
        final raw = (sent * 65536).round() & 0xFFFFFFFF;
        final hash = mine.isEmpty ? 0 : mine.first.hash;
        ctx.note(
          'node provider: sent=$sent hash=0x${hash.toRadixString(16)} '
          'expected=0x${raw.toRadixString(16)}',
        );
        expect(
          hash,
          raw,
          reason:
              'the provider Hash must hold the last sent scalar, not a hash',
        );
      } finally {
        if (srcMeta != null && coeffBefore != null) {
          await dasReg.writeBlockField(
            BlockType.resistiveMeasure.value,
            0,
            2,
            0,
            srcMeta,
            coeffBefore,
          );
        }
        if (target != null) await reg.deleteDynamic(block: target.index);
        if (saved != null) await _restoreRequesterTable(client, saved);
      }
    },
  ),
  HilTest(
    'broadcast: the core discovers every node on the bus',
    category: HilCategory.partner,
    runClass: HilRunClass.coupled,
    prereq: _coreWithNode,
    body: (ctx) async {
      final core = ctx.device(_coreWithNode);
      final node = ctx.rig.partnerFor(_node, exclude: core);
      if (node == null) fail('the runner guaranteed a node partner');

      // A node broadcasts Device Discover until the core assigns it an address;
      // the core then reaches it over the bus. A full sweep must therefore leave
      // every listed device reachable (not stale) and the node on the local net.
      final db = DeviceDatabase.instance;
      await db.refreshNetwork();
      final devices = db.all;
      ctx.note(
        'bus lists ${devices.length} device(s): '
        '${devices.map((d) => idToString(d.id)).join(', ')}',
      );
      for (final d in devices) {
        expect(
          d.stale,
          isFalse,
          reason:
              'device ${idToString(d.id)} must answer after the discovery sweep',
        );
      }
      expect(idNet(node.id), 0, reason: 'the node sits on the core local net');
      expect(
        idDevice(node.id),
        greaterThan(0),
        reason: 'the node holds an address assigned by the core',
      );
    },
  ),
  HilTest(
    'sndb: the core stores the node, not itself',
    category: HilCategory.partner,
    runClass: HilRunClass.coupled,
    prereq: _coreWithNode,
    body: (ctx) async {
      final core = ctx.device(_coreWithNode);
      final node = ctx.rig.partnerFor(_node, exclude: core);
      if (node == null) fail('the runner guaranteed a node partner');

      final db = DeviceDatabase.instance;
      await db.refreshNetwork();
      final coreEntry = db.byId(core.id);
      expect(coreEntry, isNotNull);
      expect(coreEntry!.capabilities & Capability.core, isNot(0));

      final entries = await db.sndbEntries();
      ctx.note(
        'SNDB entries: '
        '${entries.map((e) => '${idToString(e.$1)}=${e.$2}').join(', ')}',
      );
      // The core does not register itself; the SNDB lists the net's devices
      // (local devices with net 0, other cores with NetID.1), so no entry may be
      // the connected core.
      expect(
        entries.any(
          (e) => e.$1 == core.id || (idDevice(e.$1) == 1 && idNet(e.$1) != 0),
        ),
        isFalse,
        reason: 'the core must not appear in its own SNDB',
      );
      // The partner node is a local device, stored with NetID 0.
      expect(
        entries.any((e) => e.$1 == node.id),
        isTrue,
        reason: 'the node must appear in the SNDB',
      );

      // TimeSync via Register uptime: both devices report a live clock.
      await db.refreshRuntime(core.id);
      expect(db.byId(core.id)!.uptimeMs, greaterThan(0));
      await db.refreshRuntime(node.id);
      expect(db.byId(node.id)!.uptimeMs, greaterThan(0));
    },
  ),
  HilTest(
    'timesync: the node tracks the core clock',
    category: HilCategory.partner,
    runClass: HilRunClass.coupled,
    prereq: _coreWithNode,
    body: (ctx) async {
      final core = ctx.device(_coreWithNode);
      final node = ctx.rig.partnerFor(_node, exclude: core);
      if (node == null) fail('the runner guaranteed a node partner');

      final coreReg = RegisterClient(deviceId: core.id);
      final nodeReg = RegisterClient(deviceId: node.id);
      final ble = ConnectionManager.instance.source == LinkSource.ble;
      ctx.note(
        'core ${core.label} vs node ${node.label}'
        '${ble ? ' (BLE link)' : ''}',
      );

      // Wait until the node's applied offset matches the raw core-node difference, i.e. it has
      // re-synced after the core clock restarted (the HIL connection can reset it).
      //
      // The node syncs on its own 60-75 s cadence plus up to 15 s jitter, so a deadline near that
      // period fails whenever the wait starts just after a sync (observed on the bench: an 80 s
      // deadline timed out, then the same test passed in 10 s on the next run). Bound it past the
      // worst case (90 s) and keep the runner's per-test timeout above this one; USB converges in
      // seconds, BLE is reported, not gated.
      final deadline = DateTime.now().add(Duration(seconds: ble ? 20 : 100));
      var converged = false;
      while (DateTime.now().isBefore(deadline)) {
        final coreRead = await coreReg.readField(3, 0);
        final nodeRead = await nodeReg.readField(3, 0);
        final offsetRead = await nodeReg.readField(3, 2);
        // A busy bus can drop a read; retry instead of aborting the wait.
        if (coreRead == null || nodeRead == null || offsetRead == null) {
          await Future<void>.delayed(const Duration(seconds: 3));
          continue;
        }
        final coreRaw = uint32FromBytes(coreRead.value);
        final nodeRaw = uint32FromBytes(nodeRead.value);
        final offset = int32FromBytes(offsetRead.value);
        if ((offset - (coreRaw - nodeRaw)).abs() < 15) {
          ctx.note('converged offset=$offset rawDiff=${coreRaw - nodeRaw}');
          converged = true;
          break;
        }
        await Future<void>.delayed(const Duration(seconds: 3));
      }
      expect(converged, isTrue, reason: 'the node never re-synced to the core');

      // Compare the clocks, interpolating the core "Now" around the node read so
      // the round-trip skew cancels.
      final c0Read = await coreReg.readField(3, 1);
      final dRead = await nodeReg.readField(3, 1);
      final c1Read = await coreReg.readField(3, 1);
      expect(
        c0Read != null && dRead != null && c1Read != null,
        isTrue,
        reason: 'the clock compare reads must answer',
      );
      final c0 = uint32FromBytes(c0Read!.value);
      final d = uint32FromBytes(dRead!.value);
      final c1 = uint32FromBytes(c1Read!.value);
      final coreMid = (c0 + c1) ~/ 2;
      final diff = d - coreMid;
      ctx.note('coreMid=$coreMid node=$d diff=${diff}ms');
      if (ble) {
        // Over BLE the residual is dominated by link latency, not by the sync
        // (the same run took minutes to converge); report it and do not gate.
        ctx.note('BLE link: reporting the offset without asserting');
        return;
      }
      // The node syncs itself to the core and then tracks its *rate* between
      // syncs; its internal RC drifts ~1%, so the residual depends on when the
      // last sync happened. 25 ms still catches a broken sync (orders of
      // magnitude out) while tolerating the cadence.
      expect(
        diff.abs(),
        lessThan(25),
        reason: 'the node clock must track the core (achieved ${diff}ms)',
      );
    },
  ),
];
