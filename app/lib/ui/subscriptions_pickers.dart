// Block/field/key/provider pickers used by the subscription dialog.
//
// Part of subscriptions_dialog.dart (same library, so the pickers can use its
// models and the dialog can open them).

part of 'subscriptions_dialog.dart';

class BlockPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<BlockSelection> blocks;
  final BlockSelection? value;
  final ValueChanged<BlockSelection?> onChanged;

  const BlockPicker({
    super.key,
    required this.label,
    required this.hint,
    required this.blocks,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (blocks.isEmpty) {
      return InputDecorator(
        decoration: InputDecoration(
          labelText: label,
          hintText: hint,
          border: const OutlineInputBorder(),
        ),
        child: const Text('No blocks available'),
      );
    }

    return DropdownButtonFormField<BlockSelection>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: blocks.map((opt) => DropdownMenuItem(
        value: opt,
        child: Text(opt.label, overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
      menuMaxHeight: 300,
    );
  }
}

class FieldPicker extends StatelessWidget {
  final String label;
  final List<FieldSelection> fields;
  final FieldSelection? value;
  final ValueChanged<FieldSelection?> onChanged;

  const FieldPicker({
    super.key,
    required this.label,
    required this.fields,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (fields.isEmpty) {
      return const InputDecorator(
        decoration: InputDecoration(
          labelText: 'Field',
          hintText: 'No fields available',
          border: OutlineInputBorder(),
        ),
        child: Text('No fields available'),
      );
    }

    return DropdownButtonFormField<FieldSelection>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: fields.map((opt) => DropdownMenuItem(
        value: opt,
        child: Text('${opt.name}  (${opt.keyed ? "keyed" : opt.meta.dataType.name})', overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
      menuMaxHeight: 300,
    );
  }
}

class KeyPicker extends StatelessWidget {
  final String label;
  final int value;

  /// The keys the field actually offers (dictionary markers omit key 0). Falls back to 0..7.
  final List<int> keys;
  final ValueChanged<int> onChanged;

  const KeyPicker({
    super.key,
    required this.label,
    required this.value,
    required this.onChanged,
    this.keys = const [0, 1, 2, 3, 4, 5, 6, 7],
  });

  @override
  Widget build(BuildContext context) {
    // Always include the current value so an existing entry never asserts the dropdown.
    final effective = <int>[...(keys.isEmpty ? const [0] : keys)];
    if (!effective.contains(value)) effective.add(value);
    effective.sort();
    return DropdownButtonFormField<int>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: effective
          .map((k) => DropdownMenuItem(value: k, child: Text('Key $k')))
          .toList(),
      onChanged: (v) {
        if (v != null) onChanged(v);
      },
    );
  }
}

class ProviderPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<DeviceEntry> devices;
  final int? value;
  final ValueChanged<int?> onChanged;

  const ProviderPicker({
    super.key,
    required this.label,
    required this.hint,
    required this.devices,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (devices.isEmpty) {
      return InputDecorator(
        decoration: InputDecoration(
          labelText: label,
          hintText: 'No other devices found',
          border: const OutlineInputBorder(),
        ),
        child: const Text('No other devices found'),
      );
    }

    return DropdownButtonFormField<int>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: devices.map((d) => DropdownMenuItem(
        value: d.id,
        child: Text('${d.displayName} (ID: ${d.id})', overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
    );
  }
}

class NumberField extends StatelessWidget {
  final String label;
  final TextEditingController controller;
  final ValueChanged<int> onChanged;
  final int min;

  const NumberField({
    super.key,
    required this.label,
    required this.controller,
    required this.onChanged,
    this.min = 0,
  });

  @override
  Widget build(BuildContext context) {
    return TextFormField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      keyboardType: TextInputType.number,
      onChanged: (v) {
        final parsed = int.tryParse(v) ?? min;
        onChanged(parsed.clamp(min, 0x7FFFFFFF));
      },
      validator: (v) {
        final parsed = int.tryParse(v ?? '');
        if (parsed == null || parsed < min) return 'Enter a valid number (min $min)';
        return null;
      },
    );
  }
}
