import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

/// A core that answers only what a capture of its SNDB needs: the ping, and the roster read
/// (Device 0x13). Everything else refuses - the block walk, the file table and the requester
/// table - so the capture comes back carrying exactly the SNDB rows, which is the thing under
/// test. A core whose roster holds only itself then has nothing to archive at all.
class _FakeSndbCore implements RequestChannel {
  _FakeSndbCore(this.roster);

  /// The roster the core reports, as `(id, 28-character hex serial)` - the shape the SNDB reader
  /// hands every caller.
  final List<({int id, String serial})> roster;

  int _txId = 0x100;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => _txId++;

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
    if (service == ServiceType.device && functionCid == 1) return const [];
    if (service == ServiceType.device && functionCid == 0x13) {
      final out = <int>[];
      for (final entry in roster) {
        out
          ..addAll(_serial(entry.serial))
          ..addAll([entry.id & 0xFF, (entry.id >> 8) & 0xFF]);
      }
      return out;
    }
    throw StateError('$service/$functionCid not answered');
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
  }) async => throw StateError('$service/$functionCid not answered');
}

/// A 14-byte serial from its 28-character hex form (the shape `idToString`/`serialNumberToHex`
/// use for the SNDB roster).
List<int> _serial(String hex) => [
  for (var i = 0; i < hex.length; i += 2)
    int.parse(hex.substring(i, i + 2), radix: 16),
];

const _kCoreSerial = 'E4B063C820700000000000000000';
const _kChild2Serial = 'CDABA26400000000000000000000';
const _kChild3Serial = 'CDABBD6400000000000000000000';

/// bambi's rig, as its own archive recorded it: the core at 0.1 first (itself), then the two
/// children. Ids are compared as network addresses, so 2 is `0.2`.
List<({int id, String serial})> _rigRoster() => [
  (id: 1, serial: _kCoreSerial),
  (id: 2, serial: _kChild2Serial),
  (id: 3, serial: _kChild3Serial),
];

BackupSndbEntry _entry(String serial, int id) =>
    BackupSndbEntry(serial: serial, address: idToString(id));

/// An archive of the core at 0.1 carrying [sndb] - the shape an archive captured before
/// 2026-10-23 still has on disk.
BackupDevice _archive({required List<BackupSndbEntry> sndb, String? serial}) =>
    BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      serial: serial,
      blocks: const [],
      sndb: sndb,
    );

LiveDevice _liveCore({int id = 1, String? serial}) => LiveDevice(
  id: id,
  name: 'Tamu',
  serial: serial,
  capabilities: Capability.core,
  blocks: const [],
);

void main() {
  late RequestChannel? previous;

  setUp(() {
    previous = requestChannel;
  });

  tearDown(() => installRequestChannel(previous));

  test(
    'a core that holds itself and two children captures two SNDB rows',
    () async {
      final roster = _rigRoster();
      installRequestChannel(_FakeSndbCore(roster));

      final captured = await captureDevice(1);

      // Printed on purpose: the transcript the card asks for, re-runnable rather than pasted.
      print(
        'SNDB on the device: ${roster.map((e) => '${idToString(e.id)} '
            '(${e.serial})').join(', ')}',
      );
      print(
        'SNDB in the archive: ${captured?.sndb.map((e) => '${e.address} '
            '(${e.serial})').join(', ')}',
      );
      expect(
        captured,
        isNotNull,
        reason: 'the two child rows are worth archiving',
      );
      expect(
        captured!.sndb.length,
        2,
        reason: 'three rows read, the self row dropped',
      );
      expect({for (final e in captured.sndb) e.address}, {'0.2', '0.3'});
      expect(
        {for (final e in captured.sndb) e.serial},
        {_kChild2Serial, _kChild3Serial},
      );
      expect(
        captured.sndb.any((e) => e.address == '0.1'),
        isFalse,
        reason: 'the core does not address itself',
      );
      // The format is unchanged: the rows are the roster's, verbatim.
      final json = captured.toJson();
      expect((json['sndb'] as List).length, 2);
    },
  );

  test('a core whose roster holds only itself captures no SNDB rows', () async {
    installRequestChannel(_FakeSndbCore([(id: 1, serial: _kCoreSerial)]));

    final captured = await captureDevice(1);

    // The SNDB was the only thing this device had to archive, so there is nothing to back up at
    // all - not a device carrying one meaningless row.
    print('SNDB-only capture: $captured');
    expect(captured, isNull);
    // And the two readers that decide "how many rows" agree, so the device's own record is not
    // quietly kept under another form.
    expect(archiveSndbEntries(1, [(id: 1, serial: _kCoreSerial)]), isEmpty);
    expect(
      archiveSndbEntries(1, _rigRoster(), serial: _kCoreSerial).length,
      2,
      reason: 'the serial settles the self row even beside the address rule',
    );
    // The archive's `sndb` key is written as an empty list when the device carries none.
    expect(_archive(sndb: const []).toJson()['sndb'], isEmpty);
  });

  test('a row is the device\'s own by its address OR by its serial', () {
    // The address names the device: its own bus address cannot be a peer.
    expect(isSelfSndbEntry(1, null, _entry(_kChild2Serial, 1)), isTrue);
    // The serial is the device's own, under another address (a re-addressed core): still itself.
    expect(isSelfSndbEntry(1, _kCoreSerial, _entry(_kCoreSerial, 2)), isTrue);
    // Neither: a child, and it stays.
    expect(
      isSelfSndbEntry(1, _kCoreSerial, _entry(_kChild2Serial, 2)),
      isFalse,
    );
    // An unparsable address is not a self row by accident.
    expect(
      isSelfSndbEntry(
        1,
        _kCoreSerial,
        BackupSndbEntry(serial: _kChild2Serial, address: 'not an address'),
      ),
      isFalse,
    );
    // A device with no serial read is judged by its address alone.
    expect(isSelfSndbEntry(1, null, _entry(_kCoreSerial, 2)), isFalse);
  });

  test('an archive that still carries a self row never plans an item for it', () {
    // bambi/bambi_setup.zip as it is on disk: not rewritten, tolerated by the reader.
    final device = _archive(
      serial: _kCoreSerial,
      sndb: [
        _entry(_kCoreSerial, 1),
        _entry(_kChild2Serial, 2),
        _entry(_kChild3Serial, 3),
      ],
    );
    final live = _liveCore(serial: _kCoreSerial);

    final plan = planFromSnapshot(
      devices: [device],
      live: [live],
      inventories: {
        live.id: LiveInventory(
          sndb: [
            (id: 2, serial: _kChild2Serial),
            (id: 3, serial: _kChild3Serial),
          ],
        ),
      },
    );

    final rows = plan.items.where((i) => i.kind == RestoreKind.sndb).toList();
    print(
      'SNDB rows in the plan: '
      '${rows.isEmpty ? '-' : rows.map((i) => i.sndbEntry!.address).join(', ')}',
    );
    expect(rows.length, 2, reason: 'the self row is not a row at all');
    expect({for (final i in rows) i.sndbEntry!.address}, {'0.2', '0.3'});
    expect(
      rows.every((i) => i.ready),
      isTrue,
      reason: 'the children are ordinary rows and stay tickable',
    );
    expect(plan.removals, isEmpty, reason: 'a drop is not a deletion to show');
  });

  test(
    'a self row is dropped when the archive is restored onto the node it names',
    () {
      // The archive was captured at 0.1; the rig now answers at 0.2 with the same serial, so 0.2 is
      // the same core. A row naming 0.2 would write the target's own address into its own table.
      final device = _archive(
        serial: _kCoreSerial,
        sndb: [_entry(_kChild2Serial, 2)],
      );
      final live = _liveCore(id: 2, serial: _kCoreSerial);

      final plan = planFromSnapshot(
        devices: [device],
        live: [live],
        inventories: {live.id: const LiveInventory(sndb: [])},
      );

      print(
        'retargeted self row -> plan items: '
        '${plan.items.where((i) => i.kind == RestoreKind.sndb).length}',
      );
      expect(plan.deviceTargets[device.id]?.id, 2);
      expect(
        plan.items.where((i) => i.kind == RestoreKind.sndb),
        isEmpty,
        reason: 'the row names the node it is being restored onto',
      );
    },
  );
}
