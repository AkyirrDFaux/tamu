import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/render_dict.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/script_instructions.dart';

/// Guards the app<->firmware script *encoding* contract.
///
/// The 4-byte symbol stream is a private convention implemented twice: the editor writes it
/// (`lib/core/script_instructions.dart`, `script_file.dart`) and the VM reads it
/// (`firmware/src/Core/Services/ScriptDefs.h`). Nothing else would notice a drift in a category,
/// an opcode or the header size - the file would parse, the values would mean something else.
/// This compares the two tables directly.
///
/// The firmware tree is read from disk, so the test skips cleanly when the app is checked out
/// or tested on its own.
Map<String, int> _firmwareDefines() {
  final candidates = [
    '../firmware/src/Core/Services/ScriptDefs.h',
    '../../firmware/src/Core/Services/ScriptDefs.h',
  ];
  for (final path in candidates) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final defines = <String, int>{};
    final re = RegExp(r'^#define\s+(SCRIPT_\w+|MAX_SCRIPTS)\s+(-?\d+)', multiLine: true);
    for (final m in re.allMatches(file.readAsStringSync())) {
      defines[m.group(1)!] = int.parse(m.group(2)!);
    }
    return defines;
  }
  return const {};
}

/// Reads one enumerator body from a firmware header, as `name -> value`.
///
/// Handles both forms used in `Enums.h`: `enum class Name : uint16_t { ... }` and
/// `enum Name : uint16_t { ... }`.
Map<String, int> _firmwareEnum(String relativePath, String enumName) {
  final candidates = ['../firmware/$relativePath', '../../firmware/$relativePath'];
  for (final path in candidates) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    final header =
        RegExp('\\benum\\s+(?:class\\s+)?$enumName\\b[^{]*\\{').firstMatch(source);
    if (header == null) continue;
    final open = header.end - 1;
    final close = source.indexOf('};', open);
    if (close < 0) continue;
    final body = source.substring(open + 1, close);
    final values = <String, int>{};
    final re = RegExp(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)');
    for (final m in re.allMatches(body)) {
      final raw = m.group(2)!;
      values[m.group(1)!] = raw.startsWith('0x')
          ? int.parse(raw.substring(2), radix: 16)
          : int.parse(raw);
    }
    return values;
  }
  return const {};
}

/// Compares an app enum (`name -> value`) against a firmware enum.
///
/// [alias] maps app names onto the firmware spelling where they differ; [firmwareOnly] lists
/// firmware members (aliases such as `Unknown`/`System`) that deliberately have no app entry;
/// [appOnly] lists app members that are not real firmware enum values (pseudo types). Every
/// name on either side must be accounted for, so a new enum member cannot slip through.
void _expectEnumMatches(
  Map<String, int> app,
  Map<String, int> firmware, {
  Map<String, String> alias = const {},
  Set<String> firmwareOnly = const {},
  Set<String> appOnly = const {},
}) {
  String norm(String s) => s.toLowerCase().replaceAll(RegExp('[^a-z0-9]'), '');
  final fwByName = {for (final e in firmware.entries) norm(e.key): e.value};
  final seen = <String>{};

  for (final entry in app.entries) {
    final target = norm(alias[entry.key] ?? entry.key);
    if (appOnly.contains(norm(entry.key))) continue;
    expect(fwByName.containsKey(target), isTrue,
        reason: 'firmware enum has no member for "${entry.key}"');
    expect(fwByName[target], entry.value, reason: '"${entry.key}" value');
    seen.add(target);
  }

  for (final name in fwByName.keys) {
    if (seen.contains(name)) continue;
    expect(firmwareOnly.contains(name), isTrue,
        reason: 'app enum has no member for the firmware entry "$name"');
  }
}

/// Compares an app value->label map against a firmware `Name = value` enum. Labels such as
/// "Double Parabola" and the firmware's `DoubleParabola` are the same name.
void _expectSameEnum(Map<int, String> app, Map<String, int> firmware) {
  String norm(String s) => s.toLowerCase().replaceAll(RegExp('[^a-z0-9]'), '');
  expect(firmware.length, app.length, reason: 'enum size differs');
  final byName = {for (final e in firmware.entries) norm(e.key): e.value};
  for (final entry in app.entries) {
    final key = norm(entry.value);
    expect(byName.containsKey(key), isTrue,
        reason: 'firmware has no enum member for "${entry.value}"');
    expect(byName[key], entry.key, reason: '${entry.value} numbering');
  }
}

void main() {
  final fw = _firmwareDefines();
  final skipReason = fw.isEmpty ? 'firmware source not available next to the app' : null;

  group('script encoding contract (app vs firmware)', () {
    test('the symbol-type enum matches', () {
      expect(fw['SCRIPT_SYM_INSTRUCTION'], symInstruction);
      expect(fw['SCRIPT_SYM_INPUT'], symInput);
      expect(fw['SCRIPT_SYM_OUTPUT'], symOutput);
      expect(fw['SCRIPT_SYM_VARIABLE'], symVariable);
      expect(fw['SCRIPT_SYM_CONSTANT'], symConstant);
      expect(fw['SCRIPT_SYM_ENDLINE'], symEndline);
      expect(fw['SCRIPT_SYM_PREDEFINE'], symPredefine);
    });

    test('the predefine-subtype enum matches', () {
      expect(fw['SCRIPT_PRE_STATE'], preState);
      expect(fw['SCRIPT_PRE_TYPE'], preType);
      expect(fw['SCRIPT_PRE_INDEX'], preIndex);
      expect(fw['SCRIPT_PRE_CHAR'], preChar);
      expect(fw['SCRIPT_PRE_MATHOP'], preMathOp);
      expect(fw['SCRIPT_PRE_BOOL'], preBool);
      expect(fw['SCRIPT_PRE_NUMBER'], preNumber);
    });

    test('the instruction-category enum matches', () {
      expect(fw['SCRIPT_CAT_MATH'], catMath);
      expect(fw['SCRIPT_CAT_LOGIC'], catLogic);
      expect(fw['SCRIPT_CAT_FLOW'], catFlow);
      expect(fw['SCRIPT_CAT_TIME'], catTime);
      expect(fw['SCRIPT_CAT_SERVICE'], catService);
      expect(fw['SCRIPT_CAT_COMPOSE'], catCompose);
    });

    test('every inline math operator the editor offers is known to the VM', () {
      // The VM implements one SCRIPT_MATHOP_* name per operator value.
      const expected = {
        0: 'ADD',
        1: 'SUB',
        2: 'MUL',
        3: 'DIV',
        4: 'MOD',
        5: 'POW',
        6: 'AND',
        7: 'OR',
        8: 'XOR',
        9: 'NOT',
        12: 'EQ',
        13: 'NE',
        14: 'LT',
        15: 'LE',
        16: 'GT',
        17: 'GE',
        18: 'OPEN',
        19: 'CLOSE',
        20: 'FN_DOT',
        21: 'FN_CROSS',
        22: 'FN_SIZE',
        23: 'FN_TRANSPOSE',
      };
      for (final entry in expected.entries) {
        expect(fw['SCRIPT_MATHOP_${entry.value}'], entry.key,
            reason: 'math op ${entry.key} (${entry.value})');
      }
      // And the editor must not offer anything outside that set.
      for (final op in expressionOps) {
        expect(expected.containsKey(op), isTrue, reason: 'editor offers op $op');
      }
    });

    test('the instruction opcodes match the VM', () {
      // (category define prefix, op -> firmware suffix)
      const mathOps = {
        0: 'SET',
        5: 'MOD',
        6: 'MIN',
        7: 'MAX',
        9: 'ABS',
        10: 'LIMIT',
        11: 'TRANSFORM',
      };
      const flowOps = {
        0: 'IF',
        1: 'WHILE',
        2: 'END',
        3: 'JUMP',
        4: 'CALL',
        5: 'RETURN',
        6: 'HALT',
      };
      const timeOps = {0: 'DELAY', 1: 'WAIT', 2: 'GET'};
      const serviceOps = {
        0: 'LOG',
        1: 'REG_READ',
        2: 'REG_WRITE',
        3: 'STATE',
        4: 'NOP',
        5: 'REG_READ_FOREIGN',
        6: 'REG_WRITE_FOREIGN',
        7: 'SCRIPT_LOAD',
        8: 'SCRIPT_UNLOAD',
      };
      const composeOps = {0: 'COMPOSE', 1: 'EXTRACT'};

      const tables = {
        catMath: ('SCRIPT_OP_MATH_', mathOps),
        catFlow: ('SCRIPT_OP_FLOW_', flowOps),
        catTime: ('SCRIPT_OP_TIME_', timeOps),
        catService: ('SCRIPT_OP_SERVICE_', serviceOps),
        // The compose pair is not category-infix named in the firmware.
        catCompose: ('SCRIPT_OP_', composeOps),
      };

      for (final def in scriptInstructions) {
        final table = tables[def.category];
        if (table == null) continue; // logic (Select) is checked below
        final (prefix, ops) = table;
        expect(ops[def.op], isNotNull,
            reason: '${def.label}: op ${def.op} has no firmware define');
        expect(fw['$prefix${ops[def.op]}'], def.op,
            reason: '${def.label}: $prefix${ops[def.op]}');
      }

      expect(fw['SCRIPT_OP_LOGIC_SELECT'], 12);
    });

    test('the file-format sizes match', () {
      expect(fw['MAX_SCRIPTS'], maxScripts);
      expect(fw['SCRIPT_HEADER_SIZE'], scriptHeaderSize);
    });
  }, skip: skipReason);

  group('core enum contract (app vs firmware)', () {
    Map<String, int> names<T>(List<({String name, int value})> entries) =>
        {for (final e in entries) e.name: e.value};

    test('DeviceType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'DeviceType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([for (final t in DeviceType.values) (name: t.name, value: t.value)]),
        fw,
      );
    });

    test('DataType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'DataType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([for (final t in DataType.values) (name: t.name, value: t.value)]),
        fw,
        // The app spells Index/Enum/Bool differently (Dart keywords).
        alias: const {'integer': 'Index'},
        // Firmware aliases with no distinct app entry.
        firmwareOnly: const {'unknown', 'unknownkeyed'},
      );
    });

    test('BlockType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'BlockType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([for (final t in BlockType.values) (name: t.name, value: t.value)]),
        fw,
        alias: const {'vysiDisplay': 'Vysi1Display'},
        firmwareOnly: const {'system'},
        // Script blocks are exposed through the Register (0x3FE) and "render" is the app's
        // pseudo grouping for the 0x101/0x102 dictionary types - neither is a BlockType in
        // the firmware's own enum.
        appOnly: const {'script', 'render'},
      );
    });

    test('TriggerType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'TriggerType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([for (final t in TriggerType.values) (name: t.name, value: t.value)]),
        fw,
      );
    });

    test('FieldFlags matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'FieldFlags');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        const {
          'readOnly': FieldFlags.readOnly,
          'persistent': FieldFlags.persistent,
          'trigger': FieldFlags.trigger,
          'notSaved': FieldFlags.notSaved,
          'scriptUpdated': FieldFlags.scriptUpdated,
          'external': FieldFlags.external,
        },
        fw,
        firmwareOnly: const {'none'},
      );
      // The flag mask the app applies to FlagsAndType.
      expect(fw['ReadOnly'], FieldFlags.readOnly);
      expect(FieldFlags.mask, 0xFC00);
    });

    test('Capability bits match', () {
      final source = File('../firmware/src/Core/Types/Enums.h');
      if (!source.existsSync()) return;
      final fw = <String, int>{};
      final re = RegExp(r'constexpr\s+uint32_t\s+(\w+)\s*=\s*1u\s*<<\s*(\d+)');
      for (final m in re.allMatches(source.readAsStringSync())) {
        fw[m.group(1)!] = 1 << int.parse(m.group(2)!);
      }
      expect(fw, isNotEmpty);
      _expectEnumMatches(const {
        'core': Capability.core,
        'router': Capability.router,
        'cli': Capability.cli,
        'dynamicMemory': Capability.dynamicMemory,
        'scripts': Capability.scripts,
        'storageFiles': Capability.storageFiles,
        'appInterface': Capability.appInterface,
        'subscriptions': Capability.subscriptions,
        'node': Capability.node,
      }, fw,
          // The firmware namespace does not define bit 1; the app names it Router from
          // Docs/Services/Router.md (the feature is not implemented).
          appOnly: const {'router'});
    });
  }, skip: skipReason);

  group('render dictionary contract (app vs firmware)', () {
    test('shape / operation / texture numbering matches Render.h', () {
      final render = _firmwareEnum('src/Blocks/Render.h', 'Geometries');
      if (render.isEmpty) return; // firmware not available; the group below is skipped
      _expectSameEnum(renderShapes, render);
      _expectSameEnum(renderOperations,
          _firmwareEnum('src/Blocks/Render.h', 'GeometryOperation'));
      _expectSameEnum(renderTextures,
          _firmwareEnum('src/Blocks/Render.h', 'Textures2D'));
    });
  }, skip: skipReason);
}
