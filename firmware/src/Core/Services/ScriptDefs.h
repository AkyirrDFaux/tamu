#pragma once

// Constants, symbol/opcode tables and the script registry (ScriptDefs.h) - part of the script service.
//
// Shared definitions: the SCRIPT_* encodings, ScriptLineInfo/ScriptLoaded
// tables and the loaded-script registry accessors.
//
// Split out of Script.h; included by it in order so the whole service stays one
// translation unit. Wrapped in the same guard so it is a no-op without USE_SCRIPTS.

#ifdef USE_SCRIPTS

#include <cstdint>
#include "Core/Functions/Memory.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/StrideOffsets.h" // StrideAlign4 + the prefix-sum build
#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"

// Defined in Core/Services/Register.h (shared with the Subscriptions service); declared
// here so the VM resolves registers through the exact same access path.
bool RegisterGetByBlockInfo(uint32_t bi, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz);
bool RegisterSetByBlockInfo(uint32_t bi, const ValueInfo &m, const uint8_t *val, uint16_t vlen);

// Number of loaded script slots. The Scripts range is four banked block types (0x3F4-0x3F7) of
// 64 instances each, addressed by one global index 0..255 (Docs/Services/Register.md "Block
// types"). The slot index doubles as that global Register index. The *stored file* space is
// wider - see MAX_SCRIPT_FILES.
#define MAX_SCRIPTS 256
// Stored script files are named SCR_XXX (three hex digits), so a file id is 0..0xFFF.
#define MAX_SCRIPT_FILES 4096
#define SCRIPT_HEADER_SIZE 24
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
#define SCRIPT_OP_SERVICE_STATE 3
#define SCRIPT_OP_SERVICE_NOP 4
#define SCRIPT_OP_SERVICE_REG_READ_FOREIGN 5
#define SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN 6
// Script (un)loading from within a script (Docs/Services/Script.md functions list): the same
// operations as management CIDs 1 and 2, so the two paths cannot drift apart.
#define SCRIPT_OP_SERVICE_SCRIPT_LOAD 7
#define SCRIPT_OP_SERVICE_SCRIPT_UNLOAD 8

// Compose ops.
#define SCRIPT_OP_COMPOSE 0
#define SCRIPT_OP_EXTRACT 1

// Error codes (surfaced through the header's Error entry).
#define SCRIPT_ERR_NONE 0
#define SCRIPT_ERR_UNKNOWN_OP 1
#define SCRIPT_ERR_TYPE 2
#define SCRIPT_ERR_OPERAND 3
#define SCRIPT_ERR_BOUNDS 4
#define SCRIPT_ERR_STACK 5
#define SCRIPT_ERR_REGISTER 6
#define SCRIPT_ERR_TIMEOUT 7
#define SCRIPT_ERR_NOT_IMPL 8

// Per-instance script transaction IDs for foreign register access (TRID_SCRIPT_BASE..MAX,
// defined in Functions/Packet.h).

// States (Docs/Services/Script.md "State").
enum class ScriptState : uint8_t {
    Stopped  = 0,
    Running  = 1,
    Paused   = 2,
    Waiting  = 3,
    Finished = 4,
    Error    = 5
};

// Properties bits ("information about script type (load on boot etc..)").
#define SCRIPT_PROP_LOAD_ON_BOOT (1u << 0)
#define SCRIPT_PROP_RUN_ON_LOAD  (1u << 1)

// Register field categories for the Scripts range (0x3F4-0x3F7). Only the I/O categories are
// exposed through the Register (docs: "IO is in register"); Variables/Constants are
// internal (Script management) and Header is metadata.
#define SCRIPT_FIELD_INPUT    1
#define SCRIPT_FIELD_OUTPUT   2
#define SCRIPT_FIELD_COUNT    3 // register categories: Header (reserved) + Input + Output

// 4-byte stride alignment lives in Core/Functions/StrideOffsets.h (shared with the offset
// tables below).

// SCR_XXX (8-char space-padded name, the file id in three hex digits).
static inline void ScriptFileName(uint16_t id, char out[8]) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'S'; out[1] = 'C'; out[2] = 'R'; out[3] = '_';
    out[4] = hex[(id >> 8) & 0x0F];
    out[5] = hex[(id >> 4) & 0x0F];
    out[6] = hex[id & 0x0F];
    out[7] = ' ';
}

static inline uint32_t ScriptRdU32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

// Precompiled line descriptor: the instruction symbol sits at start+destCount, its
// operands follow, and the line ends with an Endline symbol.
struct ScriptLineInfo {
    uint16_t start;
    uint8_t destCount;
    uint8_t opCount;
};

// One loaded script. Value spaces are parallel to the register tables:
//   ioSpace    : inputs then outputs, 4-byte strided (volatile)
//   varSpace   : leading u32 instruction counter + variables
//   constSpace : constants (read-only)
struct LoadedScript {
    bool active = false;
    uint16_t slot = 0;
    uint16_t fileId = 0; // the stored SCR_XXX file this slot was loaded from
    uint8_t state = (uint8_t)ScriptState::Stopped;
    uint32_t properties = 0;

    uint8_t inCount = 0, outCount = 0, varCount = 0, constCount = 0;
    ValueInfo *inMeta = nullptr, *outMeta = nullptr, *varMeta = nullptr, *constMeta = nullptr;

    uint8_t *ioSpace = nullptr;
    uint8_t *varSpace = nullptr;
    uint8_t *constSpace = nullptr;
    uint16_t inTotal = 0, outTotal = 0, varTotal = 0, constTotal = 0;

    // Prefix sums of the four spaces' aligned strides, one segment per space in the order
    // inputs, outputs, variables, constants. Layout: [0..inCount] inputs, then [..outCount]
    // outputs, [..varCount] variables, [..constCount] constants, each segment ending in its
    // total. Built once by BuildOffsets() at load; see Core/Functions/StrideOffsets.h for why.
    uint16_t *offsets = nullptr;

    uint16_t uiLen = 0;
    uint32_t instrLen = 0;
    char name[BLOCK_NAME_LEN] = {0};

    // Program (preloaded instructions + compiled line/block tables).
    uint8_t *instr = nullptr;
    uint16_t instrSymbols = 0;
    ScriptLineInfo *lines = nullptr;
    uint16_t lineCount = 0;
    uint16_t *blockMatch = nullptr; // per line: matching If/While <-> EndBlock (0xFFFF none)
    uint8_t *blockKind = nullptr;   // 0 none, 1 If, 2 While, 3 EndBlock
    uint32_t *lineStamp = nullptr;  // loop-detection stamp per line

    // Runtime state.
    uint16_t ic = 0; // line index
    bool waitingOnTime = false;
    uint32_t waitUntil = 0;
    uint16_t callStack[SCRIPT_MAX_CALL_DEPTH] = {0};
    uint8_t callDepth = 0;
    uint8_t errorCode = SCRIPT_ERR_NONE;

    // Foreign register access (per-instance TRID + pending confirmation).
    uint16_t trid = 0;
    bool pendingForeign = false;
    bool pendingRead = false;
    uint16_t pendingTrid = 0;
    uint32_t pendingDeadline = 0;
    uint8_t pendingDest[4] = {0};

    void Release() {
        free(inMeta); free(outMeta); free(varMeta); free(constMeta);
        free(ioSpace); free(varSpace); free(constSpace);
        free(instr); free(lines); free(blockMatch); free(blockKind); free(lineStamp);
        free(offsets);
        inMeta = outMeta = varMeta = constMeta = nullptr;
        ioSpace = varSpace = constSpace = nullptr;
        instr = nullptr; lines = nullptr; blockMatch = nullptr; blockKind = nullptr; lineStamp = nullptr;
        offsets = nullptr;
        active = false;
        state = (uint8_t)ScriptState::Stopped;
        properties = 0;
        inCount = outCount = varCount = constCount = 0;
        inTotal = outTotal = varTotal = constTotal = 0;
        uiLen = 0; instrLen = 0; instrSymbols = 0; lineCount = 0;
        ic = 0; waitingOnTime = false; waitUntil = 0;
        callDepth = 0; errorCode = SCRIPT_ERR_NONE;
        trid = 0; pendingForeign = false; pendingRead = false; pendingTrid = 0; pendingDeadline = 0;
        memset(pendingDest, 0, sizeof(pendingDest));
        name[0] = '\0';
    }

    // Builds the offset table and the four space totals from the meta arrays. Called once by
    // ScriptLoad, after the metadata is parsed and before anything resolves a symbol. The
    // layout cannot change while the script stays loaded, so nothing needs invalidating.
    bool BuildOffsets() {
        free(offsets);
        offsets = nullptr;
        const uint16_t n = StrideTableSize(inCount, outCount, varCount, constCount);
        offsets = (uint16_t *)malloc((size_t)n * sizeof(uint16_t));
        if (!offsets) return false;
        uint16_t k = StrideOffsetsBuild(offsets, StrideInBase(), inMeta, inCount, 0);
        inTotal = offsets[inCount];
        k = StrideOffsetsBuild(offsets, k, outMeta, outCount, inTotal); // outputs follow inputs
        outTotal = (uint16_t)(offsets[k - 1] - inTotal);
        k = StrideOffsetsBuild(offsets, k, varMeta, varCount, 4); // varSpace leads with the u32 IC
        varTotal = (uint16_t)(offsets[k - 1] - 4);
        k = StrideOffsetsBuild(offsets, k, constMeta, constCount, 0);
        constTotal = offsets[k - 1];
        return true;
    }

    // Offset of entry `i` in the space starting at table index `base`. An index past the end
    // saturates on the space total rather than reading a neighbouring space's segment.
    uint16_t OffsetAt(uint16_t base, uint8_t i, uint8_t count) const {
        return offsets[base + (i < count ? i : count)];
    }
    uint16_t InputOffset(uint8_t i) const { return OffsetAt(StrideInBase(), i, inCount); }
    uint16_t OutputOffset(uint8_t i) const { return OffsetAt(StrideOutBase(inCount), i, outCount); }
    uint16_t VarOffset(uint8_t i) const { return OffsetAt(StrideVarBase(inCount, outCount), i, varCount); }
    uint16_t ConstOffset(uint8_t i) const { return OffsetAt(StrideConstBase(inCount, outCount, varCount), i, constCount); }
};

static LoadedScript scriptRegistry[MAX_SCRIPTS];
// One bit per slot, set for every slot that may hold a loaded script (a superset of the active
// slots, so hot loops can skip the empty registry entries). MAX_SCRIPTS is a multiple of 64.
static constexpr uint16_t kScriptMaskWords = MAX_SCRIPTS / 64;
static uint64_t scriptActiveMask[kScriptMaskWords];

static inline void ScriptMaskSet(uint16_t slot, bool on) {
    uint16_t w = (uint16_t)(slot >> 6), b = (uint16_t)(slot & 63);
    if (on) scriptActiveMask[w] |= (uint64_t)1 << b;
    else    scriptActiveMask[w] &= ~((uint64_t)1 << b);
}

static LoadedScript *ScriptActive(uint16_t slot) {
    if (slot >= MAX_SCRIPTS) return nullptr;
    return scriptRegistry[slot].active ? &scriptRegistry[slot] : nullptr;
}


#endif // USE_SCRIPTS
