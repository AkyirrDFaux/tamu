/// Uploads a Tamu setup archive to the rig over the core's serial link.
///
///   dart run tool/upload_setup.dart `<setup.zip>` [--port `<tty>`] [--device `<selector>`]
///                                   [--dry-run] [--yes]
///
/// The archive is an ARGUMENT: any setup archive is accepted (the per-device JSON archives the
/// Backup page produces), whatever it is called and wherever it lives. It is read, its devices
/// are matched against the live rig by serial/address/name/block type, and a per-device plan is
/// printed. Nothing is written unless `--yes` is passed; `--dry-run` is the default and is safe
/// (it never writes, and with a rig it reads the devices back so the plan can be compared with
/// what the rig holds). A device whose live target is not the same device type as the archive's
/// is refused rather than written over. A write is bracketed by read-backs, so what the device
/// held before and after is both reported.
///
/// The read-back NAMES every difference - device, table, key, expected value, actual value - and
/// classifies it (see [ReadBackClass]): a row the restore wrote and the device does not hold is a
/// failure, an unreadable table is a failure, and the PROVIDER half of a subscription is reported
/// and explained once without failing the run, because no restore writes it (it is RAM-only on the
/// node and installed by the requester itself).
///
/// Exit codes: 0 ok; 2 bad usage or bad archive; 3 no rig / refused to write; 4 a write or the
/// read-back after it failed.
///
/// Run it from `app/` (package resolution) with the Flutter SDK's `dart` on PATH:
///
///   dart run tool/upload_setup.dart ~/setups/akyirr_setup.zip
///
/// It needs `libserialport.so`, which the app's Linux build leaves in
/// `build/linux/<arch>/debug/bundle/lib`; the tool loads it from there itself. A system
/// libserialport works too. `flutter build linux --debug` once if neither exists.
library;

import 'dart:convert';
import 'dart:io';

import 'package:archive/archive_io.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/device_identity.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/service_request.dart';
import 'package:tamuapp/core/sndb.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'rig_link.dart';

const int _exitUsage = 2;
const int _exitNoRig = 3;
const int _exitFailed = 4;

const String _usage = '''
Upload a Tamu setup archive to the rig.

  dart run tool/upload_setup.dart <setup.zip> [options]

  <setup.zip>            any setup archive (required; no default)
  --port <tty>           serial port of the core; default: the Espressif by-id device,
                         else the only enumerated port
  --device <selector>    upload only the archive devices matching a selector: an address
                         (0.2 or 2), a serial number, or part of a device name
  --dry-run              plan only, never write (this is the default)
  --yes                  write the plan to the rig
  -h, --help             this text

Nothing is written without --yes. Exit codes: 0 ok, 2 usage/archive, 3 no rig or refused,
4 write or read-back failure.
''';

/// How the CLI opens the link to the core. The default is the real serial channel
/// ([RigChannel.open]); a test substitutes a fake so the plan and the refusal paths run without a
/// rig.
typedef RigOpener = Future<RigLink> Function(String portName);

Future<void> main(List<String> args) async {
  // The VM ignores an int returned from an async main, so the code goes through `exitCode`
  // (which also lets stdout flush, unlike exit()).
  exitCode = await runUpload(args);
}

/// The CLI: returns the exit code, with the link opener injectable for tests.
Future<int> runUpload(List<String> args, {RigOpener? openRig}) async {
  final open = openRig ?? RigChannel.open;
  final options = _Options.parse(args);
  if (options == null) {
    stderr.writeln(_usage);
    return _exitUsage;
  }
  if (options.help) {
    stdout.writeln(_usage);
    return 0;
  }

  // --- The archive: an argument, validated before anything touches the rig. ---------------
  final path = options.archive!;
  final file = File(path);
  if (!file.existsSync()) {
    stderr.writeln('ERROR: setup archive not found: $path');
    return _exitUsage;
  }
  final bytes = file.readAsBytesSync();
  final problems = _archiveProblems(bytes);
  if (problems.isNotEmpty) {
    stderr.writeln('ERROR: $path is not a Tamu setup archive:');
    for (final problem in problems) {
      stderr.writeln('  - $problem');
    }
    return _exitUsage;
  }
  final List<BackupDevice> parsed;
  try {
    parsed = parseBackupZip(bytes);
  } catch (error) {
    stderr.writeln('ERROR: $path could not be read as a setup archive: $error');
    return _exitUsage;
  }
  final devices = options.selector == null
      ? parsed
      : parsed.where((d) => _matches(d, options.selector!)).toList();
  stdout.writeln('ARCHIVE $path: ${parsed.length} device(s)');
  for (final device in parsed) {
    stdout.writeln(
      '  ${device.address} ${device.type} name="${device.name}" '
      'serial=${device.serial ?? '-'} '
      'blocks=${device.blocks.length} scripts=${device.scripts.length} '
      'layouts=${device.layouts.length} '
      'subs=${device.requesterSubscriptions.length} '
      'sndb=${device.sndb.length} files=${device.files.length}'
      '${devices.contains(device) ? '' : '   (not selected)'}',
    );
  }
  if (devices.isEmpty) {
    stderr.writeln(
      'ERROR: --device ${options.selector} matches no device in the archive '
      '(addresses: ${parsed.map((d) => d.address).join(', ')}).',
    );
    return _exitUsage;
  }

  // --- The link: optional. Without a rig the plan is built against zero live devices, which is
  // a complete (and safe) dry run: every archive device is reported as having no target. -----
  RigLink? channel;
  final loadedLib = preloadLibserialport();
  final port = options.port ?? corePortCandidates().firstOrNull;
  var liveIdentities = <int, DeviceIdentity>{};
  if (port == null) {
    stdout.writeln(
      '\nNO RIG: no serial port found'
      '${loadedLib == null ? ' and no bundled libserialport.so' : ''}. '
      'The plan below is computed against zero live devices.',
    );
  } else {
    stdout.writeln(
      '\nLINK $port${loadedLib == null ? '' : ' (libserialport: $loadedLib)'}',
    );
    try {
      channel = await open(port);
      installRequestChannel(channel);
      final settled = await _awaitCore();
      if (!settled) {
        stdout.writeln('  core did not answer a ping: no live devices.');
      } else {
        liveIdentities = await _scanRig();
      }
    } catch (error) {
      stderr.writeln('  could not use $port: $error');
      await channel?.close();
      channel = null;
      installRequestChannel(null);
    }
  }

  // --- The plan. ---------------------------------------------------------------------------
  final plan = await buildRestorePlan(devices);
  final refusals = _reportPlan(plan, liveIdentities);

  if (!options.yes || options.dryRun) {
    // A dry run is complete: with a rig it also reads the devices back, so what the rig holds
    // now can be compared with what the archive asks for. Differences here are informational -
    // a dry run is exactly the state where they are expected - but they are still NAMED, one
    // line each, because a count alone cannot be acted on.
    if (plan.deviceTargets.isNotEmpty) {
      final differences = await _readBack(plan, 'READ-BACK (current state)');
      for (final difference in differences) {
        stdout.writeln('  difference: ${difference.line}');
      }
      final note = runtimeDifferenceNote(differences);
      if (note != null) stdout.writeln('  $note');
    }
    await channel?.close();
    stdout.writeln(
      options.dryRun && options.yes
          ? '\nDRY RUN (--dry-run wins over --yes): plan only, nothing written'
          : '\nDRY RUN: plan only, nothing written (pass --yes to write)',
    );
    return refusals.isEmpty ? 0 : _exitNoRig;
  }

  if (channel == null) {
    return _refuse(null, 'ERROR: refusing to write without a rig.');
  }
  if (refusals.isNotEmpty) {
    return _refuse(
      channel,
      'ERROR: refusing to write: ${refusals.length} archive device(s) are not restorable '
      'onto this rig (see above).',
    );
  }
  final withoutTarget = plan.devices
      .where((d) => plan.deviceTargets[d.id] == null)
      .toList();
  if (withoutTarget.isNotEmpty) {
    return _refuse(
      channel,
      'ERROR: refusing to write: no live target for '
      '${withoutTarget.map((d) => d.name).join(', ')}.',
    );
  }

  // --- The write, between a read-back before it and one after. ------------------------------
  final before = await _readBack(plan, 'READ-BACK (before the write)');
  for (final difference in before) {
    stdout.writeln('  difference before: ${difference.line}');
  }
  final result = await applyRestorePlan(plan);
  stdout.writeln(
    '\nAPPLIED written=${result.written} '
    'skipped=${result.skipped.length} failed=${result.failed}',
  );
  // The report's own count line, in the words the app's result screen copies: written, skipped as
  // structure, failed. A structural row is a dictionary marker or a row the archive carries with no
  // value - it is not a write that failed, and the restore never sends one.
  stdout.writeln('  REPORT ${result.counts}');
  for (final skip in result.skipped) {
    stdout.writeln('  SKIPPED-STRUCTURAL ${skip.line}');
  }
  for (final prune in result.requesterPrune) {
    stdout.writeln(
      '  REQUESTER ${idToString(prune.deviceId)} "${prune.deviceName}": '
      'before=[${prune.beforeText}] cancelled=[${prune.cancelledText}] '
      'remaining=[${prune.remainingText}]'
      '${prune.skipped ? ' SKIPPED: ${prune.reason}' : ''}',
    );
  }
  for (final prune in result.scriptSlots) {
    stdout.writeln(
      '  SCRIPTS ${idToString(prune.deviceId)} "${prune.deviceName}": '
      'before=[${prune.beforeText}] unloaded=[${prune.unloadedText}] '
      'duplicates=[${prune.duplicatesText}] off-slot=[${prune.offSlotText}] '
      'refused=[${prune.refusedText}] kept-by-selection=[${prune.keptText}] '
      'remaining=[${prune.remainingText}]'
      '${prune.skipped ? ' SKIPPED: ${prune.reason}' : ''}',
    );
  }
  for (final reload in result.scriptReload) {
    stdout.writeln(
      '  RELOAD ${idToString(reload.deviceId)} "${reload.deviceName}": '
      'changed=[${reload.changedText}] reloaded=[${reload.reloadedText}] '
      'left-alone=[${reload.leftAloneText}] refused=[${reload.refusedText}]'
      '${reload.skipped ? ' SKIPPED: ${reload.reason}' : ''}',
    );
  }
  for (final failure in result.failures) {
    stdout.writeln('  FAILED-ITEM $failure');
  }
  final mismatches = await _readBack(plan, 'READ-BACK (after the write)');
  await channel.close();
  // Every difference gets its own line - device, table, key, expected, actual, class. The count
  // alone was the defect: `6 read-back mismatch(es)` named none of them (t_d02e2f2d).
  for (final mismatch in mismatches) {
    stdout.writeln('  MISMATCH ${mismatch.line}');
  }
  final note = runtimeDifferenceNote(mismatches);
  if (note != null) stdout.writeln('  $note');
  // Only a difference the restore should have settled fails the run. The provider half of a
  // subscription is not one of those: no archive carries it and no restore writes it.
  final failedReadBack = mismatches.where((m) => m.kind.fails).toList();
  if (result.failed > 0 || failedReadBack.isNotEmpty) {
    stderr.writeln(
      '\nERROR: ${result.failed} failed write(s), '
      '${failedReadBack.length} read-back mismatch(es)'
      '${failedReadBack.length == mismatches.length ? '' : ' '
                '(${mismatches.length - failedReadBack.length} runtime provider '
                'difference(s) are named above and did not fail the run)'}.',
    );
    for (final mismatch in failedReadBack) {
      stderr.writeln('  MISMATCH ${mismatch.line}');
    }
    return _exitFailed;
  }
  if (mismatches.isEmpty) {
    stdout.writeln('\nOK: the setup is on the rig and the read-back agrees.');
  } else {
    stdout.writeln(
      '\nOK: the setup is on the rig; every row this restore writes read back. '
      'The ${mismatches.length} remaining difference(s) are runtime provider state '
      '(see the NOTE above).',
    );
  }
  return 0;
}

/// Prints [message] on stderr, closes the link and returns the "no rig / refused" exit code.
///
/// Every refusal comes through here for a reason: an open serial port keeps its readers alive and
/// with them the whole isolate, so a refusal that just returned never let the process exit -
/// measured on 2026-10-21, the `--yes` run died on a 400 s timeout (exit 124) while the dry run on
/// the same port exited at once.
Future<int> _refuse(RigLink? channel, String message) async {
  stderr.writeln(message);
  await channel?.close();
  return _exitNoRig;
}

/// Waits for the core to answer a ping (opening the port resets it).
Future<bool> _awaitCore() async {
  for (var attempt = 0; attempt < 20; attempt++) {
    final reply = await serviceRequest(
      coreId,
      ServiceType.device,
      1,
      timeout: const Duration(milliseconds: 800),
      tag: 'ping',
    );
    if (reply != null) return true;
    await Future<void>.delayed(const Duration(milliseconds: 250));
  }
  return false;
}

/// Reads the identity of every device on the bus (the core and every node its SNDB names).
Future<Map<int, DeviceIdentity>> _scanRig() async {
  final ids = <int>{coreId};
  for (final entry in await sndbEntries()) {
    if (entry.id != 0) ids.add(entry.id);
  }
  final sorted = ids.toList()..sort();
  final identities = <int, DeviceIdentity>{};
  stdout.writeln('  rig:');
  for (final id in sorted) {
    // This identity decides the per-device TYPE refusal, so a lost reply must not read as the
    // wrong device type.
    final identity = await readDeviceIdentity(
      id,
      readAttempts: RegisterClient.snapshotReadAttempts,
    );
    if (identity == null) {
      stdout.writeln('    ${idToString(id)}: no answer');
      continue;
    }
    identities[id] = identity;
    stdout.writeln(
      '    ${idToString(id)} ${identity.type.label} name="${identity.name}" '
      'serial=${identity.serial ?? '-'} '
      'caps=0x${identity.capabilities.toRadixString(16)}',
    );
  }
  return identities;
}

/// Prints the per-device plan and returns the archive devices that must not be written.
///
/// A device is refused when its live target is a different device TYPE (the archive's own type
/// word against the live System Device Type) or when the target does not carry a block type the
/// archive's device has: writing either would put one device's setup on another kind of device.
///
/// A block type the target did not ANSWER for is a third case, and not a shape mismatch: the run
/// says the read failed and leaves "the target carries no `<Type>` block" for a device that really
/// lacks it (see `RegisterClient.snapshotReadAttempts`).
List<String> _reportPlan(
  RestorePlan plan,
  Map<int, DeviceIdentity> liveIdentities,
) {
  final refusals = <String>[];
  stdout.writeln('\nPLAN');
  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    final items = plan.items.where((i) => i.device.id == source.id).toList();
    final selected = items.where((i) => i.selected && i.ready).length;
    final skipped = items.where((i) => !i.ready).toList();
    final skippedWrites = skipped.where((i) => i.issue != readOnlyIssue).length;
    final basis = target == null ? null : _matchBasis(source, target);
    stdout.writeln(
      '  ${source.address} ${source.type} "${source.name}" '
      'serial=${source.serial ?? '-'} -> '
      '${target == null ? 'NO TARGET' : '${idToString(target.id)} "${target.name}" '
                '(live serial ${target.serial ?? '-'}, matched by $basis)'}'
      '  items=$selected/${items.length} to write, $skippedWrites unresolved',
    );
    if (target == null) {
      // No snapshot is not the same as no device: a node that answered its identity but not its
      // block list is UNREAD, and saying so is the whole difference between "check the link"
      // and "this is the wrong rig for the archive". The reason is reached by IDENTITY, not only
      // by address: an archive captured before the device was re-addressed names it at the old
      // address, and the live node that answered is still the same device (see
      // `RestorePlan.unreadReasonFor`).
      final unread = plan.unreadReasonFor(source);
      if (unread != null) {
        refusals.add(source.name);
        stdout.writeln('    REFUSED: $unread - unread, not absent');
      }
      continue;
    }

    final live = liveIdentities[target.id];
    final sourceType = DeviceType.fromValue(source.typeId);
    if (live != null && live.type != sourceType) {
      refusals.add(source.name);
      stdout.writeln(
        '    REFUSED: archive device type ${sourceType.label} (${source.typeId}) but the '
        'live target reports ${live.type.label} (${live.typeValue})',
      );
      continue;
    }
    // A source block type the live snapshot does not carry is one of two very different things:
    // the target really has no such block (a wrong-device-type refusal, which must stay), or the
    // walk lost that block's metadata read. The client reports the second as UNREAD, so the two
    // are never confused - on 2026-10-22 a healthy rig was refused as "the target carries no
    // Button block" by a single lost reply.
    final targetTypes = {for (final b in target.blocks) b.type};
    // Ordered sets: a type the archive carries in more than one instance (the LED Display sits at
    // instances 0 and 1 in `akyirr_setup.zip`) is one block type, so it is named once - "carries
    // no LED Display block", never "no LED Display, LED Display block".
    final absent = <String>{};
    final unreadTypes = <String>{};
    for (final b in source.blocks) {
      if (isDynamicType(b.typeIndex) ||
          isScriptType(b.typeIndex) ||
          b.typeIndex == systemBlockTypeValue ||
          targetTypes.contains(b.typeIndex)) {
        continue;
      }
      (target.unreadTypes.contains(b.typeIndex) ? unreadTypes : absent).add(
        blockTypeDisplayName(b.typeIndex),
      );
    }
    var refused = false;
    if (unreadTypes.isNotEmpty) {
      refusals.add(source.name);
      refused = true;
      stdout.writeln(
        '    REFUSED: the target did not answer the ${unreadTypes.join(', ')} block read '
        '(${RegisterClient.snapshotReadAttempts} attempts) - unread, not absent',
      );
    }
    if (absent.isNotEmpty) {
      // A refusal stops the per-device report here, exactly as it did when there was one case:
      // the "not ready" lines describe a device that is about to be written, and this one is not.
      if (!refused) refusals.add(source.name);
      refused = true;
      stdout.writeln(
        '    REFUSED: the target carries no ${absent.join(', ')} block',
      );
    }
    if (refused) continue;
    for (final item in skipped) {
      if (item.issue == readOnlyIssue) continue;
      stdout.writeln(
        '    not ready: ${item.kind.name} ${_describe(item)} -> ${item.issue}',
      );
    }
  }
  return refusals;
}

/// Why [target] was matched to [source] - the same order `matchLiveDevice` uses.
String _matchBasis(BackupDevice source, LiveDevice target) {
  if (source.serial != null && target.serial == source.serial) return 'serial';
  if (target.id == source.id) return 'address';
  if (target.name.toLowerCase() == source.name.toLowerCase()) return 'name';
  return 'block type';
}

bool _matches(BackupDevice device, String selector) {
  final wanted = selector.toLowerCase();
  final address = device.address;
  final node = idDevice(device.id).toString();
  if (wanted == address || wanted == node) return true;
  if (device.serial?.toLowerCase() == wanted) return true;
  return device.name.toLowerCase().contains(wanted);
}

String _describe(RestoreItem item) => switch (item.kind) {
  RestoreKind.entry =>
    '${item.block?.name}/${item.entry?.field}[${item.entry?.key}]',
  RestoreKind.script => 'script slot ${item.script?.slot}',
  RestoreKind.subscription => 'from ${item.subscription?.provider}',
  RestoreKind.sndb => 'sndb ${item.sndbEntry?.serial}',
  RestoreKind.file => 'file ${item.file?.name}',
};

/// What a read-back difference IS, and therefore whether the run failed.
///
/// A read-back used to be a count - `6 read-back mismatch(es)` with no line naming them (card
/// t_d02e2f2d, 2026-11-06) - which the owner could not act on. Each difference now carries its
/// device, table, key, the value the archive asks for and the value the device holds; the class
/// is what decides the exit code.
enum ReadBackClass {
  /// The device does not hold what this restore wrote (or was to write): a real failure.
  notApplied('not applied'),

  /// The read did not complete, so nothing can be said about the value. Also a failure: a
  /// read-back that could not look must never read as agreement.
  unreadable('unreadable'),

  /// A difference no restore writes: the PROVIDER half of a subscription is RAM-only on the node
  /// ("active until cancelled; the information is not persistent on this side",
  /// `Docs/Services/Subscriptions.md` "Provider"). It is installed by the REQUESTER from its own
  /// requester rows, with a 120 s lease the node drops by itself (`Issues.md` E46/E47), so no
  /// restore writes it and a read-back taken right after one cannot see the installs that come
  /// later - reported, explained once, never a failed write.
  runtime('runtime state, not written by a restore');

  final String label;
  const ReadBackClass(this.label);

  /// True when the line means this restore did not land.
  bool get fails => this != ReadBackClass.runtime;
}

/// One line of a read-back: device, table, key, expected, actual, and the class above.
class ReadBackMismatch {
  /// The live device the line is about, e.g. `0.1 "Akyirr"`.
  final String device;

  /// The table or block state was read from, e.g. `subscriptions/provider`.
  final String where;

  /// The row's identity inside that table: a TRID, or the registers the row names.
  final String key;

  /// What the archive asks for, in the device's own words.
  final String expected;

  /// What the device holds instead.
  final String actual;

  final ReadBackClass kind;

  const ReadBackMismatch({
    required this.device,
    required this.where,
    required this.key,
    required this.expected,
    required this.actual,
    required this.kind,
  });

  /// The one line the owner asked for: device, table, key, expected, actual, class.
  String get line =>
      '$device $where $key: expected $expected, actual $actual [${kind.label}]';

  /// True when this difference means the restore did not land (see [ReadBackClass.fails]).
  bool get fails => kind.fails;
}

/// What one live device answered to a read-back: the tables a restore's rows live in. A null
/// table is a read that did not answer, which is NOT an empty table.
class ReadBackSnapshot {
  final int deviceId;
  final List<DynBlock>? blocks;
  final List<RequesterSubscription>? requesters;
  final List<ProviderSubscription>? providers;
  final List<({int slot, int state, bool error})> scriptStates;

  const ReadBackSnapshot({
    required this.deviceId,
    required this.blocks,
    required this.requesters,
    required this.providers,
    this.scriptStates = const [],
  });
}

/// Reads each target device back, prints what it holds, and returns its differences.
///
/// The device owns its persistence files, so this is a read of the live device (not of the
/// archive): the dynamic blocks the setup names, the subscription tables, the script states and
/// the file table.
Future<List<ReadBackMismatch>> _readBack(RestorePlan plan, String label) async {
  final targets = <int, LiveDevice>{
    for (final target in plan.deviceTargets.values) target.id: target,
  };
  final snapshots = <ReadBackSnapshot>[];

  stdout.writeln('\n$label');
  for (final target in targets.values) {
    final device = _deviceLabel(target);
    final reg = RegisterClient(deviceId: target.id);
    final storage = StorageClient(deviceId: target.id);
    final scripts = ScriptClient(deviceId: target.id);
    final subs = SubscriptionClient(deviceId: target.id);

    final files = await storage.readFileTable();
    final blocks = await reg.readDynamicBlocks();
    // A table read that did not answer is not an empty table: the plain getters fold the two
    // together, and a read-back taken that way reported six mismatches against a table the run
    // never saw. Three sends, as the app's own read policy does (`StorageClient._readWhole`).
    // A device that cannot hold the table answers FAIL by design, so it is not read at all and
    // reports the capability instead of a failed read.
    final canRequest =
        target.capabilities & Capability.subscriptionRequest != 0;
    final canProvide =
        target.capabilities & Capability.subscriptionProvide != 0;
    final requester = canRequest
        ? await _tableRead(subs.readRequesterSubscriptions)
        : const <RequesterSubscription>[];
    final provider = canProvide
        ? await _tableRead(subs.readProviderSubscriptions)
        : const <ProviderSubscription>[];

    final states = <({int slot, int state, bool error})>[];
    for (var slot = 0; slot < 12; slot++) {
      final state = await scripts.readState(slot);
      if (state == null) continue;
      states.add((slot: slot, state: state.state, error: state.hasError));
    }
    snapshots.add(
      ReadBackSnapshot(
        deviceId: target.id,
        blocks: blocks,
        requesters: requester,
        providers: provider,
        scriptStates: states,
      ),
    );

    stdout.writeln(
      '  $device '
      'files=${files?.map((f) => '${f.name}(${f.size})').join(',') ?? 'no answer'}',
    );
    stdout.writeln(
      '    dynamic=${blocks?.map((b) => '${b.index}:${b.name}(f${b.fieldCount})').join(',') ?? 'no answer'}',
    );
    stdout.writeln(
      '    requester=${!canRequest ? '(no requester table: the device cannot request)' : requester == null ? 'no answer' : requester.map((r) => 'idx${r.index}/0x${r.trid.toRadixString(16)} '
          'provider=${idToString(r.providerAddr)}').join(', ')}',
    );
    stdout.writeln(
      '    provider=${!canProvide ? '(no provider table: the device cannot provide)' : provider == null ? 'no answer' : provider.map((p) => '0x${p.trid.toRadixString(16)}').join(', ')}',
    );
    stdout.writeln(
      '    scripts=[${states.map((s) => '${s.slot}:state=${s.state}${s.error ? ' ERR' : ''}').join(', ')}]',
    );

    // Files are reported, not gated: the device's own table decides names and layout.
    final names = {for (final f in files ?? const <FileRecord>[]) f.name};
    for (final source in plan.devices) {
      if (plan.deviceTargets[source.id]?.id != target.id) continue;
      final missing = [
        for (final f in source.files)
          if (!f.name.startsWith('.') && !names.contains(f.name)) f.name,
      ];
      if (missing.isNotEmpty) {
        stdout.writeln(
          '    files not in the table (informational): ${missing.join(', ')}',
        );
      }
    }
  }

  return readBackMismatches(plan, snapshots);
}

/// The differences between what [plan] wrote and what [snapshots] say the devices hold.
///
/// Split from the reads so the classification can be driven without a rig (`planFromSnapshot`
/// builds the plan half): the tool's own output is only as good as these lines.
List<ReadBackMismatch> readBackMismatches(
  RestorePlan plan,
  List<ReadBackSnapshot> snapshots,
) {
  final mismatches = <ReadBackMismatch>[];
  for (final snapshot in snapshots) {
    final target =
        plan.deviceTargets[snapshot.deviceId] ??
        plan.liveDevices.where((d) => d.id == snapshot.deviceId).firstOrNull;
    if (target == null) continue;
    final device = _deviceLabel(target);

    // A script the restore loaded and the device reports as faulted did not land.
    for (final state in snapshot.scriptStates) {
      if (!state.error) continue;
      mismatches.add(
        ReadBackMismatch(
          device: device,
          where: 'scripts/slot ${state.slot}',
          key: 'state',
          expected: 'a script that runs without an error',
          actual: 'the slot is in error (state ${state.state})',
          kind: ReadBackClass.notApplied,
        ),
      );
    }

    // The archive's own dynamic blocks must exist on the target, or the setup's scenario has
    // nothing to run in.
    final liveNames = {for (final b in snapshot.blocks ?? const <DynBlock>[]) b.name};
    for (final source in plan.devices) {
      if (plan.deviceTargets[source.id]?.id != target.id) continue;
      for (final block in source.blocks.where(
        (b) => isDynamicType(b.typeIndex),
      )) {
        if (liveNames.contains(block.name)) continue;
        mismatches.add(
          ReadBackMismatch(
            device: device,
            where: 'dynamic/${block.name}',
            key: 'block',
            expected: 'the archive\'s block "${block.name}"',
            actual: 'the device\'s dynamic block list does not name it',
            kind: ReadBackClass.notApplied,
          ),
        );
      }
    }

    // The requester rows this restore writes, checked row for row. A TRID is allocated by the
    // restore and the archive names none, so a row is matched on its identity - the provider's
    // NODE, the source register and the target register ([requesterKey]) - and not on its
    // address: the core re-qualifies a stored provider address to its current net at every use
    // (`SubscriptionsRequester.h`, RequesterProviderAddr), so a row set under another net is the
    // same row.
    for (final item in plan.items.where(
      (i) => i.kind == RestoreKind.subscription && i.targetDevice?.id == target.id,
    )) {
      final archived = item.subscription!;
      final identity = plan.archiveRequesterIdentity(item);
      if (identity == null) continue; // an unresolved ref: the plan already says why
      final wanted =
          '${_timingLabel(TriggerType.fromWord(archived.trigger) ?? TriggerType.none, archived.minTimeMs, archived.periodMs, archived.deadzone)} '
          '${_refLabel(archived.source)} -> '
          '${_refLabel(archived.target)} on ${_nodeLabel(plan, archived.provider)}';
      final key =
          'entry ${_refLabel(archived.source)} -> ${_refLabel(archived.target)}';
      final requesters = snapshot.requesters;
      if (requesters == null) {
        mismatches.add(
          ReadBackMismatch(
            device: device,
            where: 'subscriptions/requester',
            key: key,
            expected: wanted,
            actual: 'the requester table did not answer (3 sends)',
            kind: ReadBackClass.unreadable,
          ),
        );
        continue;
      }
      final live = requesters
          .where(
            (r) =>
                requesterKey(r.providerAddr, r.sourceReg, r.targetReg) == identity,
          )
          .firstOrNull;
      if (live == null) {
        mismatches.add(
          ReadBackMismatch(
            device: device,
            where: 'subscriptions/requester',
            key: key,
            expected: wanted,
            actual:
                'the table holds no entry with this source and target '
                '(${_count(requesters.length, 'row')}: '
                '${requesters.map((r) => '0x${r.trid.toRadixString(16)}').join(', ')})',
            kind: ReadBackClass.notApplied,
          ),
        );
        continue;
      }
      if (sameSubscription(archived, live)) continue;
      mismatches.add(
        ReadBackMismatch(
          device: device,
          where: 'subscriptions/requester',
          key: key,
          expected: wanted,
          actual:
              '${_timingLabel(live.trigger, live.minTimeMs, live.periodMs, live.deadzone)} '
              '(trid 0x${live.trid.toRadixString(16)})',
          kind: ReadBackClass.notApplied,
        ),
      );
    }
  }

  // The OTHER half of every requester row: a provider entry with the same TRID on the node its
  // provider address names. This is runtime state, not a restore item (see [ReadBackClass.runtime]),
  // so it is named and explained - never counted as a failed write.
  final tables = {for (final s in snapshots) s.deviceId: s};
  for (final snapshot in snapshots) {
    final requesters = snapshot.requesters;
    if (requesters == null) continue; // already reported as unreadable per entry
    final target = plan.deviceTargets[snapshot.deviceId];
    final device = target == null ? idToString(snapshot.deviceId) : _deviceLabel(target);
    for (final row in requesters) {
      final providerNode = idDevice(row.providerAddr);
      final providerName = _nodeLabel(plan, idToString(row.providerAddr));
      final trid = '0x${row.trid.toRadixString(16)}';
      final providerTable = tables[providerNode]?.providers;
      if (!tables.containsKey(providerNode)) {
        // The provider is not a target of this run (a `--device` selection): its table was never
        // read, which is a fact about the selection, not about the restore.
        mismatches.add(
          ReadBackMismatch(
            device: device,
            where: 'subscriptions/provider',
            key: 'entry $trid',
            expected: 'a provider entry for $trid on $providerName',
            actual: 'not read: $providerName is not a target of this run',
            kind: ReadBackClass.runtime,
          ),
        );
        continue;
      }
      if (providerTable == null) {
        mismatches.add(
          ReadBackMismatch(
            device: device,
            where: 'subscriptions/provider',
            key: 'entry $trid',
            expected: 'a provider entry for $trid on $providerName',
            actual: 'the provider table did not answer (3 sends)',
            kind: ReadBackClass.unreadable,
          ),
        );
        continue;
      }
      if (providerTable.any((p) => p.trid == row.trid)) continue;
      mismatches.add(
        ReadBackMismatch(
          device: device,
          where: 'subscriptions/provider',
          key: 'entry $trid',
          expected:
              'a provider entry for $trid '
              '(${_registerLabel(row.sourceReg)}, ${row.trigger.label}) on '
              '$providerName, installed by the requester',
          actual:
              '$providerName holds '
              '${providerTable.isEmpty ? 'no entry' : '${_count(providerTable.length, 'entry')} '
                        '(${providerTable.map((p) => '0x${p.trid.toRadixString(16)}').join(', ')})'}, '
              'none for $trid',
          kind: ReadBackClass.runtime,
        ),
      );
    }
  }
  return mismatches;
}

/// Says once, in the tool's own output, why the remaining differences are not failed writes.
///
/// The provider half of a subscription is RAM-only on the node, so no archive carries it and no
/// restore writes it: the requester installs it from its own requester rows on its own tick, with
/// a 120 s lease the node drops by itself. A read-back taken right after a restore therefore sees
/// an empty provider table, and without this note the count `6 read-back mismatch(es)` reads as six
/// failed writes (card t_d02e2f2d, 2026-11-06). Saying it once is what keeps the next restore from
/// re-opening it.
///
/// What the note must NOT say is that the subscription is live: a read-back cannot see the installs
/// the requester makes after it returns, so the line is about responsibility (this is not a write
/// the restore failed to make), not about the subscription working.
///
/// Returns null when there is nothing to say: no differences, or a difference that is not this
/// class - those are named on their own lines and must not be excused by a note.
String? runtimeDifferenceNote(List<ReadBackMismatch> mismatches) {
  if (mismatches.isEmpty) return null;
  if (mismatches.any((m) => m.kind != ReadBackClass.runtime)) return null;
  return 'NOTE: all ${mismatches.length} difference(s) are the PROVIDER half of a '
      'subscription. That table is RAM-only on the node ("active until cancelled; the '
      'information is not persistent on this side", Docs/Services/Subscriptions.md "Provider"): '
      'no archive carries it and no restore writes it - the requester (the core) installs it from '
      'its own requester rows on its own tick (CID 0x01 Change, 120 s lease the node drops by '
      'itself - Issues.md E46/E47). So this is not a failed write, and it is not a claim that the '
      'subscription is already delivering either: a read-back cannot see the installs the '
      'requester makes after it. The requester table - the half a restore DOES write - read back '
      'row for row.';
}

/// Reads a subscription table, re-sending a read that did not answer.
///
/// One table read is one forwarded request and this rig loses a few percent of those (E76), so a
/// single send would turn a lost reply into "the device holds no entry" - a lie about the restore.
/// Three sends, 200 ms then 600 ms apart, is the app's own read policy
/// (`StorageClient._readWhole`).
Future<List<T>?> _tableRead<T>(Future<List<T>?> Function() read) async {
  for (var attempt = 0; attempt < 3; attempt++) {
    final table = await read();
    if (table != null) return table;
    if (attempt == 2) break;
    await Future<void>.delayed(
      Duration(milliseconds: attempt == 0 ? 200 : 600),
    );
  }
  return null;
}

/// A live device as one line names it: `0.1 "Akyirr"`.
String _deviceLabel(LiveDevice device) =>
    '${idToString(device.id)} "${device.name}"';

/// The live device an archive address names, as `0.3 "Sensors B"`, or the bare address when the
/// rig has no such node. Matched by NODE: an archive's address carries the net it was captured
/// under, and the firmware re-qualifies the net at every use.
String _nodeLabel(RestorePlan plan, String address) {
  final id = idFromString(address);
  if (id == null) return address;
  final node = plan.liveDevices
      .where((d) => idDevice(d.id) == idDevice(id))
      .firstOrNull;
  return node == null ? address : _deviceLabel(node);
}

/// A subscription's register reference, as the archive names it: `Resistive measurement[0]/Measured Value[0]`.
String _refLabel(BackupBlockRef ref) =>
    '${ref.block}[${ref.instance}]/${ref.field}[${ref.keyIndex}]';

/// A subscription's trigger and timing, as one line names it:
/// `Periodic (1000 ms period, 100 ms min, deadzone 0.0)`.
String _timingLabel(
  TriggerType trigger,
  int minTimeMs,
  int periodMs,
  double deadzone,
) =>
    '${trigger.label} ($periodMs ms period, $minTimeMs ms min, '
    'deadzone $deadzone)';

/// A BlockInfo as one line names it: `Resistive measurement[1]/field 6[0]`.
String _registerLabel(int reg) =>
    '${blockTypeDisplayName((reg >> 22) & 0x3FF)}[${(reg >> 16) & 0x3F}]/'
    'field ${(reg >> 8) & 0xFF}[${reg & 0xFF}]';

String _count(int n, String noun) => '$n $noun${n == 1 ? '' : 's'}';


/// The problems that make [bytes] not a setup archive (empty = it is one).
///
/// A setup archive is one JSON per device, each carrying the identity keys a restore reads. The
/// check is explicit so a foreign zip (or a setup file that lost its keys) is refused with a
/// reason instead of being decoded into an empty device and reported as "nothing to write".
List<String> _archiveProblems(List<int> bytes) {
  // A zip always starts with a local file header (or, for an empty zip, the end-of-central
  // directory); anything else is not a zip however it is named.
  final header = bytes.length >= 4 ? bytes.sublist(0, 4) : const <int>[];
  final isZip =
      (header.length == 4 &&
          header[0] == 0x50 && // P
          header[1] == 0x4B && // K
          header[2] == 0x03 &&
          header[3] == 0x04) ||
      (bytes.length >= 4 &&
          bytes[0] == 0x50 &&
          bytes[1] == 0x4B &&
          bytes[2] == 0x05 &&
          bytes[3] == 0x06);
  if (!isZip) return ['not a zip archive (no PK header)'];
  final Archive archive;
  try {
    archive = ZipDecoder().decodeBytes(bytes);
  } catch (error) {
    return ['not a zip archive: $error'];
  }
  final problems = <String>[];
  var jsonFiles = 0;
  for (final file in archive.files) {
    if (file.isFile == false) continue;
    if (!file.name.toLowerCase().endsWith('.json')) continue;
    jsonFiles++;
    final Object? decoded;
    try {
      decoded = jsonDecode(utf8.decode(file.content as List<int>));
    } catch (error) {
      problems.add('${file.name}: not readable JSON ($error)');
      continue;
    }
    if (decoded is! Map<String, dynamic>) {
      problems.add('${file.name}: not a JSON object');
      continue;
    }
    final missing = <String>[
      if (decoded['app'] != 'tamuapp') 'app',
      if (decoded['type'] is! String) 'type',
      if (decoded['typeId'] is! num) 'typeId',
      if (decoded['id'] is! num) 'id',
      if (decoded['name'] is! String) 'name',
      if (decoded['blocks'] is! List) 'blocks',
    ];
    if (missing.isNotEmpty) {
      problems.add('${file.name}: missing ${missing.join(', ')}');
    }
  }
  if (jsonFiles == 0) problems.add('no per-device JSON files');
  return problems;
}

/// The option list, parsed by hand (the tool takes no package dependency for it).
class _Options {
  final String? archive;
  final String? port;
  final String? selector;
  final bool dryRun;
  final bool yes;
  final bool help;

  const _Options({
    this.archive,
    this.port,
    this.selector,
    this.dryRun = false,
    this.yes = false,
    this.help = false,
  });

  /// Null when the arguments do not make sense (the caller prints the usage).
  static _Options? parse(List<String> args) {
    String? archive;
    String? port;
    String? selector;
    var dryRun = false;
    var yes = false;
    var help = false;
    for (var i = 0; i < args.length; i++) {
      final arg = args[i];
      String next(String name) {
        if (i + 1 >= args.length) throw FormatException('$name needs a value');
        return args[++i];
      }

      try {
        switch (arg) {
          case '-h' || '--help':
            help = true;
          case '--dry-run':
            dryRun = true;
          case '--yes':
            yes = true;
          case '--port':
            port = next(arg);
          case '--device':
            selector = next(arg);
          default:
            if (arg.startsWith('-')) {
              stderr.writeln('ERROR: unknown option $arg');
              return null;
            }
            if (archive != null) {
              stderr.writeln('ERROR: more than one setup archive given');
              return null;
            }
            archive = arg;
        }
      } on FormatException catch (error) {
        stderr.writeln('ERROR: $error');
        return null;
      }
    }
    if (help) return const _Options(help: true);
    if (archive == null) {
      stderr.writeln('ERROR: no setup archive given');
      return null;
    }
    return _Options(
      archive: archive,
      port: port,
      selector: selector,
      dryRun: dryRun,
      yes: yes,
    );
  }
}
