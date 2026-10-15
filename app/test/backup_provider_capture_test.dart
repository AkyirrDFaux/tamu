import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';

/// Provider-side subscriptions are captured into the archive for the record, but they are NOT a
/// restore item: a provider entry is created by the requester that owns it, and the restored
/// requester re-registers itself (CID 0x01 Change on its first tick, and again at boot), so the
/// node rebuilds its own provider table. The restore is requester-only; writing the provider half
/// as well duplicated that work and its read-back verification raced with the node's own table
/// churn (B34).
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
