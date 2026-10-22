import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';

/// Provider-side subscriptions: the model is still READ (an archive captured by an earlier build
/// carries them) but they are neither captured nor a restore item. The provider table is RAM-only
/// on the serving device (`Docs/Services/Subscriptions.md` - "active until cancelled; the
/// information is not persistent on this side"), so the archive must not carry it at all: it would
/// be state with no persistence file behind it. The node rebuilds the table when the requesters
/// that own each entry re-register after boot (CID 0x01 Change on their first tick, and again at
/// boot), which is why writing the provider half as well duplicated the work and its read-back
/// verification raced with the node's own table churn (B34).
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

  test('a captured provider subscription survives the archive JSON', () {
    // The TRID keys the provider table, so it has to survive the archive JSON for the capture to
    // be a faithful record even though the restore no longer replays it.
    expect(BackupProviderSubscription.fromJson(sub.toJson()).trid, 0x1005);
  });

  test('the restore is requester-only: a provider subscription yields no item', () async {
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: const [],
      providerSubscriptions: const [sub],
    );
    final plan = await buildRestorePlan([device]);
    expect(plan.items, isEmpty);
  });
}
