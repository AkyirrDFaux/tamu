import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
export '../core/block_registry.dart' show FieldInfo;
import '../core/types.dart';

/// Per-data-type editing popups (Docs/App/Service views/*: "values can be
/// edited when tapped"). Each type gets widgets suited to it - switches for
/// bools, sliders for ranged numbers, dropdowns for enums, component fields
/// for vectors/matrices, swatches for colours.
///
/// `info` carries optional display metadata (name, unit, range, enum labels)
/// resolved from the block registry; editors degrade gracefully without it.

/// Returns the new encoded bytes, or null when cancelled/unchanged-invalid.
Future<List<int>?> showValueEditor(
  BuildContext context,
  DataType type,
  List<int> current, {
  FieldInfo? info,
}) async {
  switch (type) {
    case DataType.number:
      return _editRational(
        context,
        title: info?.name ?? 'Number',
        initial: current.length >= 4 ? numberFromBytes(current) : null,
        unit: info?.unit,
        min: info?.min,
        max: info?.max,
        step: info?.step,
      );
    case DataType.bool_:
      return _editBool(context, info?.name ?? 'Bool', current);
    case DataType.enum_:
      return _editEnum(context, info, current);
    case DataType.devType:
      return _editDevType(context, info?.name ?? 'Device type', current);
    case DataType.vector:
      return _editVector(context, info, current);
    case DataType.matrix:
      return _editMatrix(context, info, current);
    case DataType.colour:
      return _editColour(context, current);
    case DataType.uint32:
      return _editInt(
        context,
        title: valueWithUnit(info?.name ?? 'Uint32', info),
        initial: current.length >= 4 ? uint32FromBytes(current) : null,
        signed: false,
        hex: true,
      );
    case DataType.integer:
      return _editInt(
        context,
        title: valueWithUnit(info?.name ?? 'Index', info),
        initial: current.length >= 4 ? int32FromBytes(current) : null,
        signed: true,
        hex: false,
      );
    case DataType.netAddr:
      return _editNetAddr(context, current);
    case DataType.string:
      return _editString(
          context, info?.name ?? 'String', String.fromCharCodes(current),
          maxChars: info?.maxChars ?? 23);
    case DataType.sn:
      return _editSerialNumber(context, current);
    case DataType.none:
      // Placeholder/deleted slot - nothing to edit.
      return null;
    default:
      return _editHex(context, dataTypeLabel(type), current);
  }
}

/// Serial number: 14 bytes entered as 28 hex characters.
Future<List<int>?> _editSerialNumber(
    BuildContext context, List<int> current) {
  final controller = TextEditingController(
      text: current.length >= 14 ? serialNumberToHex(current) : '');
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Serial number (28 hex chars)'),
      content: TextField(
        controller: controller,
        autofocus: true,
        maxLength: 28,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final hex = controller.text.trim().toUpperCase();
            // 28 hex chars = 112 bits; int.tryParse would overflow int64, so
            // validate the whole string with a regex before parsing the bytes.
            if (!RegExp(r'^[0-9A-F]{28}$').hasMatch(hex)) {
              return;
            }
            Navigator.pop(context, [
              for (var i = 0; i < 28; i += 2)
                int.parse(hex.substring(i, i + 2), radix: 16)
            ]);
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

String dataTypeLabel(DataType type) => switch (type) {
      DataType.none => 'None',
      DataType.undefined => 'Undefined',
      DataType.sn => 'Serial number',
      DataType.uint32 => 'Uint32',
      DataType.number => 'Number',
      DataType.devType => 'Device type',
      DataType.netAddr => 'Net address',
      DataType.bool_ => 'Bool',
      DataType.vector => 'Vector',
      DataType.matrix => 'Matrix',
      DataType.enum_ => 'Enum',
      DataType.colour => 'Colour',
      DataType.integer => 'Index',
      DataType.string => 'String',
      DataType.deleted => 'Deleted',
    };

String formatValue(DataType type, List<int> bytes) {
  switch (type) {
    case DataType.number:
      if (bytes.length < 4) return '-';
      final v = numberFromBytes(bytes);
      return v == v.roundToDouble() ? v.toStringAsFixed(1) : v.toStringAsFixed(3);
    case DataType.bool_:
      if (bytes.isEmpty) return '-';
      return bytes[0] != 0 ? 'true' : 'false';
    case DataType.uint32:
      if (bytes.length < 4) return '-';
      return uint32FromBytes(bytes).toString();
    case DataType.integer:
      if (bytes.length < 4) return '-';
      return int32FromBytes(bytes).toString();
    case DataType.string:
      return bytes.isEmpty ? '-' : String.fromCharCodes(bytes);
    case DataType.devType:
      if (bytes.length < 2) return '-';
      return DeviceType.fromValue(bytes[0] | (bytes[1] << 8)).label;
    case DataType.netAddr:
      if (bytes.length < 2) return '-';
      return idToString(bytes[0] | (bytes[1] << 8));
    case DataType.colour:
      if (bytes.length < 4) return '-';
      return '#${bytes.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, '0')).join()}';
    case DataType.sn:
      if (bytes.length < 14) return '-';
      return serialNumberToHex(bytes.sublist(0, 14));
    case DataType.vector:
      if (bytes.length < 12) return '-';
      return '[${List.generate(3, (i) => _num3(numberFromBytes(bytes, i * 4))).join(', ')}]';
    case DataType.matrix:
      // Matrix buffer: uint16 height, uint16 width, then height*width Numbers.
      if (bytes.length < 8) return '-';
      final h = bytes[0] | (bytes[1] << 8);
      final w = bytes[2] | (bytes[3] << 8);
      if (h * w * 4 + 4 > bytes.length || h == 0 || w == 0) {
        return 'matrix ${h}x$w';
      }
      String cell(int r, int c) =>
          _num3(numberFromBytes(bytes, 4 + (r * w + c) * 4));
      return [for (var r = 0; r < h; r++) '[${[for (var c = 0; c < w; c++) cell(r, c)].join(' ')}]'].join(' ');
    case DataType.none:
      // Placeholder/deleted slot.
      return '∅';
    case DataType.deleted:
      return bytes.isEmpty ? '-' : _hex(bytes);
    default:
      return bytes.isEmpty ? '-' : _hex(bytes);
  }
}

// ---------------------------------------------------------------------------
// Editors
// ---------------------------------------------------------------------------

/// Number editor: slider when the field has a known range, text entry always.
Future<List<int>?> _editRational(
  BuildContext context, {
  required String title,
  double? initial,
  String? unit,
  double? min,
  double? max,
  double? step,
}) {
  final controller =
      TextEditingController(text: initial?.toStringAsFixed(2) ?? '');
  var sliderValue = (initial ?? (min ?? 0))
      .clamp(min ?? double.negativeInfinity, max ?? double.infinity)
      .toDouble();
  final hasRange = min != null && max != null && max > min;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: Text(unit == null ? title : '$title [$unit]'),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: controller,
            autofocus: true,
            keyboardType: TextInputType.number,
            decoration: InputDecoration(suffixText: unit),
            onChanged: (text) {
              final v = double.tryParse(text);
              if (v != null && hasRange) setState(() => sliderValue = v);
            },
          ),
          if (hasRange) ...[
            Slider(
              value: sliderValue.clamp(min, max),
              min: min,
              max: max,
              divisions: step != null && max > min
                  ? ((max - min) / step).round().clamp(1, 1000)
                  : null,
              label: sliderValue.toStringAsFixed(step != null && step < 1 ? 1 : 0),
              onChanged: (v) {
                setState(() {
                  sliderValue = v;
                  controller.text = v.toStringAsFixed(2);
                });
              },
            ),
            Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
              Text(min.toStringAsFixed(0), style: const TextStyle(fontSize: 11)),
              Text(max.toStringAsFixed(0), style: const TextStyle(fontSize: 11)),
            ]),
          ],
        ]),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final v = double.tryParse(controller.text);
              Navigator.pop(context, v == null ? null : numberToBytes(v));
            },
            child: const Text('OK'),
          ),
        ],
      ),
    ),
  );
}

Future<List<int>?> _editBool(
    BuildContext context, String title, List<int> current) {
  var value = current.isNotEmpty && current[0] != 0;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: Text(title),
        content: SwitchListTile(
          title: Text(value ? 'true' : 'false'),
          value: value,
          onChanged: (v) => setState(() => value = v),
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () =>
                  Navigator.pop(context, [value ? 1 : 0]),
              child: const Text('OK')),
        ],
      ),
    ),
  );
}

Future<List<int>?> _editEnum(
    BuildContext context, FieldInfo? info, List<int> current) {
  final options = info?.enumValues;
  if (options == null || options.isEmpty) {
    // No known labels: enter the numeric value directly.
    final currentRaw =
        current.length >= 4 ? uint32FromBytes(current) : null;
    final controller =
        TextEditingController(text: currentRaw?.toString() ?? '');
    return showDialog<List<int>>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Enum value (number)'),
        content:
            TextField(controller: controller, autofocus: true,
                keyboardType: TextInputType.number),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final v = int.tryParse(controller.text.trim());
              Navigator.pop(
                  context, v == null ? null : uint32ToBytes(v));
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }
  final currentRaw =
      current.length >= 4 ? uint32FromBytes(current) : null;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => SimpleDialog(
      title: Text(info?.name ?? 'Enum'),
      children: [
        RadioGroup<int>(
          groupValue: currentRaw,
          onChanged: (value) =>
              Navigator.pop(context, value == null ? null : uint32ToBytes(value)),
          child: Column(
            children: [
              for (final entry in options.entries)
                RadioListTile<int>(
                  title: Text(entry.value),
                  value: entry.key,
                ),
            ],
          ),
        ),
      ],
    ),
  );
}

Future<List<int>?> _editDevType(
    BuildContext context, String title, List<int> current) {
  final currentValue = current.length >= 2
      ? DeviceType.fromValue(current[0] | (current[1] << 8))
      : DeviceType.unknown;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => SimpleDialog(
      title: Text(title),
      children: [
        RadioGroup<DeviceType>(
          groupValue: currentValue,
          onChanged: (value) => Navigator.pop(context,
              value == null ? null : [value.value & 0xFF, value.value >> 8]),
          child: Column(
            children: [
              for (final type in DeviceType.values)
                RadioListTile<DeviceType>(
                  title: Text(type.label),
                  value: type,
                ),
            ],
          ),
        ),
      ],
    ),
  );
}

/// Vector (N Numbers): one numeric field per component.
Future<List<int>?> _editVector(
    BuildContext context, FieldInfo? info, List<int> current) {
  const n = 3; // firmware uses Vector<3>
  // A brand-new entry carries no value yet: start from zeros instead of
  // falling back to the raw-hex editor.
  final hasValue = current.length >= n * 4;
  final controllers = List.generate(
      n,
      (i) => TextEditingController(
          text: hasValue
              ? numberFromBytes(current, i * 4).toString()
              : '0.0'));
  final unit = info?.unit;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(unit == null
          ? (info?.name ?? 'Vector')
          : '${info!.name} [$unit]'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (var i = 0; i < n; i++)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: TextField(
                controller: controllers[i],
                keyboardType: TextInputType.number,
                decoration: InputDecoration(labelText: 'XYZ'[i]),
              ),
            ),
        ],
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final values =
                controllers.map((c) => double.tryParse(c.text)).toList();
            if (values.any((v) => v == null)) return;
            final out = BytesBuilder();
            for (final v in values) {
              out.add(numberToBytes(v!));
            }
            Navigator.pop(context, out.toBytes());
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

/// Matrix (Rows x Cols of Number): grid editor with the header (h, w) preserved.
Future<List<int>?> _editMatrix(
    BuildContext context, FieldInfo? info, List<int> current) {
  var h = current.length >= 2 ? current[0] | (current[1] << 8) : 0;
  var w = current.length >= 4 ? current[2] | (current[3] << 8) : 0;
  if (h == 0 || w == 0 || h * w * 4 + 4 > current.length || h > 6 || w > 6) {
    if (current.isEmpty) {
      // Brand-new entry: offer a fresh identity-like 3x3 of zeros.
      h = w = 3;
      current = [h & 0xFF, h >> 8, w & 0xFF, w >> 8,
                 for (var i = 0; i < h * w * 4; i++) 0];
    } else {
      return _editHex(context, 'Matrix ${h}x$w', current);
    }
  }
  final controllers = List.generate(
      h * w,
      (i) => TextEditingController(
          text: numberFromBytes(current, 4 + i * 4).toString()));
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(info?.name ?? 'Matrix ${h}x$w'),
      content: SizedBox(
        width: 280,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            for (var r = 0; r < h; r++)
              Row(children: [
                for (var c = 0; c < w; c++)
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.all(3),
                      child: TextField(
                        controller: controllers[r * w + c],
                        keyboardType: TextInputType.number,
                        textAlign: TextAlign.center,
                      ),
                    ),
                  ),
              ]),
          ],
        ),
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final values =
                controllers.map((c) => double.tryParse(c.text)).toList();
            if (values.any((v) => v == null)) return;
            final out = BytesBuilder()
              ..add([h & 0xFF, h >> 8, w & 0xFF, w >> 8]);
            for (final v in values) {
              out.add(numberToBytes(v!));
            }
            Navigator.pop(context, out.toBytes());
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

/// Colour: RGBA byte order on the wire (Data Formats.md); presets + hex entry.
Future<List<int>?> _editColour(BuildContext context, List<int> current) {
  final controller = TextEditingController(
      text: current.length >= 4
          ? current.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, '0')).join()
          : 'FFFFFF00');
  const presets = {
    'Off': [0, 0, 0, 0],
    'White': [255, 255, 255, 255],
    'Red': [255, 0, 0, 255],
    'Green': [0, 255, 0, 255],
    'Blue': [0, 0, 255, 255],
    'Orange': [255, 128, 0, 255],
  };
  Color preview(List<int>? rgba) => rgba == null || rgba.length < 4
      ? Colors.transparent
      : Color.fromARGB(rgba[3], rgba[0], rgba[1], rgba[2]);
  List<int>? parsed = current.length >= 4 ? current.sublist(0, 4) : null;
  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: const Text('Colour (RGBA hex)'),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          Row(children: [
            Container(
                width: 36,
                height: 36,
                decoration: BoxDecoration(
                    color: preview(parsed),
                    border: Border.all(color: Colors.white24))),
            const SizedBox(width: 12),
            Expanded(
              child: TextField(
                controller: controller,
                autofocus: true,
                maxLength: 8,
                decoration: const InputDecoration(hintText: 'RRGGBBAA'),
                onChanged: (text) {
                  final clean =
                      text.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
                  parsed = clean.length == 8
                      ? [
                          for (var i = 0; i < 8; i += 2)
                            int.parse(clean.substring(i, i + 2), radix: 16)
                        ]
                      : null;
                  setState(() {});
                },
              ),
            ),
          ]),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: [
              for (final entry in presets.entries)
                InkWell(
                  onTap: () {
                    parsed = entry.value;
                    controller.text = entry.value
                        .map((b) => b.toRadixString(16).padLeft(2, '0'))
                        .join();
                    setState(() {});
                  },
                  child: Container(
                      width: 28,
                      height: 28,
                      color: preview(entry.value)),
                ),
            ],
          ),
        ]),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, parsed),
              child: const Text('OK')),
        ],
      ),
    ),
  );
}

Future<List<int>?> _editInt(
  BuildContext context, {
  required String title,
  required bool signed,
  bool hex = false,
  int? initial,
}) {
  final controller = TextEditingController(text: initial?.toString() ?? '');
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(hex ? '$title (dec/hex)' : title),
      content: TextField(
        controller: controller,
        autofocus: true,
        keyboardType: TextInputType.number,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final text = controller.text.trim().toLowerCase();
            var value = text.startsWith('0x')
                ? int.tryParse(text.substring(2), radix: 16)
                : int.tryParse(text);
            if (value == null && hex) {
              value = int.tryParse(text.replaceAll(RegExp(r'[^0-9a-f]'), ''), radix: 16);
            }
            if (value == null) return;
            if (!signed && value < 0) return;
            Navigator.pop(context,
                uint32ToBytes(signed ? value & 0xFFFFFFFF : value));
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

Future<List<int>?> _editNetAddr(BuildContext context, List<int> current) {
  final currentId =
      current.length >= 2 ? current[0] | (current[1] << 8) : 0;
  final controller = TextEditingController(text: idToString(currentId));
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Net address (net:device)'),
      content: TextField(controller: controller, autofocus: true),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final parts = controller.text.split(':');
            final net = int.tryParse(parts[0], radix: 16);
            final dev = parts.length > 1
                ? int.tryParse(parts[1], radix: 16)
                : null;
            if (net == null || dev == null || net < 0 || dev < 0) return;
            final id = ((net & 0xF) << 12) | (dev & 0xFFF);
            Navigator.pop(context, [id & 0xFF, (id >> 8) & 0xFF]);
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

Future<List<int>?> _editString(
    BuildContext context, String title, String current,
    {int maxChars = 23}) {
  final controller = TextEditingController(text: current);
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: controller,
        autofocus: true,
        maxLength: maxChars,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
            onPressed: () => Navigator.pop(
                context, controller.text.codeUnits.toList()),
            child: const Text('OK')),
      ],
    ),
  );
}

Future<List<int>?> _editHex(
    BuildContext context, String title, List<int> current) {
  final controller = TextEditingController(
      text: current.map((b) => b.toRadixString(16).padLeft(2, '0')).join());
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text('$title (hex)'),
      content: TextField(controller: controller, autofocus: true),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final clean = controller.text.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
            if (clean.isEmpty || clean.length.isOdd) return;
            Navigator.pop(context, [
              for (var i = 0; i < clean.length; i += 2)
                int.parse(clean.substring(i, i + 2), radix: 16)
            ]);
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------

String _hex(List<int> bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');

String _num3(double v) =>
    v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toStringAsFixed(3);
