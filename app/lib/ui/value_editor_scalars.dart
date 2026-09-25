part of 'value_editor.dart';

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

/// Human-readable data-type name (shared with the semantic backup format, so a label
/// and a stored type word never drift apart).
String dataTypeLabel(DataType type) => dataTypeWord(type);

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
      if (bytes.isEmpty) return '-';
      if (bytes.length < 4) {
        // Short (1-3 byte) integer values, e.g. the uint8 edge counter.
        return bytesToInt(bytes).toString();
      }
      return int32FromBytes(bytes).toString();
    case DataType.string:
      return bytes.isEmpty ? '-' : String.fromCharCodes(bytes).trimRight();
    case DataType.devType:
      if (bytes.length < 2) return '-';
      return DeviceType.fromValue(bytes[0] | (bytes[1] << 8)).label;
    case DataType.id:
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
      // Size-flexible: format however many 4-byte Numbers are present (Vector2/3/N).
      if (bytes.length < 4) return '-';
      final n = bytes.length ~/ 4;
      return '[${List.generate(n, (i) => _num3(numberFromBytes(bytes, i * 4))).join(', ')}]';
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
  final valueSize = current.isNotEmpty ? current.length : 4;
  if (options == null || options.isEmpty) {
    // No known labels: enter the numeric value directly.
    final currentRaw = bytesToInt(current);
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
                  context, v == null ? null : intToBytes(v, valueSize));
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }
  final currentRaw = bytesToInt(current);
  return showDialog<List<int>>(
    context: context,
    builder: (context) => SimpleDialog(
      title: Text(info?.name ?? 'Enum'),
      children: [
        RadioGroup<int>(
          groupValue: currentRaw,
          onChanged: (value) =>
              Navigator.pop(context, value == null ? null : intToBytes(value, valueSize)),
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

