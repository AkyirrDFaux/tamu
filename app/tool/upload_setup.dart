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
    // a dry run is exactly the state where they are expected.
    if (plan.deviceTargets.isNotEmpty) {
      final differences = await _readBack(plan, 'READ-BACK (current state)');
      for (final difference in differences) {
        stdout.writeln('  difference: $difference');
      }
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
    stdout.writeln('  difference before: $difference');
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
  if (result.failed > 0 || mismatches.isNotEmpty) {
    stderr.writeln(
      '\nERROR: ${result.failed} failed write(s), '
      '${mismatches.length} read-back mismatch(es).',
    );
    return _exitFailed;
  }
  stdout.writeln('\nOK: the setup is on the rig and the read-back agrees.');
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

/// Reads each target device back and reports what it holds, returning the mismatches.
///
/// The device owns its persistence files, so this is a read of the live device (not of the
/// archive): the dynamic blocks the setup names, the subscription tables, the script states and
/// the file table. A requester entry whose provider half is missing would never send, so the two
/// halves are checked against each other.
Future<List<String>> _readBack(RestorePlan plan, String label) async {
  final mismatches = <String>[];
  final targets = <int, LiveDevice>{
    for (final target in plan.deviceTargets.values) target.id: target,
  };
  final requesterProviderNode = <int, int>{};
  final providerTrids = <int, Set<int>>{};

  stdout.writeln('\n$label');
  for (final target in targets.values) {
    final reg = RegisterClient(deviceId: target.id);
    final storage = StorageClient(deviceId: target.id);
    final scripts = ScriptClient(deviceId: target.id);
    final subs = SubscriptionClient(deviceId: target.id);

    final files = await storage.readFileTable();
    final blocks = await reg.readDynamicBlocks();
    final requester = await subs.getRequesterSubscriptions();
    final provider = await subs.getProviderSubscriptions();
    final states = <String>[];
    for (var slot = 0; slot < 12; slot++) {
      final state = await scripts.readState(slot);
      if (state == null) continue;
      states.add('$slot:state=${state.state}${state.hasError ? ' ERR' : ''}');
      if (state.hasError) {
        mismatches.add('${target.name}: script slot $slot is in error');
      }
    }
    providerTrids[idDevice(target.id)] = {for (final p in provider) p.trid};
    for (final r in requester) {
      requesterProviderNode[r.trid] = idDevice(r.providerAddr);
    }

    stdout.writeln(
      '  ${idToString(target.id)} "${target.name}" '
      'files=${files?.map((f) => '${f.name}(${f.size})').join(',') ?? 'no answer'}',
    );
    stdout.writeln(
      '    dynamic=${blocks?.map((b) => '${b.index}:${b.name}(f${b.fieldCount})').join(',') ?? 'no answer'}',
    );
    stdout.writeln(
      '    requester=${requester.map((r) => 'idx${r.index}/0x${r.trid.toRadixString(16)} '
          'provider=${idToString(r.providerAddr)}').join(', ')}',
    );
    stdout.writeln(
      '    provider=${provider.map((p) => '0x${p.trid.toRadixString(16)}').join(', ')}',
    );
    stdout.writeln('    scripts=[${states.join(', ')}]');

    // The archive's own dynamic blocks must exist on the target, or the setup's scenario has
    // nothing to run in.
    final liveNames = {for (final b in blocks ?? const <DynBlock>[]) b.name};
    for (final source in plan.devices) {
      if (plan.deviceTargets[source.id]?.id != target.id) continue;
      for (final block in source.blocks.where(
        (b) => isDynamicType(b.typeIndex),
      )) {
        if (liveNames.contains(block.name)) continue;
        mismatches.add(
          '${target.name}: dynamic block "${block.name}" is missing',
        );
      }
      // Files are reported, not gated: the device's own table decides names and layout.
      final names = {for (final f in files ?? const <FileRecord>[]) f.name};
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

  // Both halves of every subscription: a requester entry must have a provider entry with the
  // same TRID on the node its provider address names, or it points at a source that never sends.
  for (final entry in requesterProviderNode.entries) {
    final trids = providerTrids[entry.value] ?? const <int>{};
    if (trids.contains(entry.key)) continue;
    mismatches.add(
      'requester trid 0x${entry.key.toRadixString(16)} has no provider entry '
      'on node ${entry.value}',
    );
  }
  return mismatches;
}

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
