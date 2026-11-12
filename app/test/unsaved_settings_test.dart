import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/unsaved_settings.dart';

/// The unsaved-writes state machine (`core/unsaved_settings.dart`), driven without a widget.
///
/// The rule the firmware forces (E71): a write to a `Persistent` field lands in the device's RAM
/// and the device's read-back cannot tell that from a saved value. So the app holds the flag, and
/// the flag clears only on a Save All that the device answered AND that read every field back as
/// the value that was written.
class _FakeClient extends RegisterClient {
  _FakeClient({super.deviceId = 1});

  /// The device's live values, by field. A write lands here so a read-back shows it.
  final fields = <int, List<int>>{};

  /// Fields whose answer to a READ is dropped, as a lost frame would be.
  final silentReads = <int>{};

  /// Fields a write is accepted for but does not change.
  final ignoredWrites = <int>{};

  /// What Save All answers.
  bool saveAllAnswers = true;

  int saves = 0;
  final writes = <({int field, List<int> bytes})>[];

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    if (silentReads.contains(field)) return null;
    final value = fields[field];
    if (value == null) return null;
    return (
      meta: ValueInfo(type: DataType.bool_.value, size: value.length),
      value: value,
    );
  }

  @override
  Future<List<int>?> writeBlockField(
    int blockType,
    int instance,
    int field,
    int key,
    ValueInfo meta,
    List<int> value,
  ) async {
    writes.add((field: field, bytes: value));
    if (!ignoredWrites.contains(field)) fields[field] = value;
    return const [1];
  }

  @override
  Future<bool> saveAll({Duration timeout = const Duration(seconds: 25)}) async {
    saves++;
    return saveAllAnswers;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  PendingSetting pending(int field, List<int> bytes, {String? name}) =>
      PendingSetting(
        deviceId: 1,
        blockName: 'Bluetooth',
        blockType: 0x0B,
        inst: 0,
        field: field,
        fieldName: name ?? (field == 0 ? 'Enabled' : 'Display name'),
        meta: ValueInfo(
          type: DataType.bool_.value,
          size: bytes.length,
          flags: ValueFlags.persistent,
        ),
        bytes: bytes,
      );

  group('the record of what is not saved', () {
    test('a write marks the field dirty, for that device only', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));

      expect(store.isDirty(1), isTrue);
      expect(store.isDirty(2), isFalse);
      expect(store.describe(1), 'Enabled');
      expect(store.dirtyDevices, [1]);
    });

    test('a second write to one field replaces it and keeps its place', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      store.mark(pending(1, [1]));
      store.mark(pending(0, [1]));

      expect(store.describe(1), 'Enabled and Display name');
      expect(store.of(1).map((entry) => entry.bytes), [
        [1],
        [1],
      ]);
    });

    test('two blocks with one field name are told apart', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      store.mark(
        PendingSetting(
          deviceId: 1,
          blockName: 'WiFi',
          blockType: 0x0C,
          inst: 0,
          field: 0,
          fieldName: 'Enabled',
          meta: const ValueInfo(type: 0, flags: ValueFlags.persistent),
          bytes: const [1],
        ),
      );

      expect(store.describe(1), 'Bluetooth Enabled and WiFi Enabled');
    });

    test('resolving one field leaves the other unsaved', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      store.mark(pending(1, [0]));

      store.resolve([store.of(1).first]);

      expect(store.describe(1), 'Display name');
    });

    test('a change made for one device is not another device\'s change', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      store.clearDevice(1);

      expect(store.isDirty(1), isFalse);
      expect(store.dirtyDevices, isEmpty);
    });
  });

  group('saving is the only thing that clears the flag', () {
    test('a save the device answers, and reads back, clears the field', () async {
      final store = UnsavedSettings.forTest();
      final client = _FakeClient();
      store.mark(pending(0, [0]));
      client.fields[0] = [0];

      final outcome = await savePendingSettings(
        client,
        deviceId: 1,
        store: store,
      );

      expect(outcome.clean, isTrue);
      expect(outcome.saveAnswered, isTrue);
      expect(outcome.message, 'Saved: Enabled.');
      expect(store.isDirty(1), isFalse);
      // The write is re-applied before the save, because the device may have reset while the
      // change sat unsaved.
      expect(client.writes, hasLength(1));
      expect(client.saves, 1);
    });

    test(
      'a save the device does not answer keeps the field and names it',
      () async {
        final store = UnsavedSettings.forTest();
        final client = _FakeClient()..saveAllAnswers = false;
        store.mark(pending(0, [0]));

        final outcome = await savePendingSettings(
          client,
          deviceId: 1,
          store: store,
        );

        expect(outcome.clean, isFalse);
        expect(outcome.unsaved, ['Enabled']);
        expect(store.isDirty(1), isTrue);
        expect(
          outcome.message,
          'Not saved: Enabled - the device did not answer Save all, so the change is still in RAM '
          'only and a reset brings the old value back.',
        );
      },
    );

    test('a read-back that does not answer confirms nothing', () async {
      final store = UnsavedSettings.forTest();
      final client = _FakeClient()..silentReads.add(0);
      store.mark(pending(0, [0]));

      final outcome = await savePendingSettings(
        client,
        deviceId: 1,
        store: store,
      );

      expect(outcome.clean, isFalse);
      expect(outcome.unsaved, ['Enabled']);
      expect(store.isDirty(1), isTrue);
      expect(outcome.message, startsWith('Not confirmed: Enabled'));
    });

    test('a value the device reads as the old one is not saved', () async {
      final store = UnsavedSettings.forTest();
      final client = _FakeClient()
        ..fields[0] = [1]
        ..ignoredWrites.add(0);
      store.mark(pending(0, [0]));

      final outcome = await savePendingSettings(
        client,
        deviceId: 1,
        store: store,
      );

      expect(outcome.clean, isFalse);
      expect(
        outcome.message,
        startsWith('Not saved: Enabled - the device reads back a'),
      );
      expect(store.isDirty(1), isTrue);
    });

    test('a write the device refuses is reported and saves nothing', () async {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      final refusing = _RefusingClient();
      store.mark(pending(1, [1]));

      final outcome = await savePendingSettings(
        refusing,
        deviceId: 1,
        store: store,
      );

      expect(outcome.saveAnswered, isFalse);
      expect(outcome.clean, isFalse);
      expect(
        outcome.message,
        contains('Not written: Enabled and Display name'),
      );
      // Nothing was accepted, so no Save All was sent at all.
      expect(refusing.saves, 0);
      expect(store.isDirty(1), isTrue);
    });

    test('two fields are reported apart: one saved, one not', () async {
      final store = UnsavedSettings.forTest();
      final client = _FakeClient()..silentReads.add(1);
      store.mark(pending(0, [0]));
      store.mark(pending(1, [1]));

      final outcome = await savePendingSettings(
        client,
        deviceId: 1,
        store: store,
      );

      expect(outcome.saved, ['Enabled']);
      expect(outcome.unsaved, ['Display name']);
      expect(store.describe(1), 'Display name');
      expect(
        outcome.message,
        'Saved: Enabled. Not confirmed: Display name - Save all answered but the read-back did '
        'not, so treat it as unsaved.',
      );
    });

    test('nothing pending asks the device nothing', () async {
      final client = _FakeClient();
      final outcome = await savePendingSettings(
        client,
        deviceId: 1,
        store: UnsavedSettings.forTest(),
      );

      expect(outcome.clean, isTrue);
      expect(client.saves, 0);
      expect(client.writes, isEmpty);
      expect(outcome.message, contains('Nothing to save'));
    });

    test(
      'a save somebody else sent is confirmed by the read-back alone',
      () async {
        // The register view sends Save All itself; the panels' entries still have to be checked
        // against the device rather than assumed saved.
        final store = UnsavedSettings.forTest();
        final client = _FakeClient();
        store.mark(pending(0, [1]));
        client.fields[0] = [1];

        final outcome = await confirmPendingSettings(
          client,
          deviceId: 1,
          store: store,
        );

        expect(outcome.clean, isTrue);
        expect(client.saves, 0, reason: 'nothing is sent by a confirmation');
        expect(client.writes, isEmpty);
        expect(store.isDirty(1), isFalse);
      },
    );

    test('a confirmation the device disagrees with keeps the entry', () async {
      final store = UnsavedSettings.forTest();
      final client = _FakeClient()..fields[0] = [0];
      store.mark(pending(0, [1]));

      final outcome = await confirmPendingSettings(
        client,
        deviceId: 1,
        store: store,
      );

      expect(outcome.clean, isFalse);
      expect(store.describe(1), 'Enabled');
    });

    test('a recall forgets a field whatever key it was written with', () {
      final store = UnsavedSettings.forTest();
      store.mark(pending(0, [0]));
      store.mark(pending(1, [1]));

      // The register view addresses a static field with its own key sentinel (0xFF), while the
      // panel wrote with the device's primary key: a recall still ends the record.
      store.resolveField(1, blockType: 0x0B, inst: 0, field: 0);

      expect(store.describe(1), 'Display name');
    });
  });

  group('comparing a written value with a read one', () {
    test('the device\'s NUL padding is not a difference', () {
      expect(
        sameFieldValue(
          [66, 101, 110, 99, 104],
          [
            66, 101, 110, 99, 104, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
          ],
        ),
        isTrue,
      );
      expect(sameFieldValue([0], [0, 0, 0, 0]), isTrue);
    });

    test('any other byte difference is one', () {
      expect(
        sameFieldValue(
          [66, 101, 110, 99, 104],
          [
            66, 101, 110, 99, 104, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 66, //
          ],
        ),
        isFalse,
      );
      expect(sameFieldValue([1], [0]), isFalse);
      expect(sameFieldValue([0, 1], [1]), isFalse);
    });
  });
}

/// A device that refuses every write - the shape a link the block will not accept produces.
class _RefusingClient extends RegisterClient {
  _RefusingClient({super.deviceId = 1});

  int saves = 0;

  @override
  Future<List<int>?> writeBlockField(
    int blockType,
    int instance,
    int field,
    int key,
    ValueInfo meta,
    List<int> value,
  ) async => null;

  @override
  Future<bool> saveAll({Duration timeout = const Duration(seconds: 25)}) async {
    saves++;
    return true;
  }
}
