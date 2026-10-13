/// Script functions and macros (Docs/Services/Script.md "Functions and Macros").
///
/// Stage 3 of the script system, app side. Reuse is the existing Flow `Call`/`Return` - there
/// is no new VM instruction and no run-time expansion. The app owns three kinds of function:
///
/// * **Local** - a body kept in the file's app-only UI blob and laid out by the app: the body
///   symbols are appended to the assembled instruction stream and each use site's `Call` gets
///   the body's absolute line index. The device does not lay local functions out.
/// * **Macro** - an authoring-layer alias of a local function: the app expands it into a Flow
///   `Call` plus a body. One body per script, a `Call` at each use site.
/// * **Global** - linked at load, never copied into the script file. The app writes one
///   16-byte Function reservation per referenced global function (file id + memory-space
///   offset + instruction-space offset) and leaves the `Call`'s address for load-time
///   relocation.
///
/// Assembly happens in the app, before upload; it refuses a cyclic call graph and a nesting
/// depth past [scriptMaxCallDepth] so the no-stack-frame limits are build errors rather than
/// run-time surprises (Script.md: "the app refuses, at assembly, any cyclic call graph and any
/// nesting deeper than the call-depth cap of 8").
///
/// ## Encoding (matched to the firmware)
///
/// Script.md does not pin the byte layout of a 16-byte function reservation or the operand of a
/// precompiled global `Call`. The firmware (`Core/Services/ScriptDefs.h`, the parallel Stage 3
/// change) fixes both, and the app matches it here (still isolated so a later pin is one place):
///
/// * [encodeFunctionReservation] - `FileId u16 | MemOffset u16 | InstrOffset u16 | 10 zero`
///   (the reserved bytes are zero).
/// * [scriptGlobalCallOperand] - a `Predefine/Index` operand with bit 15 set and the reservation
///   index in the low 15 bits (firmware `SCRIPT_CALL_GLOBAL_BIT`); a line target is < 1024, so
///   bit 15 is free to mark a global call.
/// * [scriptLocalCallOperand] - a plain `Predefine/Index` line target. A main-program call is the
///   combined absolute line index; a call inside a body is the target's offset from that body's
///   first line (the firmware adds the body's base at load).
library;

import 'script_file.dart';
import 'script_instructions.dart';

/// The VM's call-depth cap (firmware `SCRIPT_MAX_CALL_DEPTH`, Script.md "Functions and
/// Macros"): a call graph that nests deeper than this is refused at assembly.
const int scriptMaxCallDepth = 8;

/// A 16-byte function-reservation record for a referenced global function. Re-exported from the
/// format layer ([encodeFunctionReservation]) so callers of assembly find it here too.
List<int> functionReservationFor(ScriptFunction f) => encodeFunctionReservation(
  fileId: f.globalFileId,
  memOffset: f.globalMemOffset,
  instrOffset: f.globalInstrOffset,
);

/// The `Call` operand for a local/macro body: a plain `Predefine/Index` line target, which the
/// app resolves at assembly (main-program calls carry the combined absolute line index; a call
/// inside a body carries the target's offset from that body's first line, which the firmware
/// adds the body's base to at load). A local line target is always < 1024, so bit 15 is free to
/// mark a global call. This one is well defined - the existing Jump/Call line-target operand.
ScriptSymbol scriptLocalCallOperand(int line) =>
    ScriptSymbol.predefine(preIndex, line);

/// Bit 15 of a `Call` operand marks a linked global function (firmware `SCRIPT_CALL_GLOBAL_BIT`);
/// the low 15 bits hold the reservation index. The line target of a local body never uses it.
const int scriptCallGlobalBit = 0x8000;

/// True when a `Call` operand names a linked global function.
bool scriptIsGlobalCall(int operand) => (operand & scriptCallGlobalBit) != 0;

/// The reservation index inside a global-call operand.
int scriptGlobalCallIndex(int operand) => operand & ~scriptCallGlobalBit;

/// The `Call` operand value for reservation [index] (bit 15 set, index in the low 15 bits).
int scriptGlobalCallOperandValue(int index) =>
    scriptCallGlobalBit | (index & ~scriptCallGlobalBit);

/// The `Call` operand for a referenced global function: a `Predefine/Index` whose value is the
/// function's reservation index with bit 15 set, so the device resolves the real address at load
/// (the app knows the interface at assembly and leaves only the address to be filled in).
ScriptSymbol scriptGlobalCallOperand(int reservationIndex) =>
    ScriptSymbol.predefine(
      preIndex,
      scriptGlobalCallOperandValue(reservationIndex),
    );

/// The result of assembling a script's main program together with its functions.
class ScriptAssembly {
  /// Assembly problems (empty when the program assembles cleanly).
  final List<String> errors;

  /// The assembled instruction stream as lines: the main program, then each used body laid
  /// out contiguously. Every function `Call` has its operand rewritten to the body's absolute
  /// line index (local/macro) or a reservation reference (global).
  final List<ScriptLine> lines;

  /// The number of lines the main program occupies at the head of [lines] (needed to split the
  /// program back into main + bodies when the file is read).
  final int mainLineCount;

  /// Function reservations, one 16-byte record per referenced global function.
  final List<List<int>> reservations;

  /// Reserved global-function indexes in reservation order (parallel to [reservations]).
  final List<int> reservationFunctions;

  const ScriptAssembly({
    this.errors = const [],
    this.lines = const [],
    this.mainLineCount = 0,
    this.reservations = const [],
    this.reservationFunctions = const [],
  });

  bool get ok => errors.isEmpty;

  /// True when any function at all is declared (drives whether the UI blob carries a function
  /// block).
  bool get hasFunctions => lines.length != mainLineCount;
}

/// Assembles [mainLines] with [functions] into a flat program.
///
/// Refuses, as [ScriptAssembly.errors] (no lines are produced):
/// * a `Call` whose target index is out of range ("unknown function");
/// * any cycle in the call graph (direct or indirect, including self-recursion);
/// * any nesting deeper than [callDepthCap] (the VM's call-depth cap).
ScriptAssembly assembleScriptProgram({
  required List<ScriptLine> mainLines,
  List<ScriptFunction> functions = const [],
  int callDepthCap = scriptMaxCallDepth,
}) {
  final errors = <String>[];

  // 1. Collect call edges over the function graph. Node -1 is the main program (a root only).
  final edges = <int, List<int>>{}; // function index -> called function indexes
  for (var i = 0; i < functions.length; i++) {
    edges[i] = <int>[];
  }
  final mainTargets = <int>[];
  void scan(List<ScriptLine> lines, int? from) {
    for (final line in lines) {
      final target = line.functionTarget;
      if (target == null) continue;
      if (target < 0 || target >= functions.length) {
        errors.add('Call targets unknown function #$target');
        continue;
      }
      if (from == null) {
        mainTargets.add(target);
      } else {
        edges[from]!.add(target);
      }
    }
  }

  scan(mainLines, null);
  for (var i = 0; i < functions.length; i++) {
    final f = functions[i];
    if (f.hasBody) scan(f.body, i);
  }

  // 2. Cycle detection (a colouring DFS over the whole declared graph).
  final color = List<int>.filled(
    functions.length,
    0,
  ); // 0 white, 1 grey, 2 black
  var hasCycle = false;
  void visit(int node) {
    if (hasCycle || color[node] == 2) return;
    if (color[node] == 1) {
      hasCycle = true;
      return;
    }
    color[node] = 1;
    for (final next in edges[node]!) {
      visit(next);
    }
    color[node] = 2;
  }

  for (var i = 0; i < functions.length; i++) {
    visit(i);
  }
  if (hasCycle) {
    errors.add('Cyclic call graph (recursion is not allowed)');
  }

  // 3. Nesting depth: the longest chain of calls. Main's Call is the first frame, so a chain
  // of E edges sits E+1 frames deep; E+1 must not exceed the cap.
  final depthMemo = <int, int>{};
  int longest(int node) {
    if (depthMemo.containsKey(node)) return depthMemo[node]!;
    depthMemo[node] = 0; // guards against a cycle that slipped through
    var best = 0;
    for (final next in edges[node]!) {
      final d = 1 + longest(next);
      if (d > best) best = d;
    }
    depthMemo[node] = best;
    return best;
  }

  var maxFrames = 0;
  for (final t in mainTargets) {
    final frames = 1 + longest(t);
    if (frames > maxFrames) maxFrames = frames;
  }
  if (maxFrames > callDepthCap) {
    errors.add(
      'Call nesting $maxFrames exceeds the call-depth cap of $callDepthCap',
    );
  }

  if (errors.isNotEmpty) return ScriptAssembly(errors: errors);

  // 4. Reachability from main, so only used bodies are laid out (one body per script).
  final used = List<bool>.filled(functions.length, false);
  void mark(int node) {
    if (used[node]) return;
    used[node] = true;
    for (final next in edges[node]!) {
      mark(next);
    }
  }

  for (final t in mainTargets) {
    mark(t);
  }

  // 5. Layout: main occupies [0, mainCount); used bodies follow in declaration order.
  final mainCount = mainLines.length;
  final starts = List<int>.filled(functions.length, -1);
  var cursor = mainCount;
  for (var i = 0; i < functions.length; i++) {
    if (!used[i] || !functions[i].hasBody) continue;
    starts[i] = cursor;
    cursor += functions[i].body.length;
  }

  // 6. Reservations for referenced global functions (one record each, in declaration order).
  final reservations = <List<int>>[];
  final reservationFunctions = <int>[];
  final reservationByFunction = <int, int>{};
  for (var i = 0; i < functions.length; i++) {
    if (!used[i] || !functions[i].isGlobal) continue;
    reservationByFunction[i] = reservations.length;
    reservationFunctions.add(i);
    final f = functions[i];
    reservations.add(
      encodeFunctionReservation(
        fileId: f.globalFileId,
        memOffset: f.globalMemOffset,
        instrOffset: f.globalInstrOffset,
      ),
    );
  }

  // 7. Rewrite each Call operand to the resolved address and splice the bodies in. A main-program
  // call carries the combined absolute line index; a call inside a body carries the target's
  // offset from that body's first line (the firmware adds the body's base at load).
  ScriptLine rewrite(ScriptLine line, int? enclosing) {
    final out = line.clone();
    final target = line.functionTarget;
    if (target == null || !line.isCall) return out;
    final f = functions[target];
    final ScriptSymbol operand;
    if (f.isGlobal) {
      operand = scriptGlobalCallOperand(reservationByFunction[target]!);
    } else {
      final line = enclosing == null
          ? starts[target]
          : starts[target] - starts[enclosing];
      operand = scriptLocalCallOperand(line);
    }
    if (out.operands.isEmpty) {
      out.operands.add(operand);
    } else {
      out.operands[0] = operand;
    }
    return out;
  }

  final lines = <ScriptLine>[
    for (final l in mainLines) rewrite(l, null),
    for (var i = 0; i < functions.length; i++)
      if (starts[i] >= 0) ...functions[i].body.map((l) => rewrite(l, i)),
  ];

  return ScriptAssembly(
    mainLineCount: mainCount,
    lines: lines,
    reservations: reservations,
    reservationFunctions: reservationFunctions,
  );
}

/// Assembly problems for a program and its functions (empty when it assembles cleanly). A
/// convenience for the editor's pre-upload validation.
List<String> validateScriptFunctions({
  required List<ScriptLine> mainLines,
  List<ScriptFunction> functions = const [],
}) => assembleScriptProgram(mainLines: mainLines, functions: functions).errors;
