/// Shared script UI pieces (Docs/App/Service views/Script.md): state chips and the I/O
/// tables of a loaded script. Inputs are rendered using their UI specification (sliders,
/// toggles, buttons - docs: "formatted with the UI specifications").
library;

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_file.dart';
import '../core/types.dart';
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

/// One category (inputs or outputs) of a loaded script's Register entries. Inputs are
/// editable, outputs are read-only.
class ScriptIoSection extends StatefulWidget {
  final ScriptClient client;
  final int slot;
  final int field;
  final String title;
  final IconData icon;
  final bool editable;

  /// Per-input UI specifications (inputs only), read from the script file.
  final List<ScriptInputSpec>? specs;

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
    this.specs,
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
    // Declare the actual value length (a shorter String is space-padded by the firmware);
    // the field's declared Size would make the device copy stale payload bytes.
    final meta = BlockMeta(
        flagsAndType: entry.meta.flagsAndType, size: next.length, key: entry.meta.key);
    final ok = await widget.client.writeEntry(widget.slot, widget.field, key, meta, next);
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

  ScriptInputSpec? _specFor(int index) {
    final specs = widget.specs;
    if (widget.field != ScriptField.input || specs == null || index >= specs.length) return null;
    return specs[index];
  }

  Widget _row(int index, ScriptEntry entry) {
    final spec = _specFor(index);
    if (widget.editable && spec != null) {
      final isSlider = spec.uiType == ScriptUiType.slider &&
          spec.max > spec.min &&
          entry.meta.dataType == DataType.number;
      final isToggle =
          spec.uiType == ScriptUiType.toggle && entry.meta.dataType == DataType.bool_;
      final isButton =
          spec.uiType == ScriptUiType.button && entry.meta.dataType == DataType.bool_;
      if (isSlider || isToggle || isButton) {
        return ScriptInputControl(
          client: widget.client,
          slot: widget.slot,
          inputIndex: index,
          entry: entry,
          spec: spec,
          title: '${widget.title} $index',
          onChanged: widget.onChanged,
        );
      }
    }
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

/// An input rendered per its UI specification: a slider, a toggle or a momentary button.
class ScriptInputControl extends StatefulWidget {
  final ScriptClient client;
  final int slot;
  final int inputIndex;
  final ScriptEntry entry;
  final ScriptInputSpec spec;
  final String title;
  final VoidCallback? onChanged;

  const ScriptInputControl({
    super.key,
    required this.client,
    required this.slot,
    required this.inputIndex,
    required this.entry,
    required this.spec,
    required this.title,
    this.onChanged,
  });

  @override
  State<ScriptInputControl> createState() => _ScriptInputControlState();
}

class _ScriptInputControlState extends State<ScriptInputControl> {
  double _value = 0;
  bool _dragging = false;

  @override
  void initState() {
    super.initState();
    _syncFromEntry();
  }

  @override
  void didUpdateWidget(covariant ScriptInputControl oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Re-sync with the device value on refresh, but not while the user is dragging.
    if (!_dragging) _syncFromEntry();
  }

  void _syncFromEntry() {
    final v = widget.entry.value;
    _value = v.length >= 4 ? numberFromBytes(v) : (v.isNotEmpty ? v.first.toDouble() : 0);
  }

  bool get _bool => widget.entry.value.isNotEmpty && widget.entry.value.first != 0;

  Future<void> _write(List<int> bytes) async {
    final ok = await widget.client
        .writeEntry(widget.slot, ScriptField.input, widget.inputIndex, widget.entry.meta, bytes);
    if (!mounted) return;
    if (!ok) showSnack(context, 'Write failed');
    widget.onChanged?.call();
  }

  @override
  Widget build(BuildContext context) {
    switch (widget.spec.uiType) {
      case ScriptUiType.slider:
        final min = widget.spec.min;
        final max = widget.spec.max;
        final step = widget.spec.step > 0 ? widget.spec.step : (max - min) / 100;
        final divisions = ((max - min) / step).round().clamp(1, 1000);
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: 24),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Expanded(
                  child: Text(widget.title, style: const TextStyle(fontSize: 13))),
              Text(_value.toStringAsFixed(2),
                  style: const TextStyle(fontFamily: 'monospace', color: Colors.white70)),
            ]),
            Slider(
              value: _value.clamp(min, max),
              min: min,
              max: max,
              divisions: divisions,
              onChangeStart: (_) => _dragging = true,
              onChanged: (v) => setState(() => _value = v),
              onChangeEnd: (v) {
                _dragging = false;
                _write(numberToBytes(v));
              },
            ),
          ]),
        );
      case ScriptUiType.toggle:
        return SwitchListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: 24),
          title: Text(widget.title, style: const TextStyle(fontSize: 13)),
          value: _bool,
          onChanged: (v) => _write([v ? 1 : 0]),
        );
      case ScriptUiType.button:
        return ListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: 24),
          title: Text(widget.title, style: const TextStyle(fontSize: 13)),
          trailing: FilledButton(onPressed: () => _write([1]), child: const Text('Press')),
        );
      default:
        return const SizedBox.shrink();
    }
  }
}
