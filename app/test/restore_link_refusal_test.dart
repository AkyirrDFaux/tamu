// A restore's rows follow the firmware's own write rule for the block they target.
//
// `BLE Settings` used to be USB-only (`BleBlockWritable` was `AppFrameCameOverUsb()`), and the
// plan refused its rows on any other link (card t_686176aa). The firmware changed the rule on
// 2026-11-06 (`t_1ff14f60`, `Blocks/BleWritePolicy.h`):
//
//   BleWriteAllowed(AppFrameCameOverUsb(), AppFrameCameOverBle(), AppBLELinkSecured())
//
// - USB always, a BLE frame only on a secured session, verified on the rig by a bonded-BLE write
// answering flags `0x0E`. Every link this app holds is such a session, so a restore planned over
// Bluetooth now writes the block like any other row; the refusal sentence stays for a link that
// genuinely cannot write it (a BLE link that is not a secured session).

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

BackupDevice _archive() => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('1.1')!,
  name: 'Akyirr',
  blocks: [
    BackupBlock(
      type: 'BLE Settings',
      typeIndex: BlockType.bleSettings.value,
      instance: 0,
      name: 'BLE Settings',
      isDynamic: false,
      entries: [
        BackupEntry(
          field: 'Enabled',
          fieldIndex: 0,
          key: 'Key 0',
          keyIndex: 0,
          type: 'Bool',
          flags: const ['Persistent'],
          value: true,
          size: 1,
        ),
      ],
    ),
  ],
);

/// The live core, carrying the same block with the same field so the row RESOLVES: a refusal
/// under test is the link's, never a missing target.
LiveDevice _live() => LiveDevice(
  id: idFromString('1.1')!,
  name: 'Akyirr',
  capabilities: Capability.core,
  blocks: [
    LiveBlock(
      type: BlockType.bleSettings.value,
      instance: 0,
      name: 'BLE Settings',
      isDynamic: false,
      entries: [
        LiveEntry(
          field: 0,
          key: 0,
          fieldName: 'Enabled',
          keyName: 'Key 0',
          meta: ValueInfo(type: DataType.bool_.value, size: 1),
          value: const [1],
        ),
      ],
    ),
  ],
);

RestorePlan _plan({required bool overUsb, bool bleLinkSecured = true}) =>
    planFromSnapshot(
      devices: [_archive()],
      live: [_live()],
      inventories: {
        _live().id: const LiveInventory(
          fileNames: {},
          loadedSlots: {},
          requesters: [],
          sndb: [],
        ),
      },
      linkRefusedBlocks: linkWriteRefusals(
        overUsb: overUsb,
        bleLinkSecured: bleLinkSecured,
      ),
    );

/// A core that accepts every request, so `applyRestorePlan` can be driven without a rig. The
/// register write is what this file measures; the sweeps a restore runs afterwards (slot map,
/// file table, requester table) answer empty, which the app reads as "the device holds none".
class _AcceptingBus implements RequestChannel {
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF000;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    sent.add((service: service, cid: functionCid, payload: payload));
    return const [];
  }

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    sent.add((service: service, cid: functionCid, payload: payload));
    // An accepted register write echoes the address and the ValueInfo it was sent, then the
    // value: the reply IS the request payload.
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return PacketResponse(payload: payload, success: true, fail: false);
    }
    return const PacketResponse(payload: [], success: true, fail: false);
  }
}

void main() {
  tearDown(() => installRequestChannel(null));

  test('a restore planned over Bluetooth writes the BLE Settings rows', () async {
    // The app's own BLE link: secured by the device's admission policy, so the block is
    // writable and nothing is refused before the write is sent.
    final plan = _plan(overUsb: false);
    expect(plan.linkRefusedBlocks, isEmpty);
    final item = plan.items.single;
    expect(item.issue, isNull);
    expect(item.ready, isTrue);
    expect(item.selected, isTrue);

    // ... and the run APPLIES it: one row written, none skipped, none failed, and the write that
    // reached the bus is the block's own field (type 0x0B, instance 0, field 0).
    final bus = _AcceptingBus();
    installRequestChannel(bus);
    final report = await applyRestorePlan(plan);
    // ignore: avoid_print
    print('BLE restore: ${report.counts}');
    expect(report.written, 1);
    expect(report.skipped, isEmpty);
    expect(report.failed, 0, reason: report.text);

    final write = bus.sent.firstWhere(
      (r) => r.service == ServiceType.register && r.cid == RegisterCid.write,
      orElse: () =>
          (service: ServiceType.device, cid: -1, payload: const <int>[]),
    );
    expect(write.cid, RegisterCid.write, reason: 'the block write was sent');
    final info = uint32FromBytes(write.payload);
    expect(blockInfoType(info), BlockType.bleSettings.value);
    expect(blockInfoInstance(info), 0);
    expect(blockInfoField(info), 0);
  });

  test('a plan over USB is unchanged', () async {
    final plan = _plan(overUsb: true);
    final item = plan.items.single;

    expect(item.issue, isNull);
    expect(item.ready, isTrue);
    expect(item.selected, isTrue);
    expect(plan.linkRefusedBlocks, isEmpty);

    final bus = _AcceptingBus();
    installRequestChannel(bus);
    final report = await applyRestorePlan(plan);
    expect(report.written, 1);
    expect(report.failed, 0, reason: report.text);
  });

  test('a link that is not a secured session still refuses, with the reason', () {
    // The hypothetical the sentence machinery is kept for: the firmware's rule refuses a BLE frame
    // on a link that is not a secured session, so the plan must not claim those rows are writable.
    final refusals = linkWriteRefusals(overUsb: false, bleLinkSecured: false);
    expect(refusals.keys, [BlockType.bleSettings.value]);
    final sentence = refusals[BlockType.bleSettings.value]!;
    expect(sentence, contains('not a secured session'));
    expect(sentence, isNot(contains('only be written over USB')));

    final plan = _plan(overUsb: false, bleLinkSecured: false);
    final item = plan.items.single;
    expect(item.ready, isFalse, reason: 'the firmware refuses this write');
    expect(item.selected, isFalse, reason: 'an unplaceable row is never sent');
    expect(item.issue, sentence);
  });

  test('a secured Bluetooth link is not the refusal case', () {
    // The rule itself, so a caller cannot drift from the firmware's three inputs.
    expect(bleLinkWritesSettings(overUsb: true), isTrue);
    expect(bleLinkWritesSettings(overUsb: false), isTrue);
    expect(
      bleLinkWritesSettings(overUsb: false, bleLinkSecured: false),
      isFalse,
    );
    expect(bleWriteRefusal(overUsb: false), 'The device refused the change.');
  });
}
