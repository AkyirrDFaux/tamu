part of 'backup.dart';

/// The item kinds a restore report names, in the card's vocabulary: a block field, a script, a
/// script file, a requester row, a layout, a file.
///
/// [RestoreKind] is the plan's coarse split - a script FILE, a layout and any other storage file all
/// travel as [RestoreKind.file] - because the plan writes them through one path. A report has to do
/// better: "file failed" on a rig where the row was `SCR_004` tells the reader nothing about what to
/// look at. The archive's own file kind and the `SCR_XXX` name decide it, so this agrees with what
/// the capture put in the archive.
String restoreItemKindWord(RestoreItem item) => switch (item.kind) {
  RestoreKind.entry => 'block field',
  RestoreKind.script => 'script',
  RestoreKind.subscription => 'requester row',
  RestoreKind.sndb => 'SNDB record',
  RestoreKind.file => _fileKindWord(item.file!),
};

String _fileKindWord(BackupFile file) {
  if (file.kind == 'LED layout') return 'layout';
  if (scriptFileId(file.name) != null) return 'script file';
  return 'file';
}

/// The row as the PLAN names it: the archive's own field/key words, the script and the slot it
/// occupies, the two register addresses of a requester row, the name of a file.
///
/// This is the same wording the plan page prints for the row, so a reason quoted on a card points
/// at a row the owner can find on screen.
String restoreItemSubject(RestoreItem item) => switch (item.kind) {
  RestoreKind.entry =>
    '${item.block!.type} "${item.block!.name}" #${item.block!.instance} / '
        '${item.entry!.field} [${item.entry!.key}] '
        '(field ${item.entry!.fieldIndex} key ${item.entry!.keyIndex})',
  RestoreKind.script =>
    '${item.script!.functionName.isEmpty ? 'Script' : item.script!.functionName} '
        '(${scriptFileName(item.script!.slot)}'
        '${item.script!.slot < 0 ? ', no slot' : ', slot ${item.script!.slot}'})',
  RestoreKind.subscription =>
    'from ${item.subscription!.provider} '
        '(${item.subscription!.source.block} / ${item.subscription!.source.field}) '
        'into ${item.subscription!.target.block} / '
        '${item.subscription!.target.field}',
  RestoreKind.sndb =>
    'serial ${item.sndbEntry!.serial} at ${item.sndbEntry!.address}',
  RestoreKind.file =>
    '${item.file!.name} (${item.file!.kind}, ${item.file!.bytes.length} B'
        '${scriptFileId(item.file!.name) == null ? '' : ', slot ${scriptFileId(item.file!.name)}'})',
};

/// The code a failed request came back with, in the words a card can quote - or the explicit
/// statement that the device gave none.
///
/// The device sets `FLAG_FAIL` and, per the packet format, may carry a reason payload after it;
/// this firmware sends none (`RespondStatus(Frame, false)`), so the honest answer for a refusal is
/// "FAIL, no reason payload" rather than a numeric code invented by the app.
///
/// A failure the app decided on its own has no device code at all, and says so: a blank would read
/// as "the device said nothing useful" when in fact nothing was ever sent.
String restoreCodeText({RequestFault? fault, String? clientReason}) {
  final f = fault;
  if (f != null) {
    switch (f.kind) {
      case RequestFaultKind.noLink:
        return 'no code from the device (the app has no link to the bus)';
      case RequestFaultKind.noReply:
        return 'no code from the device (no reply'
            '${f.detail == null ? '' : ": ${f.detail}"})';
      case RequestFaultKind.refused:
        final reason = f.payload.isEmpty
            ? ''
            : ', reason 0x${f.payload.map((b) => b.toRadixString(16).padLeft(2, '0')).join()}';
        return 'FAIL (FLAG_FAIL 0x${RequestFault.failFlag.toRadixString(16)}'
            '$reason${f.payload.isEmpty ? ', no reason payload' : ''})';
      case RequestFaultKind.unusable:
        return 'no code from the device (it answered ${f.payload.length} byte(s) '
            'where a successful write answers differently'
            '${f.detail == null ? '' : ': ${f.detail}'})';
    }
  }
  if (clientReason != null) {
    return 'no code from the device (the app refused it: $clientReason)';
  }
  // A write the client reported as failed without recording why: say that instead of leaving it
  // blank.
  return 'no code from the device (it was not recorded by the request layer)';
}

/// One failed restore item, with the reason the bare count never carried.
///
/// The card Akyirr filed is the specification: the device and target, the item kind (block field /
/// script / script file / requester row / layout / file), the path or register as the plan names
/// it, the error code the device returned, and one sentence saying what that means. A count is not
/// a report; this is the record behind every one of them.
///
/// Successes are not recorded here at all: the report's job is the failures, and a run of 900
/// written rows must not bury the 17 that did not land.
class RestoreFailure {
  /// The archive device the row belongs to - the section the plan page shows it under.
  final String archive;

  /// The live node the row was being written to, as the report names it (`bambi (1.2)`), or the
  /// explicit `no target` when the plan could not place the row at all.
  final String target;

  /// The live node id, when there is one; null for a row the plan never placed.
  final int? targetId;

  /// One of the kinds of [restoreItemKindWord]: `block field`, `script`, `script file`,
  /// `requester row`, `layout`, `file` - or a phase name for a sweep that is not one item
  /// (`script slots`, `script files`, `requester table`).
  final String kindWord;

  /// The path, register or address as the plan names it.
  final String subject;

  /// The code the device answered with, or the explicit statement that it gave none. Never empty.
  final String code;

  /// One human sentence: what that code means for this row, and what the device therefore holds.
  final String meaning;

  const RestoreFailure({
    required this.archive,
    required this.target,
    this.targetId,
    required this.kindWord,
    required this.subject,
    required this.code,
    required this.meaning,
  });

  /// A failure of one plan item, worded the same way everywhere it is recorded.
  factory RestoreFailure.item(
    RestoreItem item, {
    required String meaning,
    RequestFault? fault,
    String? clientReason,
  }) {
    final target = item.targetDevice;
    return RestoreFailure(
      archive: item.device.name,
      target: target == null ? 'no target' : _targetLabel(target),
      targetId: target?.id,
      kindWord: restoreItemKindWord(item),
      subject: restoreItemSubject(item),
      code: restoreCodeText(fault: fault, clientReason: clientReason),
      meaning: meaning,
    );
  }

  /// A failure of one row a sweep settled (a script slot, a script file, a requester entry), which
  /// is an item of the device rather than of the archive.
  factory RestoreFailure.device(
    LiveDevice target, {
    required String kindWord,
    required String subject,
    required String meaning,
    RequestFault? fault,
    String? clientReason,
  }) => RestoreFailure(
    archive: target.name,
    target: _targetLabel(target),
    targetId: target.id,
    kindWord: kindWord,
    subject: subject,
    code: restoreCodeText(fault: fault, clientReason: clientReason),
    meaning: meaning,
  );

  bool get placed => targetId != null;

  /// The device a report groups this row under: the live node it was written to, or the archive
  /// device when the plan never placed it.
  String get group => placed ? target : '$target (from $archive)';

  /// The one-line form, for a caller that already has the device headings (the run's plain failure
  /// list, a log line).
  String get line => '$target: $kindWord $subject - $code: $meaning';

  /// The multi-line form the result screen and the copied text use: the row, then the code the
  /// device answered with and what it means, on their own lines so a phone can read them without
  /// scrolling sideways.
  String get block =>
      '  $kindWord  $subject\n'
      '    code: $code\n'
      '    why : $meaning';
}

/// `bambi (1.2)`: the target's name and its bus address, which is what a card has to quote.
String _targetLabel(LiveDevice target) =>
    '${target.name} (${idToString(target.id)})';

/// Everything one restore run did, with its failures carrying their reasons.
///
/// This is the run's result: the counts, the structured [reasons], the four sweeps' reports, and
/// the plain-text [text] the result screen copies. It survives the page being closed through
/// [RestoreSession], so the reasons can be quoted after the fact instead of living only in a
/// snackbar that is already gone.
class RestoreReport {
  final DateTime at;
  final int written;

  /// Every failure of the run, in the order it happened, each with its reason.
  final List<RestoreFailure> reasons;

  final List<RequesterPrune> requesterPrune;
  final List<ScriptSlotPrune> scriptSlots;
  final List<ScriptFilePrune> scriptFiles;
  final List<ScriptReloadReport> scriptReload;

  RestoreReport({
    required this.at,
    required this.written,
    required this.reasons,
    this.requesterPrune = const [],
    this.scriptSlots = const [],
    this.scriptFiles = const [],
    this.scriptReload = const [],
  });

  int get failed => reasons.length;

  /// The failures as plain lines, one per reason - the shape a log line or a test reads.
  List<String> get failures => [for (final r in reasons) r.line];

  /// The failures grouped by the device they were being written to, in first-seen order, so the
  /// result screen can show one section per node - what a person fixing a rig actually walks
  /// through. Rows the plan could not place at all are grouped under `no target`.
  List<({String device, List<RestoreFailure> failures})> get byDevice {
    final order = <String>[];
    final groups = <String, List<RestoreFailure>>{};
    for (final reason in reasons) {
      final group = reason.group;
      if (groups[group] == null) {
        groups[group] = [];
        order.add(group);
      }
      groups[group]!.add(reason);
    }
    return [
      for (final device in order) (device: device, failures: groups[device]!),
    ];
  }

  /// What the run DELETED from the devices, one line per row.
  ///
  /// A deletion is not a success and not a failure: it is a row the restore removed because the
  /// archive does not name it, and it used to be reported in the plan page's snackbar - a count and
  /// the TRIDs/slots. Moving the report onto its own screen must not lose it, so it travels here and
  /// is shown under its own heading.
  List<String> get removals => [
    for (final prune in requesterPrune)
      for (final trid in prune.cancelled)
        'cancelled requester entry 0x${trid.toRadixString(16)} '
            'on ${prune.deviceName}',
    for (final prune in scriptSlots) ...[
      for (final e in prune.unloaded.entries)
        'unloaded script slot ${e.key} (${scriptFileName(e.value)}) '
            'on ${prune.deviceName}',
      for (final e in prune.duplicates.entries)
        'unloaded duplicate script slot ${e.key} '
            '(${scriptFileName(e.value.fileId)}, also at slot '
            '${e.value.canonical}) on ${prune.deviceName}',
    ],
    for (final prune in scriptFiles)
      for (final e in prune.deleted.entries)
        'deleted script file ${e.key} (${e.value} B, slot '
            '${scriptFileId(e.key)}) on ${prune.deviceName}',
  ];

  /// The rows the run deliberately LEFT because the user unticked them: "not selected" is not "not
  /// in the archive", so they are named rather than dropped from the report.
  List<String> get keptBySelection => [
    for (final prune in scriptSlots)
      for (final e in prune.keptBySelection.entries)
        'kept script slot ${e.key} (${scriptFileName(e.value)}) on '
            '${prune.deviceName}',
    for (final prune in requesterPrune)
      for (final trid in prune.kept)
        'kept requester entry 0x${trid.toRadixString(16)} on '
            '${prune.deviceName}',
    for (final prune in scriptFiles)
      for (final e in prune.keptBySelection.entries)
        'kept script file ${e.key} (${e.value} B) on ${prune.deviceName}',
  ];

  /// The whole result as text, for the clipboard: a heading, the counts, and every failure grouped
  /// by device with its code and what it means.
  ///
  /// Written to be pasted as-is: no colour, no icons, no line wider than a phone's screen, and the
  /// counts first so the reader knows the size of what follows.
  String get text {
    final buffer = StringBuffer()
      ..writeln('Restore report ${_stamp(at)}')
      ..writeln('$written written · $failed failed');
    if (reasons.isEmpty) {
      buffer
        ..writeln()
        ..writeln('Every selected item landed.');
    } else {
      for (final group in byDevice) {
        buffer
          ..writeln()
          ..writeln(
            '${group.device} · ${group.failures.length} '
            'failure${group.failures.length == 1 ? '' : 's'}',
          );
        for (final failure in group.failures) {
          buffer.writeln(failure.block);
        }
      }
    }
    void section(String heading, List<String> rows) {
      if (rows.isEmpty) return;
      buffer
        ..writeln()
        ..writeln('$heading (${rows.length})');
      for (final row in rows) {
        buffer.writeln('  $row');
      }
    }

    section('Removed from the device', removals);
    section('Left alone by selection', keptBySelection);
    return buffer.toString().trimRight();
  }

  /// The run's facts as a map, for a caller that stores a result rather than prints it: the counts
  /// and every failure with the same fields the text shows, so a card can quote a reason after the
  /// fact without parsing prose.
  Map<String, Object?> get metadata => {
    'at': at.toIso8601String(),
    'written': written,
    'failed': failed,
    'failures': [
      for (final r in reasons)
        {
          'device': r.target,
          'targetId': r.targetId,
          'archive': r.archive,
          'kind': r.kindWord,
          'subject': r.subject,
          'code': r.code,
          'meaning': r.meaning,
        },
    ],
    'removed': removals,
    'kept': keptBySelection,
  };

  static String _stamp(DateTime at) =>
      '${at.year.toString().padLeft(4, '0')}-'
      '${at.month.toString().padLeft(2, '0')}-'
      '${at.day.toString().padLeft(2, '0')} '
      '${at.hour.toString().padLeft(2, '0')}:'
      '${at.minute.toString().padLeft(2, '0')}';
}

/// The last restore of this app session.
///
/// The result screen is not the only place a reason has to survive: the owner reads the run on a
/// phone, closes it, and then writes a card - so the last report stays readable from the backup page
/// for as long as the app runs, and every failure is also copied into [AppDiagnostics], whose ring
/// the Logs page dumps and copies.
abstract final class RestoreSession {
  static RestoreReport? last;

  /// Keeps [report] as the session's last restore and records each failure's line in the
  /// diagnostics ring, so a run can be quoted long after its dialog was closed.
  static void record(RestoreReport report) {
    last = report;
    for (final failure in report.reasons) {
      AppDiagnostics.log('restore', failure.line);
    }
  }

  static void clear() => last = null;
}
