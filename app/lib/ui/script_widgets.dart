/// Shared script UI pieces (Docs/App/Service views/Script.md): state chips and the
/// input/output/variable/constant tables of a loaded script.
library;

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_file.dart';
import 'theme.dart';
import 'value_editor.dart';
import 'widgets.dart';

Color scriptStateColor(int state) => switch (state) {
      ScriptState.running => Colors.greenAccent,
      ScriptState.paused => Colors.amberAccent,
      ScriptState.waiting => Colors.lightBlueAccent,
      ScriptState.finished => Colors.white54,
      ScriptState.error => Colors.redAccent,
      _ => Colors.white38,
    };

/// One category (inputs / outputs / variables / constants) of a loaded script's Register
/// entries. Inputs and variables are editable; outputs and constants are read-only.
class ScriptIoSection extends StatefulWidget {
  final ScriptClient client;
  final int slot;
  final int field;
  final String title;
  final IconData icon;
  final bool editable;

  /// Bump to force a re-read (the parent's refresh tick).
  final int revision;
  final VoidCallback? onChanged;

  const ScriptIoSection({
    super.key,
    required this.client,
    required this.slot,
    required this.field,
    required this.title,
    required this.icon,
    required this.editable,
    required this.revision,
    this.onChanged,
  });

  @override
  State<ScriptIoSection> createState() => _ScriptIoSectionState();
}

class _ScriptIoSectionState extends State<ScriptIoSection> {
  List<ScriptEntry?> _entries = [];
  bool _loaded = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void didUpdateWidget(covariant ScriptIoSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.revision != widget.revision || oldWidget.slot != widget.slot) {
      _load();
    }
  }

  Future<void> _load() async {
    final keys = await widget.client.enumerateKeys(widget.slot, widget.field);
    final entries = <ScriptEntry?>[];
    for (final key in keys) {
      entries.add(await widget.client.readEntry(widget.slot, widget.field, key));
    }
    if (!mounted) return;
    setState(() {
      _entries = entries;
      _loaded = true;
    });
  }

  Future<void> _edit(int key, ScriptEntry entry) async {
    final next = await showValueEditor(context, entry.meta.dataType, entry.value);
    if (next == null || !mounted) return;
    final ok = await widget.client
        .writeEntry(widget.slot, widget.field, key, entry.meta, next);
    if (!mounted) return;
    showSnack(context, ok ? '${widget.title} $key updated' : 'Write failed');
    widget.onChanged?.call();
  }

  @override
  Widget build(BuildContext context) {
    if (!_loaded || _entries.isEmpty) return const SizedBox.shrink();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
          child: Row(children: [
            Icon(widget.icon, size: 16, color: kOrange),
            const SizedBox(width: 6),
            Text(widget.title,
                style: const TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
            const SizedBox(width: 6),
            Text('(${_entries.length})', style: const TextStyle(color: Colors.white54, fontSize: 12)),
          ]),
        ),
        for (var i = 0; i < _entries.length; i++)
          if (_entries[i] != null) _row(i, _entries[i]!),
      ],
    );
  }

  Widget _row(int index, ScriptEntry entry) {
    final label = formatValue(entry.meta.dataType, entry.value);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 24),
      title: Text('${widget.title} $index'),
      subtitle: Text(dataTypeLabel(entry.meta.dataType),
          style: const TextStyle(fontSize: 10, color: Colors.white54)),
      trailing: Text(label,
          style: const TextStyle(fontFamily: 'monospace', color: Colors.white70)),
      onTap: widget.editable ? () => _edit(index, entry) : null,
    );
  }
}
