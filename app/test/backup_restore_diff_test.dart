// The restore plan as a diff viewer (card t_6ffa42c6): every row is classified against the live
// device as add / replace (change) / same / delete (remove), and the plan carries the deletions it
// would apply as rows of their own.
//
// The device here is synthetic on purpose: the classification is a comparison of two snapshots, so
// it is driven by `planFromSnapshot` with the live snapshot and its inventory handed in, and no
// rig is involved. Each case states what is compared for its kind:
//
//   entry        the value bytes the restore would write against the bytes the walk read;
//   script       the image the archive rebuilds to against the image the device's SCR_XXX
//                rebuilds to (so stored padding is not a difference);
//   raw file     the file's bytes;
//   subscription its identity (requesterKey), then its trigger and timing;
//   SNDB         the address the roster names for the serial.

import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

BackupEntry _entry(
  String field,
  int fieldIndex,
  Object? value, {
  String type = 'Number',
  int size = 4,
}) => BackupEntry(
  field: field,
  fieldIndex: fieldIndex,
  key: 'Key 0',
  keyIndex: 0,
  type: type,
  flags: const ['Persistent'],
  value: value,
  size: size,
);

LiveEntry _liveEntry(
  String field,
  int fieldIndex, {
  List<int> value = const [],
  int size = 4,
}) => LiveEntry(
  field: fieldIndex,
  key: 0,
  fieldName: field,
  keyName: 'Key 0',
  meta: ValueInfo(type: DataType.number.value, size: size),
  value: value,
);

LiveBlock _pwmBlock(List<LiveEntry> entries) => LiveBlock(
  type: BlockType.pwm.value,
  instance: 0,
  name: 'Fan',
  isDynamic: false,
  entries: entries,
);

BackupBlock _pwmArchive(List<BackupEntry> entries) => BackupBlock(
  type: 'PWM output',
  typeIndex: BlockType.pwm.value,
  instance: 0,
  name: 'Fan',
  isDynamic: false,
  entries: entries,
);

BackupDevice _device({
  List<BackupBlock> blocks = const [],
  List<BackupScript> scripts = const [],
  List<BackupFile> files = const [],
  List<BackupRequesterSubscription> subscriptions = const [],
  List<BackupSndbEntry> sndb = const [],
}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('0.1')!,
  name: 'Tamu',
  blocks: blocks,
  scripts: scripts,
  files: files,
  requesterSubscriptions: subscriptions,
  sndb: sndb,
);

LiveDevice _live({
  List<LiveBlock> blocks = const [],
  int capabilities =
      Capability.scripts | Capability.subscriptionRequest | Capability.core,
}) => LiveDevice(
  id: idFromString('0.1')!,
  name: 'Tamu',
  capabilities: capabilities,
  blocks: blocks,
);

BackupScript _script(int slot, String name, {int lines = 0}) => BackupScript(
  slot: slot,
  functionName: name,
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: [for (var i = 0; i < lines; i++) BackupLine.of(ScriptLine.callTo(0))],
);

List<int> _image(BackupScript script) => script.toDraft().toImage();

/// [data] (the archive's file) with [size] bytes' worth of the device's own storage padding
/// appended: the same script, stored in a longer file.
List<int> _padded(List<int> data, {int padding = 8}) => [
  ...data,
  ...List<int>.filled(padding, 0),
];

/// The plan for [archive] against [live], with [inventory] as what the device holds beyond its
/// block walk.
RestorePlan _plan(
  BackupDevice archive,
  LiveDevice live, {
  LiveInventory? inventory,
}) => planFromSnapshot(
  devices: [archive],
  live: [live],
  inventories: inventory == null ? const {} : {live.id: inventory},
);

/// One row's class, as a word, for a test that prints the whole classification.
String _word(DiffKind? kind) => kind?.word ?? 'unclassified';

void main() {
  test('an entry is an add, a replace or the same by its value bytes', () {
    final archive = _device(
      blocks: [
        _pwmArchive([
          _entry('Frequency', 0, 25000.0),
          _entry('Duty', 1, 40.0),
          _entry('Extra', 2, 7.0),
        ]),
      ],
    );
    // The device holds Frequency at the archive's value and Duty at another one, and carries no
    // address for Extra at all.
    final frequency = _liveEntry('Frequency', 0);
    final live = _live(
      blocks: [
        _pwmBlock([
          _liveEntry(
            'Frequency',
            0,
            value: resolveEntryBytes(
              archive.blocks.first.entries[0],
              frequency,
            )!,
          ),
          _liveEntry(
            'Duty',
            1,
            value: [0, 0, 0x20, 0x42], // 40.0 would be this; 50.0 is not
          ),
        ]),
      ],
    );
    final plan = _plan(archive, live, inventory: const LiveInventory());
    final byField = {
      for (final item in plan.items)
        if (item.kind == RestoreKind.entry) item.entry!.field: item,
    };
    print(
      'entries: ${[for (final item in byField.values) '${item.entry!.field} = ${_word(item.diff)}']}',
    );
    expect(byField['Frequency']!.diff, DiffKind.same);
    expect(byField['Duty']!.diff, DiffKind.change);
    expect(
      byField['Duty']!.diffNote,
      'the device holds a different value',
      reason: 'the note is what the row prints under the title',
    );
    // No live address: the archive has it, the device does not. It stays unready, because
    // `prepareDynamicBlocks` is what creates a missing address while the restore runs.
    expect(byField['Extra']!.diff, DiffKind.add);
    expect(byField['Extra']!.ready, isFalse);
    expect(byField['Extra']!.selected, isFalse);
  });

  test('a script is the same when its image rebuilds identically', () {
    final script = _script(1, 'Blink', lines: 2);
    final archive = _device(scripts: [script]);
    final live = _live(blocks: [_pwmBlock(const [])]);
    final padded = _padded(_image(script));
    final inventory = LiveInventory(
      fileNames: {'SCR_001'},
      fileSizes: {'SCR_001': padded.length},
      files: {'SCR_001': padded},
    );
    final plan = _plan(archive, live, inventory: inventory);
    final item = plan.items.single;
    print(
      'script: image=${_image(script).length} B, device file=${padded.length} B, '
      'class=${_word(item.diff)}',
    );
    expect(
      item.diff,
      DiffKind.same,
      reason:
          'the stored file carries padding the encoder regenerates, so the '
          'semantic form is the same',
    );
    expect(item.diffNote, isNull);
  });

  test(
    'a script the device holds differently is a replace, and one it lacks is an add',
    () {
      final same = _script(1, 'Blink', lines: 2);
      final changed = _script(2, 'Fade', lines: 2);
      final archive = _device(scripts: [same, changed, _script(3, 'Beacon')]);
      final live = _live(blocks: [_pwmBlock(const [])]);
      final inventory = LiveInventory(
        fileNames: {'SCR_001', 'SCR_002'},
        fileSizes: {'SCR_001': _image(same).length},
        files: {
          'SCR_001': _image(same),
          // Same slot, a different program: the image the device rebuilds to differs.
          'SCR_002': _image(_script(2, 'Fade', lines: 3)),
        },
      );
      final plan = _plan(archive, live, inventory: inventory);
      final bySlot = {
        for (final item in plan.items) item.script!.slot: _word(item.diff),
      };
      print('scripts by slot: $bySlot');
      expect(bySlot[1], 'Same');
      expect(bySlot[2], 'Replace');
      expect(bySlot[3], 'Add');
    },
  );

  test('a raw file is a replace when its bytes differ, while its entries stay '
      'the same', () {
    final archive = _device(
      blocks: [
        _pwmArchive([_entry('Frequency', 0, 25000.0)]),
      ],
      files: [
        BackupFile(
          name: 'DATA',
          kind: 'Binary',
          data: base64Encode([1, 2, 3, 4]),
        ),
      ],
    );
    final frequency = _liveEntry('Frequency', 0);
    final live = _live(
      blocks: [
        _pwmBlock([
          _liveEntry(
            'Frequency',
            0,
            value: resolveEntryBytes(
              archive.blocks.first.entries[0],
              frequency,
            )!,
          ),
        ]),
      ],
    );
    final inventory = LiveInventory(
      fileNames: {'DATA'},
      fileSizes: {'DATA': 4},
      files: {
        'DATA': const [1, 2, 3, 9],
      },
    );
    final plan = _plan(archive, live, inventory: inventory);
    final entry = plan.items.singleWhere((i) => i.kind == RestoreKind.entry);
    final file = plan.items.singleWhere((i) => i.kind == RestoreKind.file);
    print('raw file: entry=${_word(entry.diff)} file=${_word(file.diff)}');
    expect(entry.diff, DiffKind.same);
    expect(file.diff, DiffKind.change);
    expect(file.diffNote, contains('DATA'));

    // The same file on both sides, and one the device does not carry at all.
    final identical = _plan(
      _device(
        files: [
          BackupFile(
            name: 'DATA',
            kind: 'Binary',
            data: base64Encode([1, 2, 3, 9]),
          ),
        ],
      ),
      live,
      inventory: inventory,
    );
    expect(identical.removals, isEmpty);
    expect(
      identical.items.singleWhere((i) => i.kind == RestoreKind.file).diff,
      DiffKind.same,
    );
    final missing = _plan(
      archive,
      live,
      inventory: LiveInventory(fileNames: const {}, fileSizes: const {}),
    );
    expect(
      missing.items.singleWhere((i) => i.kind == RestoreKind.file).diff,
      DiffKind.add,
    );
  });

  test('a file table that was not read is a difference, never an add', () {
    // "Could not look" is not "holds nothing": a device whose table did not answer must not read
    // as a device missing the file, and must not read as "the same" either.
    final archive = _device(
      files: [
        BackupFile(
          name: 'DATA',
          kind: 'Binary',
          data: base64Encode([1, 2, 3, 4]),
        ),
      ],
    );
    final live = _live(blocks: [_pwmBlock(const [])]);
    final plan = _plan(archive, live, inventory: const LiveInventory());
    final file = plan.items.single;
    print('unread table: class=${_word(file.diff)} note=${file.diffNote}');
    expect(file.diff, DiffKind.change);
    expect(file.diffNote, contains('did not return its file table'));
  });

  test('a subscription is classified by identity, then by its trigger and timing', () {
    // Three archive subscriptions that differ in their TARGET address, which is what makes them
    // three subscriptions and not one written three times: identity is (provider node, source
    // register, target register) - `requesterKey`, the same key the apply and the prune use.
    const sourceRef = BackupBlockRef(
      block: 'PWM output',
      typeIndex: 3,
      instance: 0,
      field: 'Frequency',
      fieldIndex: 0,
      key: 'Key 0',
      keyIndex: 0,
    );
    BackupBlockRef target(int field, String name) => BackupBlockRef(
      block: 'PWM output',
      typeIndex: 3,
      instance: 0,
      field: name,
      fieldIndex: field,
      key: 'Key 0',
      keyIndex: 0,
    );
    BackupRequesterSubscription sub(int field, String name, int period) =>
        BackupRequesterSubscription(
          provider: '0.2',
          trigger: 'Periodic',
          periodMs: period,
          minTimeMs: 100,
          deadzone: 0,
          source: sourceRef,
          target: target(field, name),
        );
    final archive = _device(
      blocks: [
        _pwmArchive([
          _entry('Frequency', 0, 25000.0),
          _entry('Duty', 1, 40.0),
          _entry('Extra', 2, 7.0),
        ]),
      ],
      subscriptions: [
        sub(0, 'Frequency', 1000),
        sub(1, 'Duty', 2000),
        sub(2, 'Extra', 3000),
      ],
    );
    final requester = _live(
      blocks: [
        _pwmBlock([
          _liveEntry('Frequency', 0),
          _liveEntry('Duty', 1),
          _liveEntry('Extra', 2),
        ]),
      ],
    );
    final provider = LiveDevice(
      id: 2,
      name: 'Provider',
      capabilities: Capability.subscriptionProvide,
      blocks: [
        _pwmBlock([_liveEntry('Frequency', 0)]),
      ],
    );
    RequesterSubscription live(
      int index,
      int trid,
      int targetReg,
      int periodMsec,
    ) => RequesterSubscription(
      index: index,
      providerAddr: 2,
      trid: trid,
      sourceReg: makeBlockInfo(BlockType.pwm.value, 0, 0, 0),
      targetReg: targetReg,
      trigger: TriggerType.periodic,
      periodMs: periodMsec,
      minTimeMs: 100,
    );
    final plan = planFromSnapshot(
      devices: [archive],
      live: [requester, provider],
      inventories: {
        requester.id: LiveInventory(
          requesters: [
            // The same subscription...
            live(0, 0x1000, makeBlockInfo(BlockType.pwm.value, 0, 0, 0), 1000),
            // ...the same identity with another period...
            live(1, 0x1001, makeBlockInfo(BlockType.pwm.value, 0, 1, 0), 2500),
            // ...and nothing for the third address: an entry the device does not carry.
          ],
        ),
      },
    );
    final diffs = [
      for (final item in plan.items)
        if (item.kind == RestoreKind.subscription) _word(item.diff),
    ];
    print('subscriptions: $diffs');
    expect(diffs, ['Same', 'Replace', 'Add']);
  });

  test(
    'the requester entries and loaded slots the archive lacks are DELETIONS',
    () {
      final archive = _device(
        blocks: [
          _pwmArchive([_entry('Frequency', 0, 25000.0)]),
        ],
        scripts: [_script(4, 'Beacon')],
      );
      final live = _live(
        blocks: [
          _pwmBlock([_liveEntry('Frequency', 0)]),
        ],
      );
      final inventory = LiveInventory(
        fileNames: {'SCR_004'},
        fileSizes: {'SCR_004': _image(_script(4, 'Beacon')).length},
        files: {'SCR_004': _image(_script(4, 'Beacon'))},
        // Slot 4 is the archive's own slot (settled); slot 0 is a second copy of SCR_004; slot 9 is
        // an earlier setup's leftover.
        loadedSlots: {0: 4, 4: 4, 9: 9},
        requesters: [
          RequesterSubscription(
            index: 0,
            providerAddr: 2,
            trid: 0x1000,
            sourceReg: 0,
            targetReg: 0,
            trigger: TriggerType.periodic,
            periodMs: 1000,
            minTimeMs: 0,
          ),
        ],
      );
      final plan = _plan(archive, live, inventory: inventory);
      final labels = [for (final removal in plan.removals) removal.label];
      print('deletions: $labels');
      expect(
        {
          for (final r in plan.removals)
            if (r.kind == RestoreKind.script) r.slot,
        },
        {0, 9},
        reason: 'slot 4 holds the archive script in the slot the archive names',
      );
      expect(
        plan.removals.firstWhere((r) => r.slot == 0).detail,
        contains('second copy'),
      );
      // The requester entry the archive does not name is a third deletion.
      expect(
        plan.removals.where((r) => r.kind == RestoreKind.subscription),
        hasLength(1),
      );
      expect(
        plan.removals
            .singleWhere((r) => r.kind == RestoreKind.subscription)
            .trid,
        0x1000,
      );
      // Every deletion is ticked by default - a restore makes the device match the archive - and
      // the two script slots map to the unticked set the apply reads.
      expect(plan.removals.every((r) => r.selected), isTrue);
      expect(plan.untickedScriptSlots(live.id), isEmpty);
      plan.setClassSelected(DiffKind.remove, false);
      expect(plan.removals.every((r) => r.selected), isFalse);
      expect(plan.untickedScriptSlots(live.id), {0, 9});
    },
  );

  test('a read-only entry is not a difference: All shows it, Diff does not', () {
    // A read-only entry is a value the DEVICE owns; the restore never writes it, so it is not
    // "the same" (nothing was compared) and it is not a difference either. It belongs in `all`,
    // where the point is to see the whole plan, and not in the default view - a page whose first
    // rows are things nothing can be done about reads as noise.
    final archive = _device(
      blocks: [
        BackupBlock(
          type: 'Button',
          typeIndex: BlockType.button.value,
          instance: 0,
          name: 'Button',
          isDynamic: false,
          entries: [
            BackupEntry(
              field: 'Button Raw State',
              fieldIndex: 0,
              key: 'Key 0',
              keyIndex: 0,
              type: 'Bool',
              flags: const ['Read Only'],
              value: false,
              size: 1,
            ),
          ],
        ),
      ],
    );
    final live = _live(
      blocks: [
        LiveBlock(
          type: BlockType.button.value,
          instance: 0,
          name: 'Button',
          isDynamic: false,
          entries: [
            LiveEntry(
              field: 0,
              key: 0,
              fieldName: 'Button Raw State',
              keyName: 'Key 0',
              meta: ValueInfo(
                type: DataType.bool_.value,
                flags: ValueFlags.readOnly,
                size: 1,
              ),
              value: const [0],
            ),
          ],
        ),
      ],
    );
    final plan = _plan(archive, live, inventory: const LiveInventory());
    final item = plan.items.single;
    expect(item.diff, isNull, reason: 'nothing was compared');
    expect(item.issue, readOnlyIssue);
    print(
      'read-only: diff view ${plan.view().shown} row(s), '
      'all view ${plan.view(PlanFilter.all).shown} row(s)',
    );
    expect(plan.view(PlanFilter.diff).items, isEmpty);
    expect(plan.view(PlanFilter.all).items, hasLength(1));
    expect(plan.view(PlanFilter.all).unavailable, 1);
    expect(item.selected, isFalse, reason: 'a read-only row cannot be ticked');
  });

  group('view filters and class toggles', () {
    RestorePlan build() {
      final archive = _device(
        blocks: [
          _pwmArchive([
            _entry('Frequency', 0, 25000.0), // same
            _entry('Duty', 1, 40.0), // replace
            _entry('Extra', 2, 7.0), // add
          ]),
        ],
      );
      final frequency = _liveEntry('Frequency', 0);
      final live = _live(
        blocks: [
          _pwmBlock([
            _liveEntry(
              'Frequency',
              0,
              value: resolveEntryBytes(
                archive.blocks.first.entries[0],
                frequency,
              )!,
            ),
            _liveEntry('Duty', 1, value: const [0, 0, 0x20, 0x42]),
          ]),
        ],
      );
      return _plan(
        archive,
        live,
        inventory: LiveInventory(loadedSlots: {9: 9}, fileNames: const {}),
      );
    }

    test('diff is the default view and hides only the same rows', () {
      final plan = build();
      final diff = plan.view();
      print(
        'diff view: shown=${diff.shown} classes='
        '${[for (final item in diff.items) _word(item.diff)]} '
        'deletes=${diff.deletes}',
      );
      expect(diff.filter, PlanFilter.diff);
      expect(diff.items.map((i) => i.diff).toSet(), {
        DiffKind.change,
        DiffKind.add,
      });
      expect(diff.deletes, 1);
      // `all` is where the untouched rows are.
      final all = plan.view(PlanFilter.all);
      expect(all.shown, plan.items.length + plan.removals.length);
      expect(all.selected, greaterThanOrEqualTo(2));
      // The single-class views are exactly one class.
      expect(plan.view(PlanFilter.add).items.single.diff, DiffKind.add);
      expect(plan.view(PlanFilter.change).items.single.diff, DiffKind.change);
      expect(plan.view(PlanFilter.remove).removals.single.slot, 9);
      expect(plan.view(PlanFilter.remove).items, isEmpty);
    });

    test('the footer counts follow the filter and the selection', () {
      final plan = build();
      final diff = plan.view();
      print(
        'diff footer: ${diff.selected}/${diff.shown} selected, '
        '${diff.deletesSelected}/${diff.deletes} to delete',
      );
      expect(diff.shown, 3, reason: 'the add, the replace and the delete');
      expect(
        diff.selected,
        2,
        reason: 'the replace and the delete are ticked; the add is unready',
      );
      expect(diff.deletesSelected, 1);

      // Unticking the delete row drops the clause without touching the writes.
      plan.removals.single.selected = false;
      expect(plan.view().deletesSelected, 0);
      expect(plan.view().selected, 1);

      // Unticking one item leaves the rest alone.
      final change = plan.view().items.singleWhere(
        (i) => i.diff == DiffKind.change,
      );
      change.selected = false;
      expect(plan.view().selected, 0);
      expect(
        plan.items.where((i) => i.selected).length,
        1,
        reason: 'only the same row (ticked, hidden by the filter) and it',
      );
    });

    test('a class toggle ticks and unticks exactly that class', () {
      final plan = build();
      final change = plan.view().items.singleWhere(
        (i) => i.diff == DiffKind.change,
      );
      final same = plan.items.singleWhere((i) => i.diff == DiffKind.same);
      expect(plan.classSelected(DiffKind.change), isTrue);
      expect(plan.classCount(DiffKind.change), 1);
      expect(plan.classCount(DiffKind.remove), 1);

      plan.setClassSelected(DiffKind.change, false);
      print(
        'after unticking Replace: change=${change.selected} same=${same.selected} '
        'deletes=${plan.removals.single.selected}',
      );
      expect(change.selected, isFalse);
      expect(plan.classSelected(DiffKind.change), isFalse);
      expect(same.selected, isTrue, reason: 'another class is untouched');
      expect(
        plan.removals.single.selected,
        isTrue,
        reason: 'the delete class is untouched',
      );
      expect(plan.view().selected, 1, reason: 'only the delete row is left');

      plan.setClassSelected(DiffKind.change, true);
      expect(change.selected, isTrue);
      // An add row cannot be ticked while it is unready: the apply could not write it.
      plan.setClassSelected(DiffKind.add, true);
      expect(
        plan.items.singleWhere((i) => i.diff == DiffKind.add).selected,
        isFalse,
      );
    });
  });
}
