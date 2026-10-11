#pragma once

// Constants, symbol/opcode tables and the script structures (ScriptDefs.h) - part of the script service.
//
// Shared definitions: the SCRIPT_* encodings and the ScriptLineInfo/LoadedScript
// tables. Included by Script.h BEFORE the ScriptService class so the class can store the
// loaded-script registry by value; the service's own functions live as members now.
//
// Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include <cstdint>
#include <cstdlib>
#include <cstring>
// ValueInfo and BLOCK_NAME_LEN come from Core/Functions/MemoryTypes.h (via StrideOffsets.h);
// the Memory/Storage subsystems are only needed by the real firmware, so the host-side test
// (test/native/script_test.cpp) can include this header with SCRIPT_HOST_TEST to exercise the
// file-header parse and the opcode/property constants without dragging in the device layer.
#include "Core/Functions/StrideOffsets.h" // MemoryTypes (ValueInfo) + StrideAlign4 + the prefix-sum build
#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"
#ifndef SCRIPT_HOST_TEST
#include "Core/Functions/Memory.h"
#include "Core/Functions/Storage.h"
#endif

// Defined in Core/Services/Register.h (shared with the Subscriptions service); declared
// here so the VM resolves registers through the exact same access path.
bool RegisterGetByBlockInfo(uint32_t Bi, ValueInfo &Meta, uint8_t *Vbuf, uint8_t &Vsz);
bool RegisterSetByBlockInfo(uint32_t Bi, const ValueInfo &Meta, const uint8_t *Val, uint16_t Vlen);

// Number of loaded script slots. The Scripts range is four banked block types (0x3F4-0x3F7) of
// 64 instances each, addressed by one global index 0..255 (Docs/Services/Register.md "Block
// types"). The slot index doubles as that global Register index. The *stored file* space is
// wider - see MAX_SCRIPT_FILES.
//
// RAM-sizing knob (like MAX_PROVIDER_SUBS): the registry is one LoadedScript per slot (~150 B),
// so 256 slots cost ~38 KB. The ESP32 core can afford that; a RAM-constrained target (the
// CH32 Valu, 20 KB) lowers it via -D MAX_SCRIPTS=N and simply cannot load the higher slots.
#ifndef MAX_SCRIPTS
#define MAX_SCRIPTS 256
#endif
// Stored script files are named SCR_XXX (three hex digits), so a file id is 0..0xFFF.
#define MAX_SCRIPT_FILES 4096
// The file header is 24 bytes (Docs/Services/Script.md "File Blocks"): a 16-bit Properties word,
// five uint8 counts (Input, Output, Variable, Constant, Function) and one padding byte, then
// four uint32 lengths (constants, input-defaults, instruction, UI info).
#define SCRIPT_HEADER_SIZE 24
// One function reservation record: the file a linked global function lives in, its memory-space
// offset and its instruction-space offset.
#define SCRIPT_FUNC_RESERVATION_SIZE 16
#define SCRIPT_MAX_LINES 1024
#define SCRIPT_INSTR_BUDGET 512
#define SCRIPT_MAX_CALL_DEPTH 8

// Symbol types (Docs symbol table).
#define SCRIPT_SYM_INSTRUCTION 0
#define SCRIPT_SYM_INPUT 1
#define SCRIPT_SYM_OUTPUT 2
#define SCRIPT_SYM_VARIABLE 3
#define SCRIPT_SYM_CONSTANT 4
#define SCRIPT_SYM_ENDLINE 5
#define SCRIPT_SYM_PREDEFINE 6

// Instruction categories (the instruction symbol's subtype).
#define SCRIPT_CAT_MATH 0
#define SCRIPT_CAT_LOGIC 1
#define SCRIPT_CAT_FLOW 2
#define SCRIPT_CAT_TIME 3
#define SCRIPT_CAT_SERVICE 4
#define SCRIPT_CAT_COMPOSE 5

// Predefine subtypes.
#define SCRIPT_PRE_STATE 0
#define SCRIPT_PRE_TYPE 1
#define SCRIPT_PRE_INDEX 2
#define SCRIPT_PRE_CHAR 3
#define SCRIPT_PRE_MATHOP 4
#define SCRIPT_PRE_BOOL 5
#define SCRIPT_PRE_NUMBER 6 // 16-bit Q8.8 fixed-point literal

// Math ops. Add/Sub/Mul/Div/Neg (1..4, 8) are retired as instructions: arithmetic is
// handled by the Set expression (`SCRIPT_MATHOP_*`); the values are left reserved so the
// wire numbering stays stable.
#define SCRIPT_OP_MATH_SET 0
#define SCRIPT_OP_MATH_MOD 5
#define SCRIPT_OP_MATH_MIN 6
#define SCRIPT_OP_MATH_MAX 7
#define SCRIPT_OP_MATH_ABS 9
#define SCRIPT_OP_MATH_LIMIT 10 // clamp(value, min, max)
#define SCRIPT_OP_MATH_TRANSFORM 11 // 2x3 matrix from (rot, ox, oy, sx, sy[, skew])

// Inline expression operator values (the `Math op` predefine): the operands of a Set
// line may be interleaved with these to form an infix expression (Docs/Services/Script.md:
// "generic math and logic processor").
#define SCRIPT_MATHOP_ADD 0
#define SCRIPT_MATHOP_SUB 1
#define SCRIPT_MATHOP_MUL 2
#define SCRIPT_MATHOP_DIV 3
#define SCRIPT_MATHOP_MOD 4
#define SCRIPT_MATHOP_POW 5
#define SCRIPT_MATHOP_AND 6
#define SCRIPT_MATHOP_OR 7
#define SCRIPT_MATHOP_XOR 8
#define SCRIPT_MATHOP_NOT 9
// 10-11 were the shift operators; dropped and left reserved so the numbers are never reused.
#define SCRIPT_MATHOP_EQ 12
#define SCRIPT_MATHOP_NE 13
#define SCRIPT_MATHOP_LT 14
#define SCRIPT_MATHOP_LE 15
#define SCRIPT_MATHOP_GT 16
#define SCRIPT_MATHOP_GE 17
#define SCRIPT_MATHOP_OPEN 18
#define SCRIPT_MATHOP_CLOSE 19
// Prefix functions (arithmetic / vector / matrix): size v, transpose m, dot a b, cross a b.
#define SCRIPT_MATHOP_FN_DOT 20
#define SCRIPT_MATHOP_FN_CROSS 21
#define SCRIPT_MATHOP_FN_SIZE 22
#define SCRIPT_MATHOP_FN_TRANSPOSE 23

// Logic op (comparisons/logic moved into the Set expression; only Select remains as an
// instruction).
#define SCRIPT_OP_LOGIC_SELECT 12

// Flow ops.
#define SCRIPT_OP_FLOW_IF 0
#define SCRIPT_OP_FLOW_WHILE 1
#define SCRIPT_OP_FLOW_END 2
#define SCRIPT_OP_FLOW_JUMP 3
#define SCRIPT_OP_FLOW_CALL 4
#define SCRIPT_OP_FLOW_RETURN 5
#define SCRIPT_OP_FLOW_HALT 6

// Time ops.
#define SCRIPT_OP_TIME_DELAY 0
#define SCRIPT_OP_TIME_WAIT 1
#define SCRIPT_OP_TIME_GET 2

// Service ops.
#define SCRIPT_OP_SERVICE_LOG 0
#define SCRIPT_OP_SERVICE_REG_READ 1
#define SCRIPT_OP_SERVICE_REG_WRITE 2
// Service op 3 was the single state instruction; retired - the number is reserved and never
// reused (Docs/Services/Script.md Service table). The state verbs are separate instructions.
#define SCRIPT_OP_SERVICE_RESERVED 3
#define SCRIPT_OP_SERVICE_NOP 4
#define SCRIPT_OP_SERVICE_REG_READ_FOREIGN 5
#define SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN 6
// Script (un)loading from within a script (Docs/Services/Script.md functions list): the same
// operations as management CIDs 1 and 2, so the two paths cannot drift apart. Both return their
// result code into a destination (0 = success) instead of advancing silently.
#define SCRIPT_OP_SERVICE_SCRIPT_LOAD 7
#define SCRIPT_OP_SERVICE_SCRIPT_UNLOAD 8
// State verbs: actions on the states, not values of one state instruction. Start enters Running
// and resets the instruction counter and the call depth, so a restart resolves to Start.
#define SCRIPT_OP_SERVICE_START 9
#define SCRIPT_OP_SERVICE_STOP 10
#define SCRIPT_OP_SERVICE_PAUSE 11
#define SCRIPT_OP_SERVICE_RESUME 12
// A custom error throw: logs the script-defined code and halts (state Error).
#define SCRIPT_OP_SERVICE_ERROR 13
// A log-only action: logs the script's state and carries on.
#define SCRIPT_OP_SERVICE_REPORT_INFO 14

// Compose ops.
#define SCRIPT_OP_COMPOSE 0
#define SCRIPT_OP_EXTRACT 1

// Error codes. The 32-bit report `error` field is instruction-specific (Docs/Services/Script.md
// "Error Model"): a code's meaning is defined by the instruction that faulted, and the
// instruction field in the same record makes it interpretable. These generic values are what
// the engine carries today; the per-instruction catalogue is still undecided, so nothing here
// is invented per instruction.
#define SCRIPT_ERR_NONE 0
#define SCRIPT_ERR_UNKNOWN_OP 1
#define SCRIPT_ERR_TYPE 2
#define SCRIPT_ERR_OPERAND 3
#define SCRIPT_ERR_BOUNDS 4
#define SCRIPT_ERR_STACK 5
#define SCRIPT_ERR_REGISTER 6
#define SCRIPT_ERR_TIMEOUT 7
#define SCRIPT_ERR_NOT_IMPL 8

// The report record's `instruction` field: the offending instruction's own encoding, i.e.
// `1 + ((category << 8) | opcode)`, where category/opcode are the Instruction symbol's
// Subtype/Value. The `+1` makes a valid instruction never 0, so 0 can mean "no instruction
// behind the fault" (engine-level, Docs/Services/Script.md "Error Model"). This is the one
// underdetermined point of the spec: it is isolated here so a different encoding is a one-line
// change at this single helper.
inline uint32_t ScriptInstructionEncoding(uint8_t Category, uint8_t Opcode) {
    return 1u + (((uint32_t)Category << 8) | (uint32_t)Opcode);
}

// CID 3 report record layout (Docs/Services/Script.md "Report Record"): state (u8) | depth (u8) |
// line (u16) = 4 bytes, with instruction (u32) | error (u32) appended only on an error = 12.
// The depth byte pads the record so `line` sits on a 2-byte boundary and the error's two 32-bit
// fields land on 4-byte boundaries.
#define SCRIPT_REPORT_OFF_STATE 0
#define SCRIPT_REPORT_OFF_DEPTH 1
#define SCRIPT_REPORT_OFF_LINE 2
#define SCRIPT_REPORT_OFF_INSTRUCTION 4
#define SCRIPT_REPORT_OFF_ERROR 8
#define SCRIPT_REPORT_SIZE 4      // no error: state | depth | line
#define SCRIPT_REPORT_SIZE_ERR 12 // on an error: + instruction | error

// Per-instance script transaction IDs for foreign register access (TRID_SCRIPT_BASE..MAX,
// defined in Functions/Packet.h).

// States (Docs/Services/Script.md "State").
enum class ScriptState : uint8_t {
    Stopped  = 0,
    Running  = 1,
    Paused   = 2,
    Waiting  = 3,
    Finished = 4,
    Error    = 5,
    // Load-related states, appended so the existing values keep their numbers. In
    // `reserved-but-not-ready` the slot's space exists but the script cannot run yet; it leaves
    // by becoming ready (Stopped) or by being released. In `load-failed` the load was refused
    // and the slot keeps its reservation until an explicit unload clears it.
    ReservedNotReady = 6,
    LoadFailed       = 7
};

// Maps a CID 4 management action to the script state it sets (Docs/Services/Script.md CID 4).
// The action enum is the state verbs only - Start 9, Stop 10, Pause 11, Resume 12. The in-script
// Error op (13) is a throw, not a management action, so it and any other value are rejected
// (returns false). Host-testable in the native suite.
inline bool ScriptActionToState(uint8_t Action, uint8_t &State) {
    switch (Action) {
        case SCRIPT_OP_SERVICE_START:  State = (uint8_t)ScriptState::Running; return true;
        case SCRIPT_OP_SERVICE_STOP:   State = (uint8_t)ScriptState::Stopped; return true;
        case SCRIPT_OP_SERVICE_PAUSE:  State = (uint8_t)ScriptState::Paused;  return true;
        case SCRIPT_OP_SERVICE_RESUME: State = (uint8_t)ScriptState::Running; return true;
        default: return false;
    }
}

// Properties bits (Docs/Services/Script.md "Properties"). The word is 16 bits: a flags field
// and, at bits 8-15, the uint8 slot a load-on-boot script loads into. A boot load is the flag
// bit plus that slot - deterministic, not the identity slot or the first free one.
#define SCRIPT_PROP_LOAD_ON_BOOT (1u << 0)
#define SCRIPT_PROP_RUN_ON_LOAD  (1u << 1)
#define SCRIPT_PROP_FUNCTION_LIB (1u << 2) // the file is a function library, not a script
#define SCRIPT_PROP_SLOT_SHIFT 8
#define SCRIPT_PROP_SLOT_MASK  (0xFFu << SCRIPT_PROP_SLOT_SHIFT)

inline bool ScriptPropLoadOnBoot(uint16_t Props)  { return (Props & SCRIPT_PROP_LOAD_ON_BOOT) != 0; }
inline bool ScriptPropRunOnLoad(uint16_t Props)   { return (Props & SCRIPT_PROP_RUN_ON_LOAD) != 0; }
inline bool ScriptPropFunctionLib(uint16_t Props) { return (Props & SCRIPT_PROP_FUNCTION_LIB) != 0; }
inline uint8_t ScriptPropSlot(uint16_t Props)     { return (uint8_t)((Props & SCRIPT_PROP_SLOT_MASK) >> SCRIPT_PROP_SLOT_SHIFT); }

// The parsed 24-byte file header. The offsets live here (host-testable) so the parser and the
// block walk in ScriptProgram.h cannot drift.
struct ScriptHeader {
    uint16_t Properties;
    uint8_t InCount;
    uint8_t OutCount;
    uint8_t VarCount;
    uint8_t ConstCount;
    uint8_t FuncCount;
    uint32_t ConstLen;
    uint32_t DefLen;   // input defaults length
    uint32_t InstrLen;
    uint32_t UiLen;
};

inline uint16_t ScriptRd16(const uint8_t *P) { return (uint16_t)(P[0] | (P[1] << 8)); }
inline uint32_t ScriptRd32(const uint8_t *P) {
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

// Reads the header from the start of a script file. Returns false for a buffer shorter than the
// header. Buf[7] is the padding byte that aligns the counts to 4 bytes.
inline bool ParseScriptHeader(const uint8_t *Buf, uint32_t Size, ScriptHeader &H) {
    if (Size < SCRIPT_HEADER_SIZE) return false;
    H.Properties = ScriptRd16(Buf + 0);
    H.InCount    = Buf[2];
    H.OutCount   = Buf[3];
    H.VarCount   = Buf[4];
    H.ConstCount = Buf[5];
    H.FuncCount  = Buf[6];
    H.ConstLen   = ScriptRd32(Buf + 8);
    H.DefLen     = ScriptRd32(Buf + 12);
    H.InstrLen   = ScriptRd32(Buf + 16);
    H.UiLen      = ScriptRd32(Buf + 20);
    return true;
}

// Register field categories for the Scripts range (0x3F4-0x3F7). Only the I/O categories are
// exposed through the Register (docs: "IO is in register"); Variables/Constants are
// internal (Script management) and Header is metadata.
#define SCRIPT_FIELD_INPUT    1
#define SCRIPT_FIELD_OUTPUT   2
#define SCRIPT_FIELD_COUNT    3 // register categories: Header (reserved) + Input + Output

// ===== Global-function linking (Stage 3, Docs/Services/Script.md "Functions and Macros") =====
//
// A script that links global functions declares them as 16-byte reservation records - one per
// function, in the block between the input defaults and the instructions. The spec fixes the
// record SIZE (16) and its CONTENT ("the file a linked global function lives in, its
// memory-space offset and its instruction-space offset", Script.md "File Blocks") but NOT its
// byte layout. That layout is the spec's weakest point; it is OUR documented choice below and is
// isolated here so pinning it later is a one-place change:
//
//   offset  size  field         meaning
//   0       2     FileId        the function library SCR_XXX the function lives in
//   2       2     MemOffset     byte offset of the body's memory space within the library file's
//                               memory space (0 for a single-body library)
//   4       2     InstrOffset   symbol offset (4-byte symbols) of the body's first instruction
//                               within the library file's instruction space (0 for a single body)
//   6       10    reserved      zero
//
// A function library is the script file format with the Function-library Properties flag (bit 2).
struct ScriptFuncReservation {
    uint16_t FileId;
    uint16_t MemOffset;
    uint16_t InstrOffset;
};

inline ScriptFuncReservation ParseFuncReservation(const uint8_t *P) {
    ScriptFuncReservation R;
    R.FileId      = ScriptRd16(P + 0);
    R.MemOffset   = ScriptRd16(P + 2);
    R.InstrOffset = ScriptRd16(P + 4);
    return R;
}

// A Flow Call to a LINKED global function carries the reservation INDEX in its operand, not a
// line number (Script.md: "Calls are precompiled: ... only the address is filled in at load").
// A line target is always < SCRIPT_MAX_LINES (1024), so bit 15 is free to mark a global call and
// the low 15 bits hold the reservation index. This operand encoding is the spec's OTHER weak
// point; isolated in these three helpers so pinning it is a one-place change. (The app must emit
// ScriptGlobalCallOperand(index) for a global call and a plain line for a local one.)
#define SCRIPT_CALL_GLOBAL_BIT 0x8000u
inline bool     ScriptIsGlobalCall(uint16_t Operand)      { return (Operand & SCRIPT_CALL_GLOBAL_BIT) != 0; }
inline uint16_t ScriptGlobalCallIndex(uint16_t Operand)   { return (uint16_t)(Operand & ~SCRIPT_CALL_GLOBAL_BIT); }
inline uint16_t ScriptGlobalCallOperand(uint16_t Index)   { return (uint16_t)(SCRIPT_CALL_GLOBAL_BIT | (Index & ~SCRIPT_CALL_GLOBAL_BIT)); }

// Relocation arithmetic: the line (or symbol) a body-local target moves to once the body is
// placed at `Base` in the combined program. Straight addition, but kept as a named helper so the
// placement math lives in one place (host-tested).
inline uint16_t ScriptRelocate(uint16_t Base, uint16_t Offset) { return (uint16_t)(Base + Offset); }

// Lowest slot index whose bit is clear in a 64-slot occupancy word (0..63), or 64 when every
// slot in the word is occupied. The host-testable core of the lowest-free-slot search; FreeSlot()
// applies it word by word over the registry's occupancy (active OR load-failed).
inline uint16_t ScriptLowestFreeBit(uint64_t Occupied) {
    uint64_t Free = ~Occupied;
    if (Free == 0) return 64;
    return (uint16_t)__builtin_ctzll(Free);
}

// 4-byte stride alignment lives in Core/Functions/StrideOffsets.h (shared with the offset
// tables below).

// Precompiled line descriptor: the instruction symbol sits at start+destCount, its
// operands follow, and the line ends with an Endline symbol.
struct ScriptLineInfo {
    uint16_t Start;
    uint8_t DestCount;
    uint8_t OpCount;
};

// One loaded script. Value spaces are parallel to the register tables:
//   ioSpace    : inputs then outputs, 4-byte strided (volatile)
//   varSpace   : variables
//   constSpace : constants (read-only)
struct LoadedScript {
    bool Active = false;
    uint16_t Slot = 0;
    uint16_t FileId = 0; // the stored SCR_XXX file this slot was loaded from
    uint8_t State = (uint8_t)ScriptState::Stopped;
    // A refused load's slot keeps its reservation (Slot/FileId below): its State is
    // ScriptState::LoadFailed, and the explicit unload clears it (Docs/Services/Script.md
    // "State": the load-failed state "keeps its reservation until an explicit unload clears
    // it"). A link failure is detected before anything is allocated, so a load-failed slot
    // holds only its slot record and no spaces.
    uint32_t Properties = 0;

    uint8_t InCount = 0, OutCount = 0, VarCount = 0, ConstCount = 0;
    uint8_t FuncCount = 0; // declared function reservations (function count)
    ValueInfo *InMeta = nullptr, *OutMeta = nullptr, *VarMeta = nullptr, *ConstMeta = nullptr;

    uint8_t *IoSpace = nullptr;
    uint8_t *VarSpace = nullptr;
    uint8_t *ConstSpace = nullptr;
    uint16_t InTotal = 0, OutTotal = 0, VarTotal = 0, ConstTotal = 0;

    // Prefix sums of the four spaces' aligned strides, one segment per space in the order
    // inputs, outputs, variables, constants. Layout: [0..inCount] inputs, then [..outCount]
    // outputs, [..varCount] variables, [..constCount] constants, each segment ending in its
    // total. Built once by BuildOffsets() at load; see Core/Functions/StrideOffsets.h for why.
    uint16_t *Offsets = nullptr;

    uint16_t UiLen = 0;
    uint32_t InstrLen = 0;
    char Name[BLOCK_NAME_LEN] = {0};

    // Program (preloaded instructions + compiled line/block tables).
    uint8_t *Instr = nullptr;
    uint16_t InstrSymbols = 0;
    ScriptLineInfo *Lines = nullptr;
    uint16_t LineCount = 0;
    uint16_t *BlockMatch = nullptr; // per line: matching If/While <-> EndBlock (0xFFFF none)
    uint8_t *BlockKind = nullptr;   // 0 none, 1 If, 2 While, 3 EndBlock
    uint32_t *LineStamp = nullptr;  // loop-detection stamp per line

    // Runtime state.
    uint16_t Ic = 0; // line index
    bool WaitingOnTime = false;
    uint32_t WaitUntil = 0;
    uint16_t CallStack[SCRIPT_MAX_CALL_DEPTH] = {0};
    uint8_t CallDepth = 0;
    uint32_t ErrorCode = SCRIPT_ERR_NONE;

    // CID 3 report bookkeeping (Docs/Services/Script.md "Report Record"). Captured at a fault
    // by SetFault: the call depth and the deepest frame's line at that moment, plus the
    // offending instruction's own encoding (0 = engine-level: no instruction behind the fault).
    // A non-error report reads CallDepth/Ic live instead. SetState clears all three with the
    // error, since setting the state clears the error.
    uint8_t ReportDepth = 0;
    uint16_t ReportLine = 0;
    uint32_t FaultInstruction = 0;

    // Foreign register access (per-instance TRID + pending confirmation).
    uint16_t Trid = 0;
    bool PendingForeign = false;
    bool PendingRead = false;
    uint16_t PendingTrid = 0;
    uint32_t PendingDeadline = 0;
    uint8_t PendingDest[4] = {0};

    void Release() {
        free(InMeta); free(OutMeta); free(VarMeta); free(ConstMeta);
        free(IoSpace); free(VarSpace); free(ConstSpace);
        free(Instr); free(Lines); free(BlockMatch); free(BlockKind); free(LineStamp);
        free(Offsets);
        InMeta = OutMeta = VarMeta = ConstMeta = nullptr;
        IoSpace = VarSpace = ConstSpace = nullptr;
        Instr = nullptr; Lines = nullptr; BlockMatch = nullptr; BlockKind = nullptr; LineStamp = nullptr;
        Offsets = nullptr;
        Active = false;
        State = (uint8_t)ScriptState::Stopped;
        Properties = 0;
        InCount = OutCount = VarCount = ConstCount = 0;
        FuncCount = 0;
        InTotal = OutTotal = VarTotal = ConstTotal = 0;
        UiLen = 0; InstrLen = 0; InstrSymbols = 0; LineCount = 0;
        Ic = 0; WaitingOnTime = false; WaitUntil = 0;
        CallDepth = 0; ErrorCode = SCRIPT_ERR_NONE;
        ReportDepth = 0; ReportLine = 0; FaultInstruction = 0;
        Trid = 0; PendingForeign = false; PendingRead = false; PendingTrid = 0; PendingDeadline = 0;
        memset(PendingDest, 0, sizeof(PendingDest));
        Name[0] = '\0';
    }

    // Builds the offset table and the four space totals from the meta arrays. Called once by
    // ScriptLoad, after the metadata is parsed and before anything resolves a symbol. The
    // layout cannot change while the script stays loaded, so nothing needs invalidating.
    bool BuildOffsets() {
        free(Offsets);
        Offsets = nullptr;
        const uint16_t Size = StrideTableSize(InCount, OutCount, VarCount, ConstCount);
        Offsets = (uint16_t *)malloc((size_t)Size * sizeof(uint16_t));
        if (!Offsets) return false;
        uint16_t k = StrideOffsetsBuild(Offsets, StrideInBase(), InMeta, InCount, 0);
        InTotal = Offsets[InCount];
        k = StrideOffsetsBuild(Offsets, k, OutMeta, OutCount, InTotal); // outputs follow inputs
        OutTotal = (uint16_t)(Offsets[k - 1] - InTotal);
        k = StrideOffsetsBuild(Offsets, k, VarMeta, VarCount, 0);
        VarTotal = Offsets[k - 1];
        k = StrideOffsetsBuild(Offsets, k, ConstMeta, ConstCount, 0);
        ConstTotal = Offsets[k - 1];
        return true;
    }

    // Offset of entry `i` in the space starting at table index `base`. An index past the end
    // saturates on the space total rather than reading a neighbouring space's segment.
    uint16_t OffsetAt(uint16_t Base, uint8_t i, uint8_t Count) const {
        return Offsets[Base + (i < Count ? i : Count)];
    }
    uint16_t InputOffset(uint8_t i) const { return OffsetAt(StrideInBase(), i, InCount); }
    uint16_t OutputOffset(uint8_t i) const { return OffsetAt(StrideOutBase(InCount), i, OutCount); }
    uint16_t VarOffset(uint8_t i) const { return OffsetAt(StrideVarBase(InCount, OutCount), i, VarCount); }
    uint16_t ConstOffset(uint8_t i) const { return OffsetAt(StrideConstBase(InCount, OutCount, VarCount), i, ConstCount); }
};

// True when a slot holds any reservation: an active script, or a load-failed one that keeps its
// slot record until an explicit unload (Docs/Services/Script.md "State").
inline bool ScriptSlotReserved(const LoadedScript &S) {
    return S.Active || S.State == (uint8_t)ScriptState::LoadFailed;
}

// One bit per slot, set for every slot that may hold a loaded script (a superset of the active
// slots, so hot loops can skip the empty registry entries). Rounded UP, so a MAX_SCRIPTS that is
// not a multiple of 64 (the Valu's 16) still gets one word instead of a zero-length mask.
static constexpr uint16_t KScriptMaskWords = (MAX_SCRIPTS + 63) / 64;

#endif // USE_SCRIPTS
