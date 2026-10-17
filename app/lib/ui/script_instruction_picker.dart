/// Instruction picker (Docs/App/Service views/Script.md): recommendation-first, grouped by
/// instruction category.
library;

import 'package:flutter/material.dart';

import '../core/script_instructions.dart';
import 'dialogs.dart';
import 'theme.dart';
import 'widgets.dart';

class ScriptInstructionPicker extends StatefulWidget {
  final ScriptInstructionDef? current;
  final bool hasDestination;

  const ScriptInstructionPicker({
    super.key,
    this.current,
    this.hasDestination = false,
  });

  @override
  State<ScriptInstructionPicker> createState() =>
      _ScriptInstructionPickerState();
}

class _ScriptInstructionPickerState extends State<ScriptInstructionPicker> {
  int? _category;

  List<(String, int, List<ScriptInstructionDef>)> _groups() {
    final groups = <(String, int, List<ScriptInstructionDef>)>[];
    for (final def in scriptInstructions) {
      if (groups.isNotEmpty && groups.last.$2 == def.category) {
        groups.last.$3.add(def);
      } else {
        groups.add((
          ScriptInstructionDef.categoryName(def.category),
          def.category,
          [def],
        ));
      }
    }
    return groups;
  }

  List<ScriptInstructionDef> _recommendations() {
    final recs = <ScriptInstructionDef>[];
    void add(int cat, int op) {
      for (final d in scriptInstructions) {
        if (d.category == cat && d.op == op && !recs.contains(d)) recs.add(d);
      }
    }

    // Context: with a destination the line produces a value; without one it is control
    // flow / timing / a service action.
    if (widget.hasDestination) {
      add(catMath, 0); // Set
      add(catTime, 2); // Get time
      add(catService, 1); // Register read
      add(catCompose, 1); // Extract
    } else {
      add(catFlow, 0); // If
      add(catFlow, 1); // While
      add(catTime, 1); // Wait until
      add(catService, 2); // Register write
      add(catService, 4); // Nop
      add(catFlow, 6); // Halt
    }
    final current = widget.current;
    if (current != null && !recs.contains(current)) recs.insert(0, current);
    return recs;
  }

  @override
  Widget build(BuildContext context) {
    final groups = _groups();
    final category = _category;
    return PickerShell(
      title: category == null
          ? 'Pick instruction'
          : ScriptInstructionDef.categoryName(category),
      onBack: category == null ? null : () => setState(() => _category = null),
      backTooltip: 'Back to categories',
      maxWidth: 380,
      height: 400,
      body: category == null
          ? ListView(
              children: [
                _header('Recommended'),
                for (final def in _recommendations()) _tile(def),
                _header('Groups'),
                for (var i = 0; i < groups.length; i++)
                  ListTile(
                    dense: true,
                    leading: const Icon(
                      Icons.folder_outlined,
                      size: IconSize.md,
                    ),
                    title: Text(groups[i].$1),
                    trailing: Text(
                      '${groups[i].$3.length}',
                      style: Theme.of(
                        context,
                      ).textTheme.labelSmall?.copyWith(color: kTextDisabled),
                    ),
                    onTap: () => setState(() => _category = groups[i].$2),
                  ),
              ],
            )
          : ListView(
              children: [
                for (final group in groups)
                  if (group.$2 == category)
                    for (final def in group.$3) _tile(def),
              ],
            ),
    );
  }

  Widget _header(String text) => Padding(
    padding: const EdgeInsets.only(top: Spacing.md, bottom: Spacing.hairline),
    child: SectionLabel(text),
  );

  Widget _tile(ScriptInstructionDef def) => ListTile(
    dense: true,
    title: Text(def.label),
    subtitle: Text(
      '${ScriptInstructionDef.categoryName(def.category)}'
      '${def.destination ? ' · destination' : ''}'
      ' · ${def.minOperands}..${def.maxOperands} operands',
      style: Theme.of(context).textTheme.bodySmall,
    ),
    onTap: () => Navigator.pop(context, def),
  );
}
