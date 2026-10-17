part of 'value_editor.dart';

/// Serial number: 14 bytes entered as 28 hex characters.
Future<List<int>?> _editSerialNumber(BuildContext context, List<int> current) {
  final controller = TextEditingController(
    text: current.length >= 14 ? serialNumberToHex(current) : '',
  );
  return showTamuDialog<List<int>>(
    context,
    title: 'Serial number (28 hex chars)',
    body: TextField(controller: controller, autofocus: true, maxLength: 28),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final hex = controller.text.trim().toUpperCase();
        // 28 hex chars = 112 bits; int.tryParse would overflow int64, so
        // validate the whole string with a regex before parsing the bytes.
        if (!RegExp(r'^[0-9A-F]{28}$').hasMatch(hex)) {
          return;
        }
        Navigator.pop(context, [
          for (var i = 0; i < 28; i += 2)
            int.parse(hex.substring(i, i + 2), radix: 16),
        ]);
      },
    ),
  );
}

// Value formatting (dataTypeLabel, formatValue) lives in formatters.dart.

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
  final styles = Theme.of(context).textTheme;
  final controller = TextEditingController(
    text: initial?.toStringAsFixed(2) ?? '',
  );
  var sliderValue = (initial ?? (min ?? 0))
      .clamp(min ?? double.negativeInfinity, max ?? double.infinity)
      .toDouble();
  final hasRange = min != null && max != null && max > min;
  return showTamuDialog<List<int>>(
    context,
    title: unit == null ? title : '$title [$unit]',
    body: StatefulBuilder(
      builder: (context, setState) => Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          TextField(
            controller: controller,
            autofocus: true,
            keyboardType: TextInputType.number,
            decoration: InputDecoration(suffixText: unit),
            onChanged: (text) {
              final v = double.tryParse(text);
              if (v != null && hasRange) {
                setState(() => sliderValue = v);
              }
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
              label: sliderValue.toStringAsFixed(
                step != null && step < 1 ? 1 : 0,
              ),
              onChanged: (v) {
                setState(() {
                  sliderValue = v;
                  controller.text = v.toStringAsFixed(2);
                });
              },
            ),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(min.toStringAsFixed(0), style: styles.labelSmall),
                Text(max.toStringAsFixed(0), style: styles.labelSmall),
              ],
            ),
          ],
        ],
      ),
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final v = double.tryParse(controller.text);
        Navigator.pop(context, v == null ? null : numberToBytes(v));
      },
    ),
  );
}

Future<List<int>?> _editBool(
  BuildContext context,
  String title,
  List<int> current,
) {
  var value = current.isNotEmpty && current[0] != 0;
  return showTamuDialog<List<int>>(
    context,
    title: title,
    body: StatefulBuilder(
      builder: (context, setState) => SwitchListTile(
        title: Text(value ? 'true' : 'false'),
        value: value,
        onChanged: (v) => setState(() => value = v),
      ),
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () => Navigator.pop(context, [value ? 1 : 0]),
    ),
  );
}

Future<List<int>?> _editEnum(
  BuildContext context,
  FieldInfo? info,
  List<int> current,
) {
  final options = info?.enumValues;
  final valueSize = current.isNotEmpty ? current.length : 4;
  if (options == null || options.isEmpty) {
    // No known labels: enter the numeric value directly.
    final currentRaw = bytesToInt(current);
    final controller = TextEditingController(
      text: currentRaw?.toString() ?? '',
    );
    return showTamuDialog<List<int>>(
      context,
      title: 'Enum value (number)',
      body: TextField(
        controller: controller,
        autofocus: true,
        keyboardType: TextInputType.number,
      ),
      actions: tamuDialogActions(
        onCancel: () => Navigator.pop(context),
        onConfirm: () {
          final v = int.tryParse(controller.text.trim());
          Navigator.pop(context, v == null ? null : intToBytes(v, valueSize));
        },
      ),
    );
  }
  final currentRaw = bytesToInt(current);
  return showTamuPicker<List<int>>(
    context,
    PickerShell(
      title: info?.name ?? 'Enum',
      body: RadioGroup<int>(
        groupValue: currentRaw,
        onChanged: (value) => Navigator.pop(
          context,
          value == null ? null : intToBytes(value, valueSize),
        ),
        child: Column(
          children: [
            for (final entry in options.entries)
              RadioListTile<int>(title: Text(entry.value), value: entry.key),
          ],
        ),
      ),
    ),
  );
}

Future<List<int>?> _editDevType(
  BuildContext context,
  String title,
  List<int> current,
) {
  final currentValue = current.length >= 2
      ? DeviceType.fromValue(current[0] | (current[1] << 8))
      : DeviceType.unknown;
  return showTamuPicker<List<int>>(
    context,
    PickerShell(
      title: title,
      body: RadioGroup<DeviceType>(
        groupValue: currentValue,
        onChanged: (value) => Navigator.pop(
          context,
          value == null ? null : [value.value & 0xFF, value.value >> 8],
        ),
        child: Column(
          children: [
            for (final type in DeviceType.values)
              RadioListTile<DeviceType>(title: Text(type.label), value: type),
          ],
        ),
      ),
    ),
  );
}
