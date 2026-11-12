/// The app's own record of the settings a device holds in RAM only (E71).
///
/// The firmware's save contract is fixed: a write to a `Persistent` field lands in RAM, and only
/// Save All (CID 5) writes the device's `.SV` backup. The device's read-back is the same either way,
/// so nothing on the wire ever says "this is not durable" - after a reset the old value is simply
/// back. For `Enabled = false` that means the radio comes up again while the user believes it is
/// off. The app is the only layer that knows a change was made and not saved, so this file is that
/// knowledge: one entry per field the app wrote and has not saved, kept per device.
///
/// Two rules shape the types:
///
///  * **The state belongs to the device, not to a page.** An entry outlives the panel that wrote
///    it, so leaving the settings page and coming back still reports the change as unsaved.
///  * **Only a confirmed Save All clears an entry.** [savePendingSettings] re-applies the write,
///    sends Save All and then reads every pending field back; a save that is refused, times out or
///    reads back a different value keeps the entries and names them.
///
/// Recall All (CID 4) is never sent from here: it loads `.SV` over the RAM copy, i.e. it discards
/// exactly the writes this file is holding.
library;

import 'package:flutter/foundation.dart';

import 'register_client.dart';
import 'types.dart';

/// One field value the device has accepted into RAM and has not written to its backup.
@immutable
class PendingSetting {
  const PendingSetting({
    required this.deviceId,
    required this.blockName,
    required this.blockType,
    required this.inst,
    required this.field,
    required this.fieldName,
    required this.meta,
    required this.bytes,
  });

  /// The device the write went to.
  final int deviceId;

  /// What the panel that wrote it calls the block ("Bluetooth", "WiFi"), used to qualify
  /// [fieldName] when two blocks of one device carry a field of the same name.
  final String blockName;

  /// The block the field lives in (`BLE Settings`, `WiFi Settings`, ...).
  final int blockType;

  /// The block instance.
  final int inst;

  /// The field index inside the block.
  final int field;

  /// The field's name as the panel prints it (`Display name`, `Enabled`, `SSID`).
  final String fieldName;

  /// The declared type, flags and key the write carried; [ValueInfo.key] addresses the value.
  final ValueInfo meta;

  /// The bytes that were written - what a re-apply sends, not what the device reads back.
  final List<int> bytes;

  /// Identity of the entry: one field of one block instance of one device.
  String get id => '$blockType/$inst/$field/${meta.key}';

  @override
  String toString() => 'PendingSetting($deviceId $blockName $fieldName)';
}

/// Every device's pending writes, by device id.
///
/// The app's instance is a singleton because the state is the device's, not a page's: a panel
/// marks into it, the panel is disposed, and the next visit (or the shell's watcher) reads the same
/// entry back. A test passes [UnsavedSettings.forTest] instead, so one suite never sees another's.
class UnsavedSettings extends ChangeNotifier {
  UnsavedSettings._();

  static final UnsavedSettings instance = UnsavedSettings._();

  /// A store of its own, holding nothing yet.
  @visibleForTesting
  factory UnsavedSettings.forTest() => UnsavedSettings._();

  final Map<int, Map<String, PendingSetting>> _byDevice = {};

  /// The pending writes of [deviceId], oldest first.
  List<PendingSetting> of(int deviceId) => List.unmodifiable(
    _byDevice[deviceId]?.values ?? const <PendingSetting>[],
  );

  /// Whether [deviceId] holds any write that is not durable.
  bool isDirty(int deviceId) => _byDevice[deviceId]?.isNotEmpty ?? false;

  /// Every device holding an unsaved write.
  List<int> get dirtyDevices => List.unmodifiable(_byDevice.keys);

  /// The fields of [deviceId] as one readable list ("Display name", "Display name and Enabled").
  String describe(int deviceId) => unsavedNames(labelsFor(of(deviceId)));

  /// Records [setting] as unsaved, replacing an earlier entry for the same field.
  ///
  /// A replacement keeps the entry's place: the list stays in the order the user made the changes,
  /// so "Display name and Enabled" does not shuffle when a field is corrected.
  ///
  /// The entry is per FIELD, whatever key addressed it. The settings panels write a field with the
  /// device's primary key while the register editor writes a static field with the field's own key
  /// (`0xFF`), and both change the one value the device holds - so an entry under another key for
  /// the same field is dropped here. Keeping both would let a Save all re-apply the older of the
  /// two values over the newer one and report the field twice in one sentence.
  void mark(PendingSetting setting) {
    final device = _byDevice.putIfAbsent(
      setting.deviceId,
      () => <String, PendingSetting>{},
    );
    device.removeWhere(
      (_, entry) =>
          entry.blockType == setting.blockType &&
          entry.inst == setting.inst &&
          entry.field == setting.field &&
          entry.id != setting.id,
    );
    device[setting.id] = setting;
    notifyListeners();
  }

  /// Forgets [settings] - the ones a confirmed save made durable.
  void resolve(Iterable<PendingSetting> settings) {
    var changed = false;
    for (final setting in settings) {
      final device = _byDevice[setting.deviceId];
      if (device == null) continue;
      if (device.remove(setting.id) != null) changed = true;
      if (device.isEmpty) _byDevice.remove(setting.deviceId);
    }
    if (changed) notifyListeners();
  }

  /// Forgets every pending write of one FIELD of [deviceId], whatever key it was written with.
  ///
  /// A register-page save or recall addresses a static field by its own key sentinel (`0xFF`, or a
  /// System member's key) while the panels use the device's primary key, so matching is on the
  /// field itself: the value the device holds for it changed under the app, either way.
  void resolveField(
    int deviceId, {
    required int blockType,
    required int inst,
    required int field,
  }) {
    final device = _byDevice[deviceId];
    if (device == null) return;
    final gone = [
      for (final setting in device.values)
        if (setting.blockType == blockType &&
            setting.inst == inst &&
            setting.field == field)
          setting,
    ];
    resolve(gone);
  }

  /// Forgets every pending write of [deviceId].
  void clearDevice(int deviceId) {
    if (_byDevice.remove(deviceId) != null) notifyListeners();
  }

  /// The labels of [settings], naming the block only where the device has two fields of one name
  /// (both the Bluetooth and the WiFi block carry an `Enabled`).
  List<String> labelsFor(Iterable<PendingSetting> settings) {
    final all = settings.toList();
    final counts = <String, int>{};
    for (final setting in all) {
      counts[setting.fieldName] = (counts[setting.fieldName] ?? 0) + 1;
    }
    return [
      for (final setting in all)
        (counts[setting.fieldName] ?? 0) > 1
            ? '${setting.blockName} ${setting.fieldName}'
            : setting.fieldName,
    ];
  }
}

/// "Display name", "Display name and Enabled", "Display name, Enabled and SSID".
///
/// A message that says only "unsaved changes" is the one the card forbids: the user has to know
/// which setting is at risk to judge whether leaving is safe.
String unsavedNames(List<String> names) {
  final unique = <String>[];
  for (final name in names) {
    if (!unique.contains(name)) unique.add(name);
  }
  if (unique.isEmpty) return 'nothing';
  if (unique.length == 1) return unique.single;
  return '${unique.take(unique.length - 1).join(', ')} and ${unique.last}';
}

/// Whether two field values are the same value, as the device may pad one of them.
///
/// A fixed-width text field is NUL-padded on the wire (`Docs/Services/Register.md`), so writing
/// "Bench" (5 bytes) and reading 16 bytes ending in eleven NULs is one value, not two. Trailing
/// zeros are the only normalisation: everything else is compared byte for byte, so the check cannot
/// pass over a value the device did not take.
bool sameFieldValue(List<int> want, List<int> got) {
  var wanted = want.length;
  while (wanted > 0 && want[wanted - 1] == 0) {
    wanted--;
  }
  var actual = got.length;
  while (actual > 0 && got[actual - 1] == 0) {
    actual--;
  }
  if (wanted != actual) return false;
  for (var i = 0; i < wanted; i++) {
    if (want[i] != got[i]) return false;
  }
  return true;
}

/// What became of one pending write when the app tried to make it durable.
enum SaveFate {
  /// The write landed, Save All answered and the field read back as the value that was written.
  durable,

  /// The device refused the write when it was re-applied, so nothing was sent for it.
  writeRefused,

  /// Save All did not answer (refused or timed out), so the backup was not written.
  saveFailed,

  /// Save All answered, but the field did not answer the read-back.
  notReadBack,

  /// Save All answered, and the field reads back a different value.
  different,
}

/// One pending write's fate, with the name the panels print for the field.
@immutable
class PendingResult {
  const PendingResult(this.setting, this.fate, this.label);

  final PendingSetting setting;
  final SaveFate fate;

  /// The field's name for a message: the panel's own word, qualified with the block's when the
  /// device carries the same field name twice.
  final String label;

  bool get durable => fate == SaveFate.durable;

  @override
  String toString() => 'PendingResult($label, ${fate.name})';
}

/// The outcome of one save: which writes are durable now and which the app still holds.
class SaveOutcome {
  const SaveOutcome({required this.results, required this.saveAnswered});

  /// One entry per pending write, in the order the changes were made.
  final List<PendingResult> results;

  /// Whether Save All itself answered success. False means the backup was not written.
  final bool saveAnswered;

  /// Whether every pending write is durable, so the store holds nothing for this device.
  bool get clean => results.every((result) => result.durable);

  /// The fields that are durable now.
  List<String> get saved => [
    for (final result in results)
      if (result.durable) result.label,
  ];

  /// The fields the app still holds, with the reason spelled out in [message].
  List<String> get unsaved => [
    for (final result in results)
      if (!result.durable) result.label,
  ];

  /// One sentence naming every field that is (or is not) durable.
  String get message => saveOutcomeMessage(this);
}

/// The sentence [SaveOutcome] carries: what is saved, and per reason what is not.
///
/// It is built here rather than in each panel so a save's report cannot drift from the store's
/// idea of what is still pending, and so a test can pin the wording once.
String saveOutcomeMessage(SaveOutcome outcome) {
  if (outcome.results.isEmpty) {
    return 'Nothing to save: the device holds no unsaved change.';
  }
  final parts = <String>[];
  if (outcome.saved.isNotEmpty) {
    parts.add('Saved: ${unsavedNames(outcome.saved)}.');
  }
  for (final fate in SaveFate.values) {
    final names = unsavedNames([
      for (final result in outcome.results)
        if (result.fate == fate) result.label,
    ]);
    if (names == 'nothing') continue;
    parts.add(switch (fate) {
      SaveFate.durable => '',
      SaveFate.writeRefused =>
        'Not written: $names - the device refused the write, so it is not there to save.',
      SaveFate.saveFailed =>
        'Not saved: $names - the device did not answer Save all, so the change is still in RAM '
            'only and a reset brings the old value back.',
      SaveFate.notReadBack =>
        'Not confirmed: $names - Save all answered but the read-back did not, so treat it as '
            'unsaved.',
      SaveFate.different =>
        'Not saved: $names - the device reads back a different value, so that is not what its '
            'backup holds.',
    });
  }
  return parts.where((part) => part.isNotEmpty).join(' ');
}

/// Makes every pending write of [deviceId] durable, and returns what each field's fate is.
///
/// The three steps are one operation because the device has no partial save (`Docs/Services/
/// Register.md`: "the device can only overwrite or load the whole file"):
///
///  1. **Re-apply** each pending write. The device may have reset while the link was down, so its
///     RAM can hold the OLD value while the app holds the user's change; writing it back is what
///     makes "save" mean "save the user's value". The write is idempotent for a field the device
///     already holds.
///  2. **Save All** (CID 5) - the only command that writes `.SV`. A refused or unanswered save
///     leaves every entry in place: a timeout is not a save.
///  3. **Read every field back** and clear only the entries that read back as written. The
///     read-back cannot show durability, but it does show which value the device now holds, which
///     is the half the app can check.
Future<SaveOutcome> savePendingSettings(
  RegisterClient client, {
  required int deviceId,
  UnsavedSettings? store,
  Duration timeout = const Duration(seconds: 25),
}) async {
  final settings = store ?? UnsavedSettings.instance;
  final pending = settings.of(deviceId);
  if (pending.isEmpty) {
    return const SaveOutcome(results: [], saveAnswered: true);
  }
  final labels = settings.labelsFor(pending);

  final refused = <String>{};
  for (final setting in pending) {
    final reply = await client.writeBlockField(
      setting.blockType,
      setting.inst,
      setting.field,
      setting.meta.key,
      setting.meta,
      setting.bytes,
    );
    if (reply == null) refused.add(setting.id);
  }

  final accepted = [
    for (final setting in pending)
      if (!refused.contains(setting.id)) setting,
  ];
  // A Save All with nothing left to save would only report a device state nobody changed.
  final answered = accepted.isEmpty
      ? false
      : await client.saveAll(timeout: timeout);

  final results = <PendingResult>[];
  final durable = <PendingSetting>[];
  for (var i = 0; i < pending.length; i++) {
    final setting = pending[i];
    final label = labels[i];
    if (refused.contains(setting.id)) {
      results.add(PendingResult(setting, SaveFate.writeRefused, label));
      continue;
    }
    if (!answered) {
      results.add(PendingResult(setting, SaveFate.saveFailed, label));
      continue;
    }
    final read = await client.readBlockField(
      setting.blockType,
      setting.inst,
      setting.field,
      setting.meta.key,
    );
    if (read == null) {
      results.add(PendingResult(setting, SaveFate.notReadBack, label));
      continue;
    }
    if (sameFieldValue(setting.bytes, read.value)) {
      results.add(PendingResult(setting, SaveFate.durable, label));
      durable.add(setting);
    } else {
      results.add(PendingResult(setting, SaveFate.different, label));
    }
  }
  settings.resolve(durable);
  return SaveOutcome(results: results, saveAnswered: answered);
}

/// Reads every pending write of [deviceId] back and clears the ones the device holds, without
/// sending anything: the confirmation half of [savePendingSettings], for a Save All somebody else
/// sent.
///
/// The register view sends Save All itself (its own button, the doc's "Save all"), so the entries
/// the settings panels hold have to be confirmed against the device rather than assumed saved:
/// this is the same read-back [savePendingSettings] ends on, minus the write and the save.
Future<SaveOutcome> confirmPendingSettings(
  RegisterClient client, {
  required int deviceId,
  UnsavedSettings? store,
}) async {
  final settings = store ?? UnsavedSettings.instance;
  final pending = settings.of(deviceId);
  if (pending.isEmpty) {
    return const SaveOutcome(results: [], saveAnswered: true);
  }
  final labels = settings.labelsFor(pending);
  final results = <PendingResult>[];
  final durable = <PendingSetting>[];
  for (var i = 0; i < pending.length; i++) {
    final setting = pending[i];
    final read = await client.readBlockField(
      setting.blockType,
      setting.inst,
      setting.field,
      setting.meta.key,
    );
    final fate = read == null
        ? SaveFate.notReadBack
        : (sameFieldValue(setting.bytes, read.value)
              ? SaveFate.durable
              : SaveFate.different);
    results.add(PendingResult(setting, fate, labels[i]));
    if (fate == SaveFate.durable) durable.add(setting);
  }
  settings.resolve(durable);
  return SaveOutcome(results: results, saveAnswered: true);
}

/// What a panel says when it holds a write and the link is gone: the save cannot be sent, and the
/// next reset is what the change is measured against.
String unsavedOfflineMessage(List<String> names) =>
    'Not saved: ${unsavedNames(names)}. The link is down, so the save cannot be sent, and a reset '
    'brings the old value back.';

/// What a panel says after a write it has not saved: Save All is named as the one durable step.
String unsavedWriteNotice(String name) =>
    '$name written. Not saved yet: the device holds it in RAM only, and Save all makes it survive '
    'a reset.';

/// What the shell says when the link went away while the device held unsaved writes.
String unsavedLinkLostNotice(List<String> names) =>
    'The link dropped with unsaved changes: ${unsavedNames(names)}. The device has them in RAM '
    'only, so a reset brings the old values back.';

/// What the shell says when the link came back and the device still holds unsaved writes.
String unsavedLinkBackNotice(List<String> names) =>
    'The link is back and unsaved changes are still waiting: ${unsavedNames(names)}. Only Save all '
    'writes them to the device; a reset brings the old values back.';
