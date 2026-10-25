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

/// The row as the PLAN names it, for one archive entry: the block's type word, name and instance,
/// then the field and key in both words and indexes. Shared by [restoreItemSubject] and by a
/// structural skip, so a skipped marker and a written row are named the same way.
String restoreEntrySubject(BackupBlock block, BackupEntry entry) =>
    '${block.type} "${block.name}" #${block.instance} / '
    '${entry.field} [${entry.key}] '
    '(field ${entry.fieldIndex} key ${entry.keyIndex})';

/// The row as the PLAN names it: the archive's own field/key words, the script and the slot it
/// occupies, the two register addresses of a requester row, the name of a file.
///
/// This is the same wording the plan page prints for the row, so a reason quoted on a card points
/// at a row the owner can find on screen.
String restoreItemSubject(RestoreItem item) => switch (item.kind) {
  RestoreKind.entry => restoreEntrySubject(item.block!, item.entry!),
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
      case RequestFaultKind.notSent:
        return 'no code from the device (the app refused it and sent nothing'
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

/// One row the restore left as STRUCTURE: an address the archive carries no value for, so the run
/// wrote nothing to it ([entryDeclaresNoValue]). "Wrote nothing" is not "created nothing": a
/// structural row that DECLARES a length and that the device was missing is created zero-filled by
/// the structure phase, and one that declares no length cannot be created at all - the firmware
/// refuses a write with no bytes.
///
/// A dictionary marker is the case this exists for: `Geometry`/`Texture` at key 0 declares a field
/// as a keyed dictionary and holds no value, and the LED display's eye blocks are nine of them per
/// block. Writing one is a zero-length write, which the firmware refuses - and a refusal is
/// invisible in the result screen except as a failure count. These rows are neither written nor
/// failed: they are reported, one line each, so the count can say `skipped (structural)` and the
/// reader can tell a marker from a write that did not land.
///
/// [reason] gives both halves, because they are different facts: WHY the archive carries no value
/// for the row (it declares no length - a marker - or it is a volatile row whose value is runtime
/// state the capture leaves out) and what the device state makes of it (carried with a value of its
/// own, created as structure, uncreatable, or unplaceable).
class RestoreSkip {
  final String archive;
  final String target;
  final int? targetId;
  final String kindWord;
  final String subject;
  final String reason;

  const RestoreSkip({
    required this.archive,
    required this.target,
    this.targetId,
    required this.kindWord,
    required this.subject,
    required this.reason,
  });

  bool get placed => targetId != null;

  /// The one-line form: the row, then why nothing was sent to it.
  String get line =>
      '$target: $kindWord $subject - skipped (structural): $reason';

  /// The result screen's shape: the row on its own line, the reason wrapped under it.
  ///
  /// The wrap happens here rather than in the screen because [RestoreReport.text] is what a reader
  /// pastes into a card: a reason that rides out as one 150-character line cannot be read on a
  /// phone, and the report's whole point is that its rows can. Same reason the failure blocks carry
  /// one sentence per line.
  String get block {
    const head = '    why : ';
    final buffer = StringBuffer('  $kindWord  $subject\n$head');
    _wrapUnder(buffer, reason, head);
    return buffer.toString();
  }
}

/// Wraps [text] under [head] at a phone-readable width, appending to [buffer].
///
/// Shared by the report's row shapes ([RestoreSkip.block], [RestoreStructureRow.block]) so the
/// clipboard text has ONE wrap rule: every one of them is pasted into a card.
void _wrapUnder(StringBuffer buffer, String text, String head) {
  const width = 76;
  var column = head.length;
  for (final word in text.split(' ')) {
    if (column > head.length && column + word.length + 1 > width) {
      buffer.write('\n${' ' * head.length}$word');
      column = head.length + word.length;
    } else {
      if (column > head.length) {
        buffer.write(' ');
        column++;
      }
      buffer.write(word);
      column += word.length;
    }
  }
}

/// One dynamic row the STRUCTURE phase made on a target during a run, as the report names it.
///
/// A restore writes into a structure that must already exist, so `prepareDynamicBlocks` makes the
/// rows the device is missing - and RE-CREATES the ones it holds in a `(type, size)` that
/// contradicts the archive, because a dynamic row carries its OWN type and length: `SetEntry` gives
/// the address the archive's shape as soon as a write carrying one lands, and writing the value
/// into a row of another type would re-assert the damage instead of repairing it.
///
/// Both used to be silent. A dynamic row's type is state the device holds, so a row left holding
/// `Number` at 1 byte where the archive declares `Bool` at 1 byte broke every reader of it (the
/// emote selector's register read lands as `SCRIPT_ERR_TYPE`) while the report said `0 failed`.
/// These rows are neither written nor skipped: they are one line each, with the shape the row was
/// made AS and the shape the device held before it.
class RestoreStructureRow {
  final String archive;
  final String target;
  final int? targetId;
  final String kindWord;
  final String subject;

  /// The shape the row was made as, in the archive's words: `Bool, 1 byte`.
  final String shape;

  /// What the device held before, when it held the row in another shape: `Number, 1 byte`. Null
  /// for a row the device did not carry at all, which is a create.
  final String? held;

  const RestoreStructureRow({
    required this.archive,
    required this.target,
    this.targetId,
    required this.kindWord,
    required this.subject,
    required this.shape,
    this.held,
  });

  /// A row the device already carried in another shape: this one was re-created, not created.
  bool get recreated => held != null;

  String get verb => recreated ? 're-created' : 'created';

  bool get placed => targetId != null;

  /// The one-line form: the row, what it was made as, and what the device held before.
  String get line =>
      '$target: $kindWord $subject - $verb as $shape'
      '${held == null ? '' : ', where it held $held'}';

  /// The result screen's shape: the row on its own line, the shape wrapped under it.
  String get block {
    const head = '    as  : ';
    final buffer = StringBuffer('  $kindWord  $subject\n$head');
    _wrapUnder(
      buffer,
      '$verb as $shape'
      '${held == null ? '' : ', where the device held $held'}',
      head,
    );
    return buffer.toString();
  }
}

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

  /// The rows the run deliberately did not write because they carry no value of their own
  /// ([RestoreSkip]): dictionary markers, structural rows. Counted apart from [written] and
  /// [reasons], never folded into either.
  final List<RestoreSkip> skipped;

  /// The dynamic rows the STRUCTURE phase made on a target ([RestoreStructureRow]), in the order
  /// the run touched them: created when the device was missing the address, re-created when it
  /// held it in a `(type, size)` that contradicts the archive.
  ///
  /// They are the rows a restore needs before it can write anything, and the ones a type the
  /// device had been left holding wrongly is repaired through, so a run that reports only
  /// `written` and `failed` cannot show that repair - or the failure to make it - at all.
  final List<RestoreStructureRow> structure;

  final List<RequesterPrune> requesterPrune;
  final List<ScriptSlotPrune> scriptSlots;
  final List<ScriptFilePrune> scriptFiles;
  final List<ScriptReloadReport> scriptReload;

  RestoreReport({
    required this.at,
    required this.written,
    required this.reasons,
    this.skipped = const [],
    this.structure = const [],
    this.requesterPrune = const [],
    this.scriptSlots = const [],
    this.scriptFiles = const [],
    this.scriptReload = const [],
  });

  int get failed => reasons.length;

  /// The structure rows the device did NOT carry before the run.
  List<RestoreStructureRow> get created => [
    for (final row in structure)
      if (!row.recreated) row,
  ];

  /// The structure rows the device held in a `(type, size)` the archive contradicts, and the run
  /// therefore re-created in the archive's shape.
  List<RestoreStructureRow> get recreated => [
    for (final row in structure)
      if (row.recreated) row,
  ];

  /// The structure rows as plain lines, one per row.
  List<String> get structureLines => [for (final row in structure) row.line];

  /// The failures as plain lines, one per reason - the shape a log line or a test reads.
  List<String> get failures => [for (final r in reasons) r.line];

  /// The structural skips as plain lines, one per row.
  List<String> get skips => [for (final s in skipped) s.line];

  /// The run's counts as one line: written, created, re-created, skipped as structure, failed.
  ///
  /// The three structural numbers are separate because they are three different facts and a run
  /// that folded them together hid the reason a row was not written behind a count: a STRUCTURAL
  /// row is one the archive carries no value for (nothing was meant to be sent to it), a CREATED
  /// row one the device did not have, and a RE-CREATED row one it held in the wrong shape - the
  /// case a device that had been left holding a `Number` where the archive declares a `Bool` is
  /// repaired through. `created` and `re-created` are counted per structure row, so a row whose
  /// value the run then wrote appears here AND in `written`; [text] says so under the heading.
  String get counts {
    final parts = <String>['$written written'];
    if (created.isNotEmpty) parts.add('${created.length} created');
    if (recreated.isNotEmpty) parts.add('${recreated.length} re-created');
    parts.add('${skipped.length} skipped (structural)');
    parts.add('$failed failed');
    return parts.join(' · ');
  }

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
      ..writeln(counts);
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
    // The rows that were never a write: named, so "0 failed" cannot be read as "nothing was left
    // out". A structural row is a dictionary marker or a row whose value the archive does not
    // carry, and there is nothing to send to it.
    if (skipped.isNotEmpty) {
      buffer
        ..writeln()
        ..writeln('Skipped as structural (${skipped.length})');
      for (final skip in skipped) {
        buffer.writeln(skip.block);
      }
    }
    // The structure the run made, per row. A created row is one the device did not have; a
    // re-created one is a row it held at the archive's address but NOT in the archive's
    // (type, size) - the damage a device keeps until a run gives it the shape back. The overlap
    // with `written` is stated under the heading so the counts cannot be read as disjoint.
    void structureSection(
      String heading,
      String note,
      List<RestoreStructureRow> rows,
    ) {
      if (rows.isEmpty) return;
      buffer
        ..writeln()
        ..writeln('$heading (${rows.length})');
      for (final row in rows) {
        buffer.writeln(row.block);
      }
      // The note is wrapped like every other sentence in this text: the report is pasted into a
      // card, and one 300-character line cannot be read on a phone.
      const head = '  note: ';
      buffer.write(head);
      _wrapUnder(buffer, note, head);
      buffer.writeln();
    }

    structureSection(
      'Created',
      'The device did not carry these rows; they are made as the archive '
          'declares them. A row whose value the archive carries is then '
          'written, so it is counted here and in "written" above.',
      created,
    );
    structureSection(
      'Re-created',
      'The device held these rows in another type or size; each is re-created '
          'as the archive declares it, which is what repairs a row left holding '
          'a Number where the archive declares a Bool. A row whose value the '
          'archive carries is then written, so it is counted here and in '
          '"written" above.',
      recreated,
    );
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
    'created': created.length,
    'recreated': recreated.length,
    'skipped': skipped.length,
    'failed': failed,
    'structure': [
      for (final row in structure)
        {
          'device': row.target,
          'targetId': row.targetId,
          'archive': row.archive,
          'kind': row.kindWord,
          'subject': row.subject,
          'verb': row.verb,
          'shape': row.shape,
          'held': row.held,
        },
    ],
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
    'structural': [
      for (final s in skipped)
        {
          'device': s.target,
          'targetId': s.targetId,
          'archive': s.archive,
          'kind': s.kindWord,
          'subject': s.subject,
          'reason': s.reason,
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
  ///
  /// The structural skips are recorded too: "18 skipped (structural)" is a fact about the run, and
  /// a reader who reopens it on the Logs page must be able to see WHICH rows they were.
  static void record(RestoreReport report) {
    last = report;
    for (final failure in report.reasons) {
      AppDiagnostics.log('restore', failure.line);
    }
    for (final skip in report.skipped) {
      AppDiagnostics.log('restore', skip.line);
    }
    // A created or re-created row is a repair of device state the archive contradicts; the run's
    // proof of it has to outlive the dialog the same way its failures do.
    for (final row in report.structure) {
      AppDiagnostics.log('restore', row.line);
    }
  }

  static void clear() => last = null;
}
