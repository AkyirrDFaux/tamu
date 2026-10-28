/// Per-data-type editing popups (Docs/App/Service views/*: "values can be
/// edited when tapped"). Each type gets widgets suited to it - switches for
/// bools, sliders for ranged numbers, dropdowns for enums, component fields
/// for vectors/matrices, swatches for colours.
///
/// `info` carries optional display metadata (name, unit, range, enum labels)
/// resolved from the block registry; editors degrade gracefully without it.
///
/// This is the one value-editing entry point (TODO.md, "App UX pass", phase 1b):
/// every other editing path - the register/dynamic callers, the script-entry
/// callers and `script_value_dialog.dart` - routes through [showValueEditor],
/// and every editor it dispatches to renders through the shared dialog shell
/// (`showTamuDialog` / `PickerShell`) with the theme tokens, so no editor carries
/// its own chrome or styling.

library;

import 'dart:typed_data';
import 'package:flutter/material.dart';
import '../core/block_registry.dart';
import '../core/transform_23.dart';

export '../core/block_registry.dart' show FieldInfo;
import '../core/types.dart';
import 'block_info_picker.dart';
import 'dialogs.dart';
import 'formatters.dart';
import 'theme.dart';

part 'value_editor_scalars.dart'; // Number/Bool/Enum/DevType/Serial editors
part 'value_editor_containers.dart'; // Vector/Matrix/BlockInfo editors
part 'value_editor_visual.dart'; // Colour and transform-matrix editors

/// Returns the new encoded bytes, or null when cancelled/unchanged-invalid.
///
/// A [deviceId] routes the device-aware types to their picker (a script
/// BlockInfo operand, which is addressed by block/field/key, needs the tiered
/// register picker); without one, an editor that works off the local registry is
/// used instead, so the register and script callers share this one entry point.
Future<List<int>?> showValueEditor(
  BuildContext context,
  DataType type,
  List<int> current, {
  FieldInfo? info,
  int? deviceId,
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
      return (info?.transform == true)
          ? _editTransformMatrix(context, current)
          : _editMatrix(context, info, current);
    case DataType.colour:
      return _editColour(context, current);
    case DataType.blockInfo:
      return deviceId != null
          ? pickBlockInfo(context, deviceId, current: current)
          : _editBlockInfo(context, current);
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
    case DataType.text:
    case DataType.name:
      return _editString(
        context,
        info?.name ?? (type == DataType.name ? 'Name' : 'Text'),
        String.fromCharCodes(current),
        maxChars: type == DataType.name ? 16 : (info?.maxChars ?? 23),
        // A secret field (the WiFi password) is typed in the dark: the editor never
        // pre-fills it with what the device holds, so the value is not echoed on screen.
        secret: info?.secret ?? false,
      );
    case DataType.char:
      return _editString(
        context,
        info?.name ?? 'Char',
        String.fromCharCodes(current),
        maxChars: 1,
      );
    case DataType.sn:
      return _editSerialNumber(context, current);
    case DataType.id:
      return _editNetAddr(context, current);
    case DataType.filename:
      return _editString(
        context,
        info?.name ?? 'Filename',
        String.fromCharCodes(current),
        maxChars: 8,
      );
    case DataType.none:
      // Placeholder/deleted slot - nothing to edit.
      return null;
    default:
      return _editHex(context, dataTypeLabel(type), current);
  }
}
