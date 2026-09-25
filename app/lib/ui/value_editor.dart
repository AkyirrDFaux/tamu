/// Per-data-type editing popups (Docs/App/Service views/*: "values can be
/// edited when tapped"). Each type gets widgets suited to it - switches for
/// bools, sliders for ranged numbers, dropdowns for enums, component fields
/// for vectors/matrices, swatches for colours.
///
/// `info` carries optional display metadata (name, unit, range, enum labels)
/// resolved from the block registry; editors degrade gracefully without it.

library;

import 'dart:typed_data';
import 'package:flutter/material.dart';
import '../core/block_registry.dart';
import '../core/transform_23.dart';

export '../core/block_registry.dart' show FieldInfo;
import '../core/types.dart';
import 'widgets.dart';




part 'value_editor_scalars.dart'; // Number/Bool/Enum/DevType/Serial editors
part 'value_editor_containers.dart'; // Vector/Matrix/BlockInfo editors
part 'value_editor_visual.dart'; // Colour and transform-matrix editors

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
      return (info?.transform == true)
          ? _editTransformMatrix(context, current)
          : _editMatrix(context, info, current);
    case DataType.colour:
      return _editColour(context, current);
    case DataType.blockInfo:
      return _editBlockInfo(context, current);
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
    case DataType.id:
      return _editNetAddr(context, current);
    case DataType.filename:
      return _editString(context, info?.name ?? 'Filename', String.fromCharCodes(current), maxChars: 8);
    case DataType.none:
      // Placeholder/deleted slot - nothing to edit.
      return null;
    default:
      return _editHex(context, dataTypeLabel(type), current);
  }
}

