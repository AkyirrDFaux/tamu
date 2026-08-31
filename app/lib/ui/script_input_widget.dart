/// Renders one script input as its interaction control (Button / Switch / Picker /
/// Slider / Text per the input's style, resolved through "Automatic") and writes
/// live values back to the running script. Used by the expandable script rows and the
/// editor so a script can be driven without leaving the page.
///
/// Each control is a *controlled* widget: its position reflects the device's live
/// value. To prevent a toggle/drag from visibly "returning" while the write is in
/// flight (or when the script is stopped and the poll drops the live value), the
/// control keeps the value the user last set as [_pending] and shows it until the
/// device echoes it back (or a later live value differs).
library;

import 'package:flutter/material.dart';

import '../core/script_file.dart';
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;

class ScriptInputControl extends StatefulWidget {
  final int index;
  final ScriptInput input;
  final List<int>? liveValue;
  final Future<void> Function(List<int> value) onWrite;

  const ScriptInputControl({
    super.key,
    required this.index,
    required this.input,
    this.liveValue,
    required this.onWrite,
  });

  @override
  State<ScriptInputControl> createState() => _ScriptInputControlState();
}

class _ScriptInputControlState extends State<ScriptInputControl> {
  /// The value the user last set that the device has not echoed yet. While set, the
  /// control shows it instead of the possibly-stale live/default value, so a switch
  /// or slider does not "return" until the device confirms.
  List<int>? _pending;

  /// Controller for the text input tile. Created once and synced in didUpdateWidget
  /// only when the value changes from the device side (not from user typing).
  late TextEditingController _textController;

  List<int> get _current =>
      _pending ?? widget.liveValue ?? widget.input.defaultValue;

  DataType get _type => widget.input.dataType;

  InputStyle get _style =>
      widget.input.style == InputStyle.automatic
          ? InputStyle.forType(_type)
          : widget.input.style;

  @override
  void initState() {
    super.initState();
    _textController = TextEditingController(text: String.fromCharCodes(_current));
  }

  @override
  void dispose() {
    _textController.dispose();
    super.dispose();
  }

  void _write(List<int> value) {
    setState(() => _pending = value);
    widget.onWrite(value);
  }

  static bool _same(List<int> a, List<int> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  @override
  void didUpdateWidget(covariant ScriptInputControl oldWidget) {
    super.didUpdateWidget(oldWidget);
    final live = widget.liveValue;
    if (_pending != null) {
      if (live != null && _same(_pending!, live)) {
        setState(() => _pending = null); // the device echoed our value
      }
      // Otherwise keep the pending value - the device has not confirmed it yet.
    }
    // Sync the text controller when the value changes from the device side
    // (not from user typing — _pending stays non-null while typing).
    if (_pending == null) {
      final newText = String.fromCharCodes(_current);
      if (_textController.text != newText) {
        _textController.text = newText;
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final label = 'In${widget.index} · ${dataTypeLabel(_type)}';
    switch (_style) {
      case InputStyle.button:
        return ListTile(
          dense: true,
          contentPadding: EdgeInsets.zero,
          leading:
              const Icon(Icons.touch_app_outlined, size: 18, color: kOrange),
          title: Text(label, style: const TextStyle(fontSize: 13)),
          trailing: FilledButton(
            style: FilledButton.styleFrom(
                visualDensity: VisualDensity.compact,
                padding: const EdgeInsets.symmetric(horizontal: 14)),
            child: const Text('Press'),
            onPressed: () => _write(_pressValue()),
          ),
        );
      case InputStyle.switch_:
        final on = _current.isNotEmpty && _current[0] != 0;
        return SwitchListTile(
          dense: true,
          contentPadding: EdgeInsets.zero,
          secondary:
              const Icon(Icons.toggle_on_outlined, size: 18, color: kOrange),
          title: Text(label, style: const TextStyle(fontSize: 13)),
          value: on,
          onChanged: (v) => _write(v ? [1] : [0]),
        );
      case InputStyle.picker:
        return ListTile(
          dense: true,
          contentPadding: EdgeInsets.zero,
          leading: const Icon(Icons.arrow_drop_down_circle_outlined,
              size: 18, color: kOrange),
          title: Text(label, style: const TextStyle(fontSize: 13)),
          trailing: DropdownButton<int>(
            value: _current.isEmpty ? 0 : (_current[0] & 0x07),
            items: [
              for (var v = 0; v < 8; v++)
                DropdownMenuItem(value: v, child: Text('$v')),
            ],
            onChanged: (v) {
              if (v != null) _write([v]);
            },
          ),
        );
      case InputStyle.slider:
        return _sliderTile(context, label);
      case InputStyle.text:
        return _textTile(context, label);
      default:
        // Unsupported style/type combination: fall back to the value editor.
        return ListTile(
          dense: true,
          contentPadding: EdgeInsets.zero,
          leading: const Icon(Icons.edit_note, size: 18, color: kOrange),
          title: Text(label, style: const TextStyle(fontSize: 13)),
          subtitle: Text(_formatCurrent(),
              style: const TextStyle(fontFamily: 'monospace', fontSize: 11)),
          trailing: TextButton(
            onPressed: () async {
              final value = await showValueEditor(context, _type, _current);
              if (value != null) _write(value);
            },
            child: const Text('Set'),
          ),
        );
    }
  }

  Widget _sliderTile(BuildContext context, String label) {
    final (double min, double max, double current) = switch (_type) {
      DataType.number => (
          0.0,
          100.0,
          _current.length >= 4 ? numberFromBytes(_current) : 0.0
        ),
      DataType.integer => (
          0.0,
          255.0,
          _current.length >= 4 ? int32FromBytes(_current).toDouble() : 0.0
        ),
      DataType.uint32 => (
          0.0,
          255.0,
          _current.length >= 4 ? uint32FromBytes(_current).toDouble() : 0.0
        ),
      _ => (0.0, 100.0, 0.0),
    };
    // A dedicated full-width row (not a ListTile trailing) so the Slider always has
    // full height and a clean hit area.
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(
        children: [
          const Icon(Icons.tune, size: 18, color: kOrange),
          const SizedBox(width: 8),
          Expanded(child: Text(label, style: const TextStyle(fontSize: 13))),
          const SizedBox(width: 8),
          SizedBox(
            width: 180,
            height: 44,
            child: _ScriptSlider(
              min: min,
              max: max,
              current: current,
              onCommit: (v) => _write(_encodeScalar(v)),
            ),
          ),
        ],
      ),
    );
  }

  Widget _textTile(BuildContext context, String label) {
    return ListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      leading: const Icon(Icons.text_fields, size: 18, color: kOrange),
      title: Text(label, style: const TextStyle(fontSize: 13)),
      subtitle: TextField(
        controller: _textController,
        style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
        onSubmitted: (v) => _write(v.codeUnits),
      ),
    );
  }

  String _formatCurrent() {
    try {
      return formatValue(_type, _current);
    } catch (_) {
      return _current.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    }
  }

  /// Value written by a Button press: a momentary true/1 for the expected type.
  List<int> _pressValue() => switch (_type) {
        DataType.bool_ => [1],
        DataType.number => numberToBytes(1.0),
        DataType.integer => [1, 0, 0, 0],
        DataType.uint32 => [1, 0, 0, 0],
        _ => [1],
      };

  List<int> _encodeScalar(double v) => switch (_type) {
        DataType.number => numberToBytes(v),
        DataType.integer => int32ToBytes(v.round()),
        DataType.uint32 => uint32ToBytes(v.round()),
        _ => [v.round() & 0xFF],
      };
}

/// A slider whose thumb follows the finger while dragging (local state) and commits
/// on release (and throttled during the drag). The parent's [_ScriptInputControlState]
/// pending value keeps `current` stable until the device echoes, so the thumb does
/// not snap back.
class _ScriptSlider extends StatefulWidget {
  final double min;
  final double max;
  final double current;
  final ValueChanged<double> onCommit;

  const _ScriptSlider({
    required this.min,
    required this.max,
    required this.current,
    required this.onCommit,
  });

  @override
  State<_ScriptSlider> createState() => _ScriptSliderState();
}

class _ScriptSliderState extends State<_ScriptSlider> {
  late double _value;
  bool _dragging = false;

  /// Throttles the continuous commits during a drag.
  DateTime _lastCommit = DateTime.fromMillisecondsSinceEpoch(0);

  @override
  void initState() {
    super.initState();
    _value = widget.current.clamp(widget.min, widget.max);
  }

  @override
  void didUpdateWidget(covariant _ScriptSlider oldWidget) {
    super.didUpdateWidget(oldWidget);
    // While the user drags, keep their position; otherwise track the (stable,
    // pending-guarded) current value.
    if (!_dragging) {
      _value = widget.current.clamp(widget.min, widget.max);
    }
  }

  void _commit(double v) {
    _lastCommit = DateTime.now();
    widget.onCommit(v);
  }

  @override
  Widget build(BuildContext context) {
    return Slider(
      min: widget.min,
      max: widget.max,
      value: _value.clamp(widget.min, widget.max),
      label: _value.toStringAsFixed(1),
      onChanged: (v) {
        setState(() {
          _dragging = true;
          _value = v;
        });
        // Commit continuously (throttled) so the script follows the drag.
        if (DateTime.now().difference(_lastCommit).inMilliseconds >= 80) {
          _commit(v);
        }
      },
      onChangeEnd: (v) {
        setState(() => _dragging = false);
        _commit(v);
      },
    );
  }
}