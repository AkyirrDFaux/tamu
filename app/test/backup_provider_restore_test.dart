import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/types.dart';

/// B22 (Issues.md): provider-side subscriptions were captured but never restored, so a device
/// stopped serving the remote requesters it served. The restore rebuilds each one with the
/// provider-set command (CID 0x21), which carries the requester's address in the payload - the
/// requester need not be present to re-register.
void main() {
  const source = BackupBlockRef(
    block: 'Resistive measurement',
    typeIndex: 8,
    instance: 0,
    field: 'Measured Value',
    fieldIndex: 3,
    key: 'Key 0',
    keyIndex: 0,
  );
  const sub = BackupProviderSubscription(
    requester: '0.2',
    trid: 0x1005,
    trigger: 'Periodic',
    periodMs: 1000,
    minTimeMs: 100,
    deadzone: 0.5,
    source: source,
  );

  test('a captured provider subscription is restored, not just captured', () async {
    // The TRID keys the provider table, so it has to survive the archive JSON.
    expect(BackupProviderSubscription.fromJson(sub.toJson()).trid, 0x1005);

    // The captured entry becomes a restorable item instead of being dropped on the floor.
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: const [],
      providerSubscriptions: const [sub],
    );
    final plan = await buildRestorePlan([device]);
    final items = plan.items
        .where((i) => i.kind == RestoreKind.provider)
        .toList();
    expect(items, hasLength(1));
    expect(items.single.providerSubscription!.requester, '0.2');

    // It resolves ready against a target that provides and carries the source register.
    final live = LiveDevice(
      id: 1,
      name: 'Tamu',
      capabilities: Capability.subscriptionProvide,
      blocks: [
        LiveBlock(
          type: 8,
          instance: 0,
          name: 'Resistive measurement',
          isDynamic: false,
          entries: [
            LiveEntry(
              field: 3,
              key: 0,
              fieldName: 'Measured Value',
              keyName: 'Key 0',
              meta: ValueInfo(type: DataType.number.value, size: 4),
            ),
          ],
        ),
      ],
    );
    final item = RestoreItem(
      kind: RestoreKind.provider,
      device: device,
      providerSubscription: sub,
    );
    RestorePlan(
      devices: [device],
      liveDevices: [live],
      items: [item],
      deviceTargets: {1: live},
      blockTargets: const {},
    ).resolve();
    expect(item.issue, isNull);

    // An archive captured before the TRID was recorded carries 0; installing that would create a
    // provider entry no node can key, so the item is left unready rather than written.
    final stale = RestoreItem(
      kind: RestoreKind.provider,
      device: device,
      providerSubscription: const BackupProviderSubscription(
        requester: '0.2',
        trid: 0,
        trigger: 'Periodic',
        periodMs: 1000,
        minTimeMs: 100,
        deadzone: 0.5,
        source: source,
      ),
    );
    RestorePlan(
      devices: [device],
      liveDevices: [live],
      items: [stale],
      deviceTargets: {1: live},
      blockTargets: const {},
    ).resolve();
    expect(stale.issue, 'Unknown subscription id');

    // The install payload is the full 32-byte entry: the requester's address rides the payload
    // (Change, 0x01, would take it from the packet source and could not name a remote requester),
    // and the TRID rides the packet header.
    final entry = ProviderSubscription(
      index: 0,
      requesterAddr: 2,
      trid: 0x1005,
      table: SubscriptionTable(
        sourceReg: makeBlockInfo(8, 0, 3, 0),
        trigger: TriggerType.periodic,
        minTimeMs: 100,
        periodMs: 1000,
        deadzone: 0.5,
      ),
      lastSentMs: 0,
      hash: 0,
      timeout: 0,
    );
    final payload = entry.toBytes();
    expect(payload.length, 32);
    final back = ProviderSubscription.fromBytes(0, payload);
    expect(back.requesterAddr, 2);
    expect(back.trid, 0x1005);
    expect(back.sourceReg, makeBlockInfo(8, 0, 3, 0));
    expect(back.periodMs, 1000);
  });
}
