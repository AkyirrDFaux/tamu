#pragma once

// Script service (Docs/Services/Script.md).
//
// Milestone A: SCR_XXX file parsing, the loaded-script registry, Register exposure (block
// the Scripts range 0x3F4-0x3F7) and management commands 0x0500-0x0507.
// Milestone B: preloaded instructions, line/block tables, the execution engine (the VM)
// and the six-state machine. The opcode/symbol encoding is an internal convention shared
// with the app's script_instructions.dart.
//
// The service is split into parts, all included here in dependency order so it stays a
// single translation unit (the parts use file-local `static` helpers and share the
// SCRIPT_* macro encodings):
//   ScriptDefs.h    constants, tables, the loaded-script registry
//   ScriptProgram.h SCR_XXX parsing and program compilation
//   ScriptVm.h      symbol resolution, values, assignment
//   ScriptExpr.h    infix expression evaluation (Set)
//   ScriptExec.h    per-line instruction execution
//   ScriptRuntime.h tick, boot load, management commands

#ifdef USE_SCRIPTS

#include "Core/Services/ScriptDefs.h"
#include "Core/Services/ScriptProgram.h"
#include "Core/Services/ScriptVm.h"
#include "Core/Services/ScriptExpr.h"
#include "Core/Services/ScriptExec.h"
#include "Core/Services/ScriptRuntime.h"

#endif // USE_SCRIPTS
