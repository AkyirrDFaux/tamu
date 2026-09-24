#pragma once

// Script service (Docs/Services/Script.md).
//
// Milestone A: SCR_XX file parsing, the loaded-script registry, Register exposure (block
// type 0x3FE) and management commands 0x0500-0x0507.
// Milestone B: preloaded instructions, line/block tables, the execution engine (the VM)
// and the six-state machine. The opcode/symbol encoding is an internal convention shared
// with the app's script_instructions.dart.

#ifdef USE_SCRIPTS

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "Core/Functions/Memory.h"
#include "Core/Functions/Storage.h"
#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"

// Defined in Core/Services/Register.h (shared with the Subscriptions service); declared
// here so the VM resolves registers through the exact same access path.
bool RegisterGetByBlockInfo(uint32_t bi, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz);
bool RegisterSetByBlockInfo(uint32_t bi, const BlockMeta &m, const uint8_t *val, uint16_t vlen);

// Number of script file slots (SCR_00..SCR_3F); the slot index doubles as the Script File
// ID and as the Register instance number of block type 0x3FE.
#define MAX_SCRIPTS 64
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

// Per-instance script transaction IDs for foreign register access.
#define SCRIPT_TRID_BASE 0xF000
#define SCRIPT_TRID_MAX  0xF9FF

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

// Register field categories for block type 0x3FE (Scripts). Only the I/O categories are
// exposed through the Register (docs: "IO is in register"); Variables/Constants are
// internal (Script management) and Header is metadata.
#define SCRIPT_FIELD_INPUT    1
#define SCRIPT_FIELD_OUTPUT   2
#define SCRIPT_FIELD_COUNT    3 // register categories: Header (reserved) + Input + Output

static inline uint16_t ScriptAlign4(uint16_t size) { return (uint16_t)((size + 3u) & ~3u); }

// SCR_XX (8-char space-padded name, the slot index in hex).
static inline void ScriptFileName(uint8_t id, char out[8]) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'S'; out[1] = 'C'; out[2] = 'R'; out[3] = '_';
    out[4] = hex[(id >> 4) & 0x0F];
    out[5] = hex[id & 0x0F];
    out[6] = ' '; out[7] = ' ';
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
    uint8_t slot = 0;
    uint8_t state = (uint8_t)ScriptState::Stopped;
    uint32_t properties = 0;

    uint8_t inCount = 0, outCount = 0, varCount = 0, constCount = 0;
    BlockMeta *inMeta = nullptr, *outMeta = nullptr, *varMeta = nullptr, *constMeta = nullptr;

    uint8_t *ioSpace = nullptr;
    uint8_t *varSpace = nullptr;
    uint8_t *constSpace = nullptr;
    uint16_t inTotal = 0, outTotal = 0, varTotal = 0, constTotal = 0;

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
        inMeta = outMeta = varMeta = constMeta = nullptr;
        ioSpace = varSpace = constSpace = nullptr;
        instr = nullptr; lines = nullptr; blockMatch = nullptr; blockKind = nullptr; lineStamp = nullptr;
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

    uint16_t InputOffset(uint8_t i) const {
        uint16_t off = 0;
        for (uint8_t k = 0; k < i && k < inCount; k++) off += ScriptAlign4(inMeta[k].Size);
        return off;
    }
    uint16_t OutputOffset(uint8_t i) const {
        uint16_t off = inTotal;
        for (uint8_t k = 0; k < i && k < outCount; k++) off += ScriptAlign4(outMeta[k].Size);
        return off;
    }
    uint16_t VarOffset(uint8_t i) const {
        uint16_t off = 4;
        for (uint8_t k = 0; k < i && k < varCount; k++) off += ScriptAlign4(varMeta[k].Size);
        return off;
    }
    uint16_t ConstOffset(uint8_t i) const {
        uint16_t off = 0;
        for (uint8_t k = 0; k < i && k < constCount; k++) off += ScriptAlign4(constMeta[k].Size);
        return off;
    }
};

static LoadedScript scriptRegistry[MAX_SCRIPTS];
// Bit `i` is set for every slot that may hold a loaded script (a superset of the active
// slots, so hot loops can skip the empty registry entries).
static uint64_t scriptActiveMask = 0;

static inline void ScriptMaskSet(uint8_t slot, bool on) {
    if (on) scriptActiveMask |= (uint64_t)1 << slot;
    else    scriptActiveMask &= ~((uint64_t)1 << slot);
}

static LoadedScript *ScriptActive(uint8_t slot) {
    if (slot >= MAX_SCRIPTS) return nullptr;
    return scriptRegistry[slot].active ? &scriptRegistry[slot] : nullptr;
}

static uint16_t ScriptSumStrides(const BlockMeta *metas, uint8_t count) {
    uint16_t total = 0;
    for (uint8_t i = 0; i < count; i++) total += ScriptAlign4(metas[i].Size);
    return total;
}

static BlockMeta *ScriptAllocMetas(uint8_t count) {
    if (count == 0) return nullptr;
    return (BlockMeta *)calloc(count, sizeof(BlockMeta));
}

// ===== Program compilation (instructions -> lines + If/While block table) =====

static bool ScriptBuildProgram(LoadedScript *s, const uint8_t *instr, uint32_t instrLen) {
    s->instrSymbols = (uint16_t)(instrLen / 4);
    if (s->instrSymbols) {
        s->instr = (uint8_t *)malloc((size_t)s->instrSymbols * 4);
        if (!s->instr) return false;
        memcpy(s->instr, instr, (size_t)s->instrSymbols * 4);
    }

    // First pass: count lines.
    uint16_t count = 0;
    uint16_t i = 0;
    while (i < s->instrSymbols) {
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        i++; // instruction
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        if (i < s->instrSymbols) i++; // endline
        count++;
        if (count > SCRIPT_MAX_LINES) return false;
    }

    s->lineCount = count;
    if (count == 0) return true;
    s->lines = (ScriptLineInfo *)calloc(count, sizeof(ScriptLineInfo));
    s->blockMatch = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    s->blockKind = (uint8_t *)calloc(count, 1);
    s->lineStamp = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (!s->lines || !s->blockMatch || !s->blockKind || !s->lineStamp) return false;

    // Second pass: fill line descriptors and match If/While blocks. Mirrors the first
    // pass exactly (including orphan-symbol skipping) so indexes stay aligned.
    uint16_t *stack = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    if (!stack) return false;
    uint16_t sp = 0;
    uint16_t line = 0;
    i = 0;
    while (line < count) {
        uint16_t start = i;
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION &&
               s->instr[i * 4] != SCRIPT_SYM_ENDLINE) {
            i++;
        }
        if (i >= s->instrSymbols || s->instr[i * 4] != SCRIPT_SYM_INSTRUCTION) {
            while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
            if (i < s->instrSymbols) i++;
            continue;
        }
        uint8_t destCount = (uint8_t)(i - start);
        uint8_t cat = s->instr[i * 4 + 1];
        uint8_t op = s->instr[i * 4 + 2];
        i++; // instruction
        uint16_t opStart = i;
        while (i < s->instrSymbols && s->instr[i * 4] != SCRIPT_SYM_ENDLINE) i++;
        uint8_t opCount = (uint8_t)(i - opStart);
        if (i < s->instrSymbols) i++; // endline

        s->lines[line].start = start;
        s->lines[line].destCount = destCount;
        s->lines[line].opCount = opCount;
        s->blockMatch[line] = 0xFFFF;

        if (cat == SCRIPT_CAT_FLOW && (op == SCRIPT_OP_FLOW_IF || op == SCRIPT_OP_FLOW_WHILE)) {
            s->blockKind[line] = (op == SCRIPT_OP_FLOW_IF) ? 1 : 2;
            if (sp < count) stack[sp++] = line;
        } else if (cat == SCRIPT_CAT_FLOW && op == SCRIPT_OP_FLOW_END) {
            s->blockKind[line] = 3;
            if (sp > 0) {
                uint16_t open = stack[--sp];
                s->blockMatch[open] = line;
                s->blockMatch[line] = open;
            }
        }
        line++;
    }
    free(stack);
    return true;
}

// Parses SCR_<fileId> into the registry slot with the same index. Returns false (and leaves
// the slot inactive) when the file is missing or malformed.
static bool ScriptLoad(uint8_t fileId) {
    if (fileId >= MAX_SCRIPTS) return false;

    char name[8];
    ScriptFileName(fileId, name);
    uint32_t size = Storage.FileExists(name);
    if (size == 0xFFFFFFFF || size < SCRIPT_HEADER_SIZE) return false;

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return false;
    uint32_t got = Storage.ReadFromFile(name, 0, size, (char *)buf);
    if (got < SCRIPT_HEADER_SIZE) { free(buf); return false; }

    LoadedScript *s = &scriptRegistry[fileId];
    s->Release();
    ScriptMaskSet(fileId, false);
    s->slot = fileId;
    s->trid = (uint16_t)(SCRIPT_TRID_BASE + fileId);

    s->properties = ScriptRdU32(buf + 0);
    s->inCount    = buf[4];
    s->outCount   = buf[5];
    s->varCount   = buf[6];
    s->constCount = buf[7];
    uint32_t constLen = ScriptRdU32(buf + 8);
    uint32_t defLen   = ScriptRdU32(buf + 12);
    s->instrLen       = ScriptRdU32(buf + 16);
    uint32_t uiLen    = ScriptRdU32(buf + 20);

    uint32_t entries = (uint32_t)(s->inCount + s->outCount + s->varCount + s->constCount);
    uint32_t need = SCRIPT_HEADER_SIZE + entries * 4 + constLen + defLen + s->instrLen + uiLen;
    if (need > got || need > size) { free(buf); return false; }

    s->inMeta    = ScriptAllocMetas(s->inCount);
    s->outMeta   = ScriptAllocMetas(s->outCount);
    s->varMeta   = ScriptAllocMetas(s->varCount);
    s->constMeta = ScriptAllocMetas(s->constCount);
    if ((s->inCount && !s->inMeta) || (s->outCount && !s->outMeta) ||
        (s->varCount && !s->varMeta) || (s->constCount && !s->constMeta)) {
        s->Release(); free(buf); return false;
    }

    uint32_t off = SCRIPT_HEADER_SIZE;
    if (s->inCount)    { memcpy(s->inMeta,    buf + off, (size_t)s->inCount * 4);    off += (uint32_t)s->inCount * 4; }
    if (s->outCount)   { memcpy(s->outMeta,   buf + off, (size_t)s->outCount * 4);   off += (uint32_t)s->outCount * 4; }
    if (s->varCount)   { memcpy(s->varMeta,   buf + off, (size_t)s->varCount * 4);   off += (uint32_t)s->varCount * 4; }
    if (s->constCount) { memcpy(s->constMeta, buf + off, (size_t)s->constCount * 4); off += (uint32_t)s->constCount * 4; }

    s->inTotal    = ScriptSumStrides(s->inMeta, s->inCount);
    s->outTotal   = ScriptSumStrides(s->outMeta, s->outCount);
    s->varTotal   = ScriptSumStrides(s->varMeta, s->varCount);
    s->constTotal = ScriptSumStrides(s->constMeta, s->constCount);

    const uint8_t *constBlob = buf + off; off += constLen;
    const uint8_t *defBlob   = buf + off; off += defLen;
    const uint8_t *instrBlob = buf + off; off += s->instrLen;
    const uint8_t *uiBlob    = buf + off; off += uiLen;

    if (s->inTotal + s->outTotal) {
        s->ioSpace = (uint8_t *)calloc(1, (size_t)s->inTotal + s->outTotal);
        if (!s->ioSpace) { s->Release(); free(buf); return false; }
    }
    s->varSpace = (uint8_t *)calloc(1, (size_t)4 + s->varTotal);
    if (!s->varSpace) { s->Release(); free(buf); return false; }
    if (s->constTotal) {
        s->constSpace = (uint8_t *)calloc(1, s->constTotal);
        if (!s->constSpace) { s->Release(); free(buf); return false; }
    }

    // Compile the program (also copies the instruction symbols into RAM).
    if (!ScriptBuildProgram(s, instrBlob, s->instrLen)) { s->Release(); free(buf); return false; }

    // Inputs carry their defaults from the file (same 4-byte-strided packing as the space).
    for (uint8_t i = 0; i < s->inCount; i++) {
        uint32_t src = 0;
        for (uint8_t k = 0; k < i; k++) src += ScriptAlign4(s->inMeta[k].Size);
        if (src + s->inMeta[i].Size <= defLen)
            memcpy(s->ioSpace + s->InputOffset(i), defBlob + src, s->inMeta[i].Size);
    }
    for (uint8_t i = 0; i < s->constCount; i++) {
        uint32_t src = 0;
        for (uint8_t k = 0; k < i; k++) src += ScriptAlign4(s->constMeta[k].Size);
        if (src + s->constMeta[i].Size <= constLen)
            memcpy(s->constSpace + s->ConstOffset(i), constBlob + src, s->constMeta[i].Size);
    }

    // UI info (version 1) starts with a length-prefixed function name; fall back to the
    // file name. The remaining names/specs are app-side editor metadata.
    s->uiLen = (uint16_t)uiLen;
    s->name[0] = '\0';
    if (uiLen >= 2 && uiBlob[0] == 1) {
        uint8_t nlen = uiBlob[1];
        if (2u + nlen <= uiLen && nlen < BLOCK_NAME_LEN) {
            memcpy(s->name, uiBlob + 2, nlen);
            s->name[nlen] = '\0';
        }
    }
    if (s->name[0] == '\0') {
        memcpy(s->name, name, 6);
        s->name[6] = '\0';
    }

    // Force the key byte to 0 and enforce read-only on outputs/constants.
    for (uint8_t i = 0; i < s->inCount; i++)    s->inMeta[i].Key = 0;
    for (uint8_t i = 0; i < s->outCount; i++)   { s->outMeta[i].Key = 0; s->outMeta[i].FlagsAndType |= FieldFlags::ReadOnly; }
    for (uint8_t i = 0; i < s->varCount; i++)   s->varMeta[i].Key = 0;
    for (uint8_t i = 0; i < s->constCount; i++) { s->constMeta[i].Key = 0; s->constMeta[i].FlagsAndType |= FieldFlags::ReadOnly; }

    s->active = true;
    ScriptMaskSet(fileId, true);
    s->state = (uint8_t)ScriptState::Stopped;
    s->ic = 0;
    free(buf);
    return true;
}

static void ScriptUnload(uint8_t slot) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return;
    s->Release();
    ScriptMaskSet(slot, false);
}

// Number of loaded scripts + their slots (dense list, stable instance = slot).
static uint8_t ScriptListInstances(uint8_t *out, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_SCRIPTS && n < max; i++)
        if (scriptRegistry[i].active) out[n++] = i;
    return n;
}

static uint8_t ScriptKeyCount(uint8_t slot, uint8_t field) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return 0;
    switch (field) {
        case SCRIPT_FIELD_INPUT:    return s->inCount;
        case SCRIPT_FIELD_OUTPUT:   return s->outCount;
        default: return 0; // Header/Variables/Constants are not register content
    }
}

// Resolves one I/O entry into a descriptor + value copy. Only inputs (field 1) and
// outputs (field 2) are exposed through the Register.
static bool ScriptGetEntry(uint8_t slot, uint8_t field, uint8_t key, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return false;
    m = {};
    vsz = 0;

    const BlockMeta *meta = nullptr;
    const uint8_t *data = nullptr;
    if (field == SCRIPT_FIELD_INPUT && key < s->inCount) {
        meta = &s->inMeta[key]; data = s->ioSpace + s->InputOffset(key);
    } else if (field == SCRIPT_FIELD_OUTPUT && key < s->outCount) {
        meta = &s->outMeta[key]; data = s->ioSpace + s->OutputOffset(key);
    } else {
        return false;
    }

    m = *meta;
    uint8_t n = m.Size;
    if (n) memcpy(vbuf, data, n);
    vsz = n;
    return true;
}

// Writes an input (field 1) entry - the only writable Register entry of a script.
static bool ScriptSetEntry(uint8_t slot, uint8_t field, uint8_t key, const BlockMeta &m,
                           const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || field != SCRIPT_FIELD_INPUT || key >= s->inCount) return false;
    const BlockMeta *meta = &s->inMeta[key];
    uint8_t *data = s->ioSpace + s->InputOffset(key);
    if (meta->FlagsAndType & FieldFlags::ReadOnly) return false;
    if (BlockMetaType(meta->FlagsAndType) != BlockMetaType(m.FlagsAndType)) return false;
    if (vlen > meta->Size) return false;
    if (vlen) memcpy(data, val, vlen);
    // A short write defines the rest of the fixed-size input too: spaces for a string
    // (matching the static-block behaviour), zero otherwise, so no stale bytes survive.
    uint16_t type = BlockMetaType(meta->FlagsAndType);
    uint8_t fill = (type == (uint16_t)DataType::String || type == (uint16_t)DataType::Filename)
                       ? (uint8_t)' ' : 0;
    for (uint16_t i = vlen; i < meta->Size; i++) data[i] = fill;
    return true;
}

// Non-copying I/O lookup for subscription sources and cross-service register access:
// returns a pointer straight into the I/O space (inputs then outputs).
static bool ScriptGetIoPointer(uint8_t slot, uint8_t field, uint8_t key, BlockMeta &m, void *&data) {
    LoadedScript *s = ScriptActive(slot);
    if (!s) return false;
    if (field == SCRIPT_FIELD_INPUT && key < s->inCount) {
        m = s->inMeta[key];
        m.Key = key;
        data = s->ioSpace + s->InputOffset(key);
        return true;
    }
    if (field == SCRIPT_FIELD_OUTPUT && key < s->outCount) {
        m = s->outMeta[key];
        m.Key = key;
        data = s->ioSpace + s->OutputOffset(key);
        return true;
    }
    return false;
}

// Writes a variable's RAM (Script management CID 7 "Write Variable" - editor debug).
static bool ScriptSetVariable(uint8_t slot, uint8_t varId, const uint8_t *val, uint16_t vlen) {
    LoadedScript *s = ScriptActive(slot);
    if (!s || varId >= s->varCount) return false;
    if (vlen > s->varMeta[varId].Size) return false;
    if (vlen) memcpy(s->varSpace + s->VarOffset(varId), val, vlen);
    return true;
}

// ===== VM =====

struct ScriptScalar {
    Number n;
    int32_t i;
};

// Defined after the tick; used by the Script-state opcode so self state changes follow the
// same transition rules (IC reset, pending-confirmation clearing) as CID 4.
static void ScriptSetState(LoadedScript *s, uint8_t newState);

static inline uint16_t ScriptSymVal(const uint8_t *sym) { return (uint16_t)(sym[2] | (sym[3] << 8)); }

static inline bool ScriptIsNumericDtype(uint16_t dtype) {
    switch (dtype) {
        case (uint16_t)DataType::Bool:
        case (uint16_t)DataType::Index:
        case (uint16_t)DataType::Number:
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::Uint32:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id:
            return true;
        default:
            return false;
    }
}

static int32_t ScriptLoadInt(const uint8_t *d, uint8_t sz, bool sign) {
    int32_t v = 0;
    for (uint8_t i = 0; i < sz && i < 4; i++) v |= (int32_t)d[i] << (8 * i);
    if (sign && sz > 0 && sz < 4 && (d[sz - 1] & 0x80)) v |= ~((1 << (8 * sz)) - 1);
    return v;
}

static void ScriptStoreInt(uint8_t *d, uint8_t sz, int32_t v) {
    for (uint8_t i = 0; i < sz && i < 4; i++) d[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    for (uint8_t i = 4; i < sz; i++) d[i] = 0;
}

// Resolves a source symbol into bytes. Predefines are materialised into `scratch`.
static bool ScriptResolve(LoadedScript *s, uint8_t stype, uint8_t ssub, uint16_t sval,
                          uint8_t *scratch, const uint8_t **data, uint8_t *size, uint16_t *dtype) {
    switch (stype) {
        case SCRIPT_SYM_INPUT:
            if (sval >= s->inCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->InputOffset((uint8_t)sval);
            *size = s->inMeta[sval].Size;
            *dtype = BlockMetaType(s->inMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_OUTPUT:
            if (sval >= s->outCount || !s->ioSpace) return false;
            *data = s->ioSpace + s->OutputOffset((uint8_t)sval);
            *size = s->outMeta[sval].Size;
            *dtype = BlockMetaType(s->outMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_VARIABLE:
            if (sval >= s->varCount) return false;
            *data = s->varSpace + s->VarOffset((uint8_t)sval);
            *size = s->varMeta[sval].Size;
            *dtype = BlockMetaType(s->varMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_CONSTANT:
            if (sval >= s->constCount) return false;
            *data = s->constSpace + s->ConstOffset((uint8_t)sval);
            *size = s->constMeta[sval].Size;
            *dtype = BlockMetaType(s->constMeta[sval].FlagsAndType);
            return true;
        case SCRIPT_SYM_PREDEFINE:
            memset(scratch, 0, 4);
            switch (ssub) {
                case SCRIPT_PRE_INDEX: scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Index; break;
                case SCRIPT_PRE_CHAR:  scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Uint32; break;
                case SCRIPT_PRE_BOOL:  scratch[0] = sval ? 1 : 0; *size = 1; *dtype = (uint16_t)DataType::Bool; break;
                case SCRIPT_PRE_STATE: scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_MATHOP:scratch[0] = sval & 0xFF; *size = 1; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_TYPE:  scratch[0] = sval & 0xFF; scratch[1] = (sval >> 8) & 0xFF; *size = 2; *dtype = (uint16_t)DataType::Enum; break;
                case SCRIPT_PRE_NUMBER: {
                    // 16-bit Q8.8 literal -> 16.16 Number (e.g. 128 -> 0.5).
                    int32_t raw = (int32_t)(int16_t)sval << 8;
                    memcpy(scratch, &raw, 4);
                    *size = 4; *dtype = (uint16_t)DataType::Number; break;
                }
                default: return false;
            }
            *data = scratch;
            return true;
        default:
            return false;
    }
}

static bool ScriptToScalar(const uint8_t *data, uint8_t size, uint16_t dtype, bool num, ScriptScalar &out) {
    out.n = Number(0);
    out.i = 0;
    if (num) {
        switch (dtype) {
            case (uint16_t)DataType::Number: {
                if (size < 4) return false;
                int32_t raw = 0; memcpy(&raw, data, 4); out.n = Number::FromRaw(raw); return true;
            }
            case (uint16_t)DataType::Index: out.n = Number(ScriptLoadInt(data, size, true)); return true;
            case (uint16_t)DataType::Uint32: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Bool: out.n = Number(data[0] ? 1 : 0); return true;
            case (uint16_t)DataType::Enum: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::DevType: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            case (uint16_t)DataType::Id: out.n = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
            default: return false;
        }
    }
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            if (size < 4) return false;
            int32_t raw = 0; memcpy(&raw, data, 4); out.i = Number::FromRaw(raw).ToInt(); return true;
        }
        case (uint16_t)DataType::Index: out.i = ScriptLoadInt(data, size, true); return true;
        case (uint16_t)DataType::Uint32: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Bool: out.i = data[0] ? 1 : 0; return true;
        case (uint16_t)DataType::Enum: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::DevType: out.i = ScriptLoadInt(data, size, false); return true;
        case (uint16_t)DataType::Id: out.i = ScriptLoadInt(data, size, false); return true;
        default: return false;
    }
}

static bool ScriptResolveOperandScalar(LoadedScript *s, const uint8_t *sym, bool num, ScriptScalar &out) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    return ScriptToScalar(data, size, dtype, num, out);
}

static bool ScriptResolveOperandInt(LoadedScript *s, const uint8_t *sym, int32_t &out) {
    ScriptScalar sc;
    if (!ScriptResolveOperandScalar(s, sym, false, sc)) return false;
    out = sc.i;
    return true;
}

// Resolves a destination symbol (variable or output) into its storage.
static bool ScriptResolveDest(LoadedScript *s, const uint8_t *sym, uint16_t &dtype, uint8_t *&data, uint8_t &size) {
    if (sym[0] == SCRIPT_SYM_VARIABLE) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->varCount) return false;
        data = s->varSpace + s->VarOffset((uint8_t)idx);
        size = s->varMeta[idx].Size;
        dtype = BlockMetaType(s->varMeta[idx].FlagsAndType);
        return true;
    }
    if (sym[0] == SCRIPT_SYM_OUTPUT) {
        uint16_t idx = ScriptSymVal(sym);
        if (idx >= s->outCount || !s->ioSpace) return false;
        data = s->ioSpace + s->OutputOffset((uint8_t)idx);
        size = s->outMeta[idx].Size;
        dtype = BlockMetaType(s->outMeta[idx].FlagsAndType);
        return true;
    }
    return false;
}

static void ScriptStoreScalar(uint16_t dtype, uint8_t *data, uint8_t size, bool num, const ScriptScalar &v) {
    switch (dtype) {
        case (uint16_t)DataType::Number: {
            int32_t raw = num ? v.n.Value : Number(v.i).Value;
            memcpy(data, &raw, 4);
            break;
        }
        case (uint16_t)DataType::Bool: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            data[0] = iv ? 1 : 0;
            break;
        }
        default: {
            int32_t iv = num ? v.n.RoundToInt() : v.i;
            ScriptStoreInt(data, size, iv);
            break;
        }
    }
}

// Stores a resolved source into an already-resolved destination (numeric conversion when
// both sides are numeric, raw clamped copy otherwise).
static uint8_t ScriptAssignResolved(uint16_t dtype, uint8_t *dest, uint8_t dsize,
                                    const uint8_t *src, uint8_t ssize, uint16_t stype) {
    if (ScriptIsNumericDtype(dtype) && ScriptIsNumericDtype(stype)) {
        ScriptScalar sc;
        bool num = (dtype == (uint16_t)DataType::Number);
        if (!ScriptToScalar(src, ssize, stype, num, sc)) return SCRIPT_ERR_TYPE;
        ScriptStoreScalar(dtype, dest, dsize, num, sc);
    } else {
        uint8_t n = ssize < dsize ? ssize : dsize;
        if (n) memcpy(dest, src, n);
        for (uint8_t i = n; i < dsize; i++) dest[i] = 0;
    }
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptAssign(LoadedScript *s, const uint8_t *destSym, const uint8_t *src,
                            uint8_t ssize, uint16_t stype) {
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, destSym, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    return ScriptAssignResolved(dtype, dest, dsize, src, ssize, stype);
}

// Number of elements in a Vector/Matrix value (0 for scalars / malformed containers).
static uint16_t ScriptContainerCount(uint16_t dtype, uint8_t size, const uint8_t *data) {
    if (dtype == (uint16_t)DataType::Vector) return size / 4;
    if (dtype == (uint16_t)DataType::Matrix) {
        if (size < 4 || !data) return 0;
        uint16_t h = (uint16_t)(data[0] | (data[1] << 8));
        uint16_t w = (uint16_t)(data[2] | (data[3] << 8));
        uint32_t n = (uint32_t)h * w;
        if (n > (uint32_t)(size - 4) / 4) return 0;
        return (uint16_t)n;
    }
    return 0;
}

// Reads element `e` of a value as a Number. Scalars broadcast (the same value for any e).
static bool ScriptVectorElement(const uint8_t *data, uint8_t size, uint16_t dtype, uint32_t e, Number &out) {
    int32_t raw = 0;
    switch (dtype) {
        case (uint16_t)DataType::Number:
            if (size < 4) return false;
            memcpy(&raw, data, 4); out = Number::FromRaw(raw); return true;
        case (uint16_t)DataType::Index: out = Number(ScriptLoadInt(data, size, true)); return true;
        case (uint16_t)DataType::Uint32: out = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
        case (uint16_t)DataType::Bool: out = Number(data[0] ? 1 : 0); return true;
        case (uint16_t)DataType::Enum:
        case (uint16_t)DataType::DevType:
        case (uint16_t)DataType::Id: out = Number((int32_t)ScriptLoadInt(data, size, false)); return true;
        case (uint16_t)DataType::Vector:
            if ((e + 1) * 4 > size) return false;
            memcpy(&raw, data + e * 4, 4); out = Number::FromRaw(raw); return true;
        case (uint16_t)DataType::Matrix:
            if (e >= ScriptContainerCount(dtype, size, data)) return false;
            memcpy(&raw, data + 4 + e * 4, 4); out = Number::FromRaw(raw); return true;
        default: return false;
    }
}

// Element-wise math on a Vector/Matrix destination (Mod/Min/Max/Abs/Limit). Scalar
// operands broadcast, same-size containers apply element-wise.
static uint8_t ScriptExecVectorMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                    uint8_t cat, uint8_t op, uint16_t dtype, uint8_t *dest, uint8_t dsize) {
    // Only the element-wise instructions that remain (Modulo/Minimum/Maximum/Absolute/Limit);
    // arithmetic on a container goes through the Set expression.
    if (cat != SCRIPT_CAT_MATH) return SCRIPT_ERR_TYPE;
    if (op != SCRIPT_OP_MATH_MOD && op != SCRIPT_OP_MATH_MIN && op != SCRIPT_OP_MATH_MAX &&
        op != SCRIPT_OP_MATH_ABS && op != SCRIPT_OP_MATH_LIMIT)
        return SCRIPT_ERR_TYPE;

    uint16_t count = ScriptContainerCount(dtype, dsize, dest);
    if (count == 0) return SCRIPT_ERR_TYPE;
    uint8_t off = (dtype == (uint16_t)DataType::Matrix) ? 4 : 0;

    uint8_t n = ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    const uint8_t *odata[8];
    uint8_t osize[8];
    uint16_t otype[8];
    uint8_t oscratch[8][4];
    for (uint8_t i = 0; i < n; i++) {
        if (!ScriptResolve(s, opbase[(size_t)i * 4], opbase[(size_t)i * 4 + 1],
                           ScriptSymVal(opbase + (size_t)i * 4), oscratch[i],
                           &odata[i], &osize[i], &otype[i]))
            return SCRIPT_ERR_OPERAND;
    }

    for (uint32_t e = 0; e < count; e++) {
        Number acc;
        if (!ScriptVectorElement(odata[0], osize[0], otype[0], e, acc)) return SCRIPT_ERR_TYPE;
        if (op == SCRIPT_OP_MATH_LIMIT) {
            Number lo, hi;
            if (n < 3) return SCRIPT_ERR_OPERAND;
            if (!ScriptVectorElement(odata[1], osize[1], otype[1], e, lo)) return SCRIPT_ERR_TYPE;
            if (!ScriptVectorElement(odata[2], osize[2], otype[2], e, hi)) return SCRIPT_ERR_TYPE;
            if (acc < lo) acc = lo;
            if (acc > hi) acc = hi;
        } else if (op == SCRIPT_OP_MATH_ABS) {
            if (n != 1) return SCRIPT_ERR_TYPE; // Absolute is unary
            acc = abs(acc);
        } else {
            for (uint8_t k = 1; k < n; k++) {
                Number v;
                if (!ScriptVectorElement(odata[k], osize[k], otype[k], e, v)) return SCRIPT_ERR_TYPE;
                switch (op) {
                    case SCRIPT_OP_MATH_MOD: {
                        int32_t b = v.RoundToInt();
                        if (b == 0) return SCRIPT_ERR_TYPE;
                        acc = Number(acc.RoundToInt() % b);
                        break;
                    }
                    case SCRIPT_OP_MATH_MIN: acc = min(acc, v); break;
                    case SCRIPT_OP_MATH_MAX: acc = max(acc, v); break;
                    default: return SCRIPT_ERR_TYPE;
                }
            }
        }
        int32_t raw = acc.Value;
        memcpy(dest + off + e * 4, &raw, 4);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}

// ---------------------------------------------------------------------------
// Infix expression evaluation (the Set instruction): scalar, vector and matrix
// values, element-wise with scalar broadcast (Docs/Services/Script.md: "generic math
// and logic processor"). Limit/Min/Max stay separate instructions.
// ---------------------------------------------------------------------------

#define SCRIPT_EXPR_MAX_ELEMS 9 // covers Number, Vector2/3, Matrix2x3, Colour
#define SCRIPT_EXPR_MAX_DEPTH 8 // paren/`^` nesting cap (keeps the VM stack bounded)

struct ExprValue
{
    uint16_t dtype = (uint16_t)DataType::None;
    uint16_t mh = 0, mw = 0; // Matrix header (0 otherwise)
    uint8_t count = 0;       // elements (1 = scalar)
    Number e[SCRIPT_EXPR_MAX_ELEMS];
};

struct ExprParser
{
    LoadedScript *s;
    const uint8_t *tok;
    uint8_t n;
    uint8_t i;
    uint8_t depth;
    uint8_t scratch[4];
};

// Resolves a value token into an ExprValue (scalar / Vector / Matrix).
static bool ScriptExprResolve(ExprParser *p, const uint8_t *sym, ExprValue &out)
{
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(p->s, sym[0], sym[1], ScriptSymVal(sym), p->scratch, &data, &size, &dtype))
        return false;
    out.dtype = dtype;
    out.mh = 0;
    out.mw = 0;

    uint16_t cnt = 1;
    if (dtype == (uint16_t)DataType::Vector || dtype == (uint16_t)DataType::Matrix)
    {
        cnt = ScriptContainerCount(dtype, size, data);
        if (cnt == 0 || cnt > SCRIPT_EXPR_MAX_ELEMS) return false;
        if (dtype == (uint16_t)DataType::Matrix)
        {
            out.mh = (uint16_t)(data[0] | (data[1] << 8));
            out.mw = (uint16_t)(data[2] | (data[3] << 8));
        }
    }
    for (uint16_t i = 0; i < cnt; i++)
        if (!ScriptVectorElement(data, size, dtype, i, out.e[i])) return false;
    out.count = (uint8_t)cnt;
    return true;
}

// `Math op` value at the cursor, or 0xFF when the next token is a value / end.
static uint8_t ScriptExprPeekOp(ExprParser *p)
{
    if (p->i >= p->n) return 0xFF;
    const uint8_t *sym = p->tok + (size_t)p->i * 4;
    if (sym[0] == SCRIPT_SYM_PREDEFINE && sym[1] == SCRIPT_PRE_MATHOP)
        return (uint8_t)(ScriptSymVal(sym) & 0xFF);
    return 0xFF;
}

// base ^ exp: exp == 0.5 -> sqrt, integer exp -> repeated multiply (negative -> reciprocal).
static bool ScriptExprPow(Number base, Number exp, Number &out)
{
    if (exp.Value == (int32_t)(1 << 15)) { out = sqrt(base); return true; } // 0.5
    int32_t e = exp.RoundToInt();
    if (exp.Value != (e << 16)) return false; // only integer exponents (and 0.5)
    bool neg = e < 0;
    if (neg) e = -e;
    if (e > 64) return false;
    Number r = Number(1);
    for (int32_t i = 0; i < e; i++) r = r * base;
    if (neg) { if (r.Value == 0) return false; r = Number(1) / r; }
    out = r;
    return true;
}

// Comparisons and logic ops (scalar operands -> Bool).
static bool ScriptExprIsBoolOp(uint8_t op)
{
    return op == SCRIPT_MATHOP_EQ || op == SCRIPT_MATHOP_NE || op == SCRIPT_MATHOP_LT ||
           op == SCRIPT_MATHOP_LE || op == SCRIPT_MATHOP_GT || op == SCRIPT_MATHOP_GE ||
           op == SCRIPT_MATHOP_AND || op == SCRIPT_MATHOP_OR || op == SCRIPT_MATHOP_XOR;
}

// a = a <op> b, element-wise (a scalar broadcasts to a container's shape). Comparisons and
// logic produce a scalar Bool and require scalar operands.
static bool ScriptExprApply(ExprValue &a, const ExprValue &b, uint8_t op)
{
    if (ScriptExprIsBoolOp(op))
    {
        if (a.count != 1 || b.count != 1) return false;
        int32_t x = a.e[0].Value, y = b.e[0].Value;
        bool r;
        switch (op)
        {
        case SCRIPT_MATHOP_EQ: r = (x == y); break;
        case SCRIPT_MATHOP_NE: r = (x != y); break;
        case SCRIPT_MATHOP_LT: r = (x < y); break;
        case SCRIPT_MATHOP_LE: r = (x <= y); break;
        case SCRIPT_MATHOP_GT: r = (x > y); break;
        case SCRIPT_MATHOP_GE: r = (x >= y); break;
        case SCRIPT_MATHOP_AND: r = (x != 0) && (y != 0); break;
        case SCRIPT_MATHOP_OR: r = (x != 0) || (y != 0); break;
        case SCRIPT_MATHOP_XOR: r = (x != 0) != (y != 0); break;
        default: return false;
        }
        a.dtype = (uint16_t)DataType::Bool;
        a.mh = 0;
        a.mw = 0;
        a.count = 1;
        a.e[0] = Number(r ? 1 : 0);
        return true;
    }

    uint8_t count;
    if (a.count == b.count) count = a.count;
    else if (a.count == 1) count = b.count;
    else if (b.count == 1) count = a.count;
    else return false;
    const bool abcast = (a.count == 1);
    const Number asrc = abcast ? a.e[0] : Number(); // cached before the in-place writes
    if (abcast && b.count > 1) { a.dtype = b.dtype; a.mh = b.mh; a.mw = b.mw; }
    a.count = count;
    for (uint8_t k = 0; k < count; k++)
    {
        Number va = abcast ? asrc : a.e[k];
        Number vb = (b.count == 1) ? b.e[0] : b.e[k];
        switch (op)
        {
        case SCRIPT_MATHOP_ADD: va = va + vb; break;
        case SCRIPT_MATHOP_SUB: va = va - vb; break;
        case SCRIPT_MATHOP_MUL: va = va * vb; break;
        case SCRIPT_MATHOP_DIV: if (vb.Value == 0) return false; va = va / vb; break;
        case SCRIPT_MATHOP_MOD: {
            int32_t b = vb.RoundToInt();
            if (b == 0) return false;
            va = Number(va.RoundToInt() % b);
            break;
        }
        case SCRIPT_MATHOP_POW: if (!ScriptExprPow(va, vb, va)) return false; break;
        default: return false;
        }
        a.e[k] = va;
    }
    return true;
}

static bool ScriptExprBin(ExprParser *p, ExprValue &out, uint8_t minPrec);

static bool ScriptExprPrimary(ExprParser *p, ExprValue &out)
{
    if (p->depth >= SCRIPT_EXPR_MAX_DEPTH) return false;
    p->depth++;
    bool ok;
    if (ScriptExprPeekOp(p) == SCRIPT_MATHOP_OPEN)
    {
        p->i++;
        ok = ScriptExprBin(p, out, 1);
        if (ok)
        {
            if (ScriptExprPeekOp(p) != SCRIPT_MATHOP_CLOSE) ok = false;
            else p->i++;
        }
    }
    else if (p->i < p->n)
    {
        const uint8_t *sym = p->tok + (size_t)p->i * 4;
        ok = ScriptExprResolve(p, sym, out);
        if (ok) p->i++;
    }
    else
    {
        ok = false;
    }
    p->depth--;
    return ok;
}

// size v: Euclidean norm -> Number.
static bool ScriptExprFnSize(const ExprValue &a, ExprValue &out)
{
    Number s = Number(0);
    for (uint8_t k = 0; k < a.count; k++) s = s + a.e[k] * a.e[k];
    out.dtype = (uint16_t)DataType::Number;
    out.mh = 0;
    out.mw = 0;
    out.count = 1;
    out.e[0] = sqrt(s);
    return true;
}

// transpose m: Matrix R x C -> C x R.
static bool ScriptExprFnTranspose(const ExprValue &a, ExprValue &out)
{
    if (a.dtype != (uint16_t)DataType::Matrix || a.mh == 0 || a.mw == 0) return false;
    uint16_t R = a.mh, C = a.mw;
    if ((uint32_t)R * C > SCRIPT_EXPR_MAX_ELEMS) return false;
    out.dtype = (uint16_t)DataType::Matrix;
    out.mh = C;
    out.mw = R;
    out.count = a.count;
    for (uint16_t r = 0; r < R; r++)
        for (uint16_t c = 0; c < C; c++)
            out.e[c * R + r] = a.e[r * C + c];
    return true;
}

// dot a b: sum(a_i * b_i) -> Number.
static bool ScriptExprFnDot(const ExprValue &a, const ExprValue &b, ExprValue &out)
{
    if (a.count == 0 || a.count != b.count) return false;
    Number s = Number(0);
    for (uint8_t k = 0; k < a.count; k++) s = s + a.e[k] * b.e[k];
    out.dtype = (uint16_t)DataType::Number;
    out.mh = 0;
    out.mw = 0;
    out.count = 1;
    out.e[0] = s;
    return true;
}

// cross a b: Vector3 x Vector3 -> Vector3.
static bool ScriptExprFnCross(const ExprValue &a, const ExprValue &b, ExprValue &out)
{
    if (a.count != 3 || b.count != 3) return false;
    out.dtype = (uint16_t)DataType::Vector;
    out.mh = 0;
    out.mw = 0;
    out.count = 3;
    out.e[0] = a.e[1] * b.e[2] - a.e[2] * b.e[1];
    out.e[1] = a.e[2] * b.e[0] - a.e[0] * b.e[2];
    out.e[2] = a.e[0] * b.e[1] - a.e[1] * b.e[0];
    return true;
}

static bool ScriptExprUnary(ExprParser *p, ExprValue &out)
{
    uint8_t op = ScriptExprPeekOp(p);
    if (op == SCRIPT_MATHOP_FN_SIZE || op == SCRIPT_MATHOP_FN_TRANSPOSE)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        return (op == SCRIPT_MATHOP_FN_SIZE) ? ScriptExprFnSize(a, out)
                                             : ScriptExprFnTranspose(a, out);
    }
    if (op == SCRIPT_MATHOP_FN_DOT || op == SCRIPT_MATHOP_FN_CROSS)
    {
        p->i++;
        ExprValue a, b;
        if (!ScriptExprUnary(p, a)) return false;
        if (!ScriptExprUnary(p, b)) return false;
        return (op == SCRIPT_MATHOP_FN_DOT) ? ScriptExprFnDot(a, b, out)
                                            : ScriptExprFnCross(a, b, out);
    }
    if (op == SCRIPT_MATHOP_SUB)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        for (uint8_t k = 0; k < a.count; k++) a.e[k] = -a.e[k];
        out = a;
        return true;
    }
    if (op == SCRIPT_MATHOP_NOT)
    {
        p->i++;
        ExprValue a;
        if (!ScriptExprUnary(p, a)) return false;
        if (a.count != 1) return false;
        out.dtype = (uint16_t)DataType::Bool;
        out.mh = 0;
        out.mw = 0;
        out.count = 1;
        out.e[0] = Number(a.e[0].Value == 0 ? 1 : 0);
        return true;
    }
    return ScriptExprPrimary(p, out);
}

// Precedence climbing: MUL/DIV bind tighter than ADD/SUB; POW is right-associative.
static bool ScriptExprBin(ExprParser *p, ExprValue &out, uint8_t minPrec)
{
    if (!ScriptExprUnary(p, out)) return false;
    for (;;)
    {
        uint8_t op = ScriptExprPeekOp(p);
        uint8_t prec = (op == SCRIPT_MATHOP_POW) ? 9
                       : (op == SCRIPT_MATHOP_MUL || op == SCRIPT_MATHOP_DIV ||
                          op == SCRIPT_MATHOP_MOD) ? 8
                       : (op == SCRIPT_MATHOP_ADD || op == SCRIPT_MATHOP_SUB) ? 7
                       : (op == SCRIPT_MATHOP_LT || op == SCRIPT_MATHOP_LE ||
                          op == SCRIPT_MATHOP_GT || op == SCRIPT_MATHOP_GE) ? 5
                       : (op == SCRIPT_MATHOP_EQ || op == SCRIPT_MATHOP_NE) ? 4
                       : (op == SCRIPT_MATHOP_AND) ? 3
                       : (op == SCRIPT_MATHOP_XOR) ? 2
                       : (op == SCRIPT_MATHOP_OR) ? 1 : 0;
        if (prec == 0 || prec < minPrec) break;
        p->i++;
        ExprValue b;
        uint8_t next = (op == SCRIPT_MATHOP_POW) ? prec : (uint8_t)(prec + 1);
        if (!ScriptExprBin(p, b, next)) return false;
        if (!ScriptExprApply(out, b, op)) return false;
    }
    return true;
}

// Evaluates the Set line's token stream and stores it into the destination.
static uint8_t ScriptExecSet(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                             uint16_t dtype, uint8_t *dest, uint8_t dsize)
{
    if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
    ExprParser p;
    p.s = s;
    p.tok = opbase;
    p.n = ln.opCount;
    p.i = 0;
    p.depth = 0;
    ExprValue v;
    if (!ScriptExprBin(&p, v, 1)) return SCRIPT_ERR_OPERAND;
    if (p.i != p.n) return SCRIPT_ERR_OPERAND; // trailing tokens

    if (dtype == (uint16_t)DataType::Vector)
    {
        uint8_t dcount = dsize / 4;
        if (dcount == 0 || v.count != dcount) return SCRIPT_ERR_TYPE;
        for (uint8_t k = 0; k < dcount; k++)
        {
            int32_t raw = v.e[k].Value;
            memcpy(dest + (size_t)k * 4, &raw, 4);
        }
    }
    else if (dtype == (uint16_t)DataType::Matrix)
    {
        // A Matrix destination takes its dimensions from the value's header (the dest slot
        // may be uninitialised). The value must be a Matrix (or a Matrix expression result).
        if (v.dtype != (uint16_t)DataType::Matrix || v.count == 0) return SCRIPT_ERR_TYPE;
        if ((uint32_t)dsize < 4u + (uint32_t)v.count * 4u) return SCRIPT_ERR_TYPE;
        dest[0] = (uint8_t)(v.mh & 0xFF);
        dest[1] = (uint8_t)(v.mh >> 8);
        dest[2] = (uint8_t)(v.mw & 0xFF);
        dest[3] = (uint8_t)(v.mw >> 8);
        for (uint8_t k = 0; k < v.count; k++)
        {
            int32_t raw = v.e[k].Value;
            memcpy(dest + 4 + (size_t)k * 4, &raw, 4);
        }
    }
    else
    {
        if (v.count != 1) return SCRIPT_ERR_TYPE;
        ScriptScalar sc;
        sc.n = v.e[0];
        sc.i = v.e[0].RoundToInt();
        ScriptStoreScalar(dtype, dest, dsize, dtype == (uint16_t)DataType::Number, sc);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}

// Evaluates a token stream as an expression and returns its truth value (used by the flow
// instructions and Wait until).
static bool ScriptExprTruthy(LoadedScript *s, const uint8_t *tok, uint8_t n, bool &ok)
{
    if (n < 1) return false;
    ExprParser p;
    p.s = s;
    p.tok = tok;
    p.n = n;
    p.i = 0;
    p.depth = 0;
    ExprValue v;
    if (!ScriptExprBin(&p, v, 1)) return false;
    if (p.i != p.n) return false;
    ok = (v.count > 0) && (v.e[0].Value != 0);
    return true;
}

// Transform dest = rot, ox, oy, sx, sy [, skew] -> a 2x3 affine matrix (rotation in
// radians), matching the render Position/Offset format.
static uint8_t ScriptExecTransform(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                   uint16_t dtype, uint8_t *dest, uint8_t dsize)
{
    if (dtype != (uint16_t)DataType::Matrix || dsize < 4 + 6 * 4) return SCRIPT_ERR_TYPE;
    if (ln.opCount < 5) return SCRIPT_ERR_OPERAND;
    Number v[6] = {Number(0), Number(0), Number(0), Number(0), Number(0), Number(0)};
    uint8_t n = ln.opCount;
    if (n > 6) n = 6;
    for (uint8_t i = 0; i < n; i++)
    {
        ScriptScalar sc;
        if (!ScriptResolveOperandScalar(s, opbase + (size_t)i * 4, true, sc)) return SCRIPT_ERR_OPERAND;
        v[i] = sc.n;
    }
    Number rot = v[0], tx = v[1], ty = v[2], sx = v[3], sy = v[4], skew = v[5];
    Number c = cos(rot), s2 = sin(rot);
    Number k = Number(0);
    if (skew.Value != 0)
    {
        Number cd = cos(skew);
        if (cd.Value != 0) k = sin(skew) / cd;
    }
    // Cells [a b tx; c d ty] (matches the app's Transform23).
    Number cells[6];
    cells[0] = sx * c;
    cells[1] = sx * c * k + sy * s2;
    cells[2] = tx;
    cells[3] = -sx * s2;
    cells[4] = -sx * s2 * k + sy * c;
    cells[5] = ty;
    dest[0] = 2; dest[1] = 0; dest[2] = 3; dest[3] = 0;
    for (uint8_t i = 0; i < 6; i++)
    {
        int32_t raw = cells[i].Value;
        memcpy(dest + 4 + (size_t)i * 4, &raw, 4);
    }
    s->ic++;
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptExecMath(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t cat, uint8_t op) {
    if (ln.destCount < 1) return SCRIPT_ERR_OPERAND;
    uint16_t dtype = 0;
    uint8_t *dest = nullptr;
    uint8_t dsize = 0;
    if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
    bool num = (dtype == (uint16_t)DataType::Number);

    // SET evaluates an infix expression (scalar / vector / matrix).
    if (cat == SCRIPT_CAT_MATH && op == SCRIPT_OP_MATH_SET) {
        return ScriptExecSet(s, ln, opbase, dtype, dest, dsize);
    }

    // Transform: a 2x3 affine matrix from (rot, ox, oy, sx, sy[, skew]).
    if (cat == SCRIPT_CAT_MATH && op == SCRIPT_OP_MATH_TRANSFORM) {
        return ScriptExecTransform(s, ln, opbase, dtype, dest, dsize);
    }

    // Vector/Matrix destination: element-wise math (operands resolved raw).
    if (dtype == (uint16_t)DataType::Vector || dtype == (uint16_t)DataType::Matrix) {
        return ScriptExecVectorMath(s, ln, opbase, cat, op, dtype, dest, dsize);
    }

    ScriptScalar in[8];
    uint8_t n = ln.opCount;
    if (n > 8) n = 8;
    if (n < 1) return SCRIPT_ERR_OPERAND;
    for (uint8_t i = 0; i < n; i++) {
        if (!ScriptResolveOperandScalar(s, opbase + (size_t)i * 4, num, in[i])) return SCRIPT_ERR_OPERAND;
    }

    ScriptScalar out;
    out.n = Number(0);
    out.i = 0;
    switch (cat) {
        case SCRIPT_CAT_MATH:
            // Fold every operand left-to-right (n-ary: dest = a op b op c ...).
            out = in[0];
            switch (op) {
                case SCRIPT_OP_MATH_MOD:
                    for (uint8_t k = 1; k < n; k++) {
                        if (num) { int32_t b = in[k].n.RoundToInt(); if (b == 0) return SCRIPT_ERR_TYPE; out.n = Number(out.n.RoundToInt() % b); }
                        else { if (in[k].i == 0) return SCRIPT_ERR_TYPE; out.i %= in[k].i; }
                    }
                    break;
                case SCRIPT_OP_MATH_MIN:
                    for (uint8_t k = 1; k < n; k++) { if (num) out.n = min(out.n, in[k].n); else out.i = out.i < in[k].i ? out.i : in[k].i; }
                    break;
                case SCRIPT_OP_MATH_MAX:
                    for (uint8_t k = 1; k < n; k++) { if (num) out.n = max(out.n, in[k].n); else out.i = out.i > in[k].i ? out.i : in[k].i; }
                    break;
                case SCRIPT_OP_MATH_ABS: if (num) out.n = abs(out.n); else out.i = out.i < 0 ? -out.i : out.i; break;
                case SCRIPT_OP_MATH_LIMIT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    if (num) {
                        Number lo = in[1].n, hi = in[2].n;
                        if (out.n.Value < lo.Value) out.n = lo;
                        if (out.n.Value > hi.Value) out.n = hi;
                    } else {
                        int32_t lo = in[1].i, hi = in[2].i;
                        if (out.i < lo) out.i = lo;
                        if (out.i > hi) out.i = hi;
                    }
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        case SCRIPT_CAT_LOGIC:
            // Comparisons/logic moved into the expression (Set/If/While); only Select remains.
            switch (op) {
                case SCRIPT_OP_LOGIC_SELECT: {
                    if (n < 3) return SCRIPT_ERR_OPERAND;
                    bool cond = num ? (in[0].n.Value != 0) : (in[0].i != 0);
                    if (num) out.n = cond ? in[1].n : in[2].n; else out.i = cond ? in[1].i : in[2].i;
                    break;
                }
                default: return SCRIPT_ERR_UNKNOWN_OP;
            }
            break;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }

    ScriptStoreScalar(dtype, dest, dsize, num, out);
    s->ic++;
    return SCRIPT_ERR_NONE;
}

static uint8_t ScriptExecFlow(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op) {
    switch (op) {
        case SCRIPT_OP_FLOW_IF: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
            uint16_t match = s->blockMatch[s->ic];
            s->ic = ok ? s->ic + 1 : (match == 0xFFFF ? s->ic + 1 : match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_WHILE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
            uint16_t match = s->blockMatch[s->ic];
            s->ic = ok ? s->ic + 1 : (match == 0xFFFF ? s->ic + 1 : match + 1);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_END: {
            uint16_t open = (s->blockMatch[s->ic] == 0xFFFF) ? 0xFFFF : s->blockMatch[s->ic];
            if (open != 0xFFFF && s->blockKind[open] == 2) { s->ic = open; return SCRIPT_ERR_NONE; } // while -> re-check
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_JUMP: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t target = 0;
            if (!ScriptResolveOperandInt(s, opbase, target)) return SCRIPT_ERR_OPERAND;
            if (target < 0 || target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            s->ic = (uint16_t)target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_CALL: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t target = 0;
            if (!ScriptResolveOperandInt(s, opbase, target)) return SCRIPT_ERR_OPERAND;
            if (target < 0 || target >= s->lineCount) return SCRIPT_ERR_BOUNDS;
            if (s->callDepth >= SCRIPT_MAX_CALL_DEPTH) return SCRIPT_ERR_STACK;
            s->callStack[s->callDepth++] = (uint16_t)(s->ic + 1);
            s->ic = (uint16_t)target;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_RETURN: {
            if (s->callDepth == 0) { s->state = (uint8_t)ScriptState::Finished; return SCRIPT_ERR_NONE; }
            s->ic = s->callStack[--s->callDepth];
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_FLOW_HALT:
            s->state = (uint8_t)ScriptState::Finished;
            return SCRIPT_ERR_NONE;
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

static uint8_t ScriptExecTime(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op, uint32_t nowMs, bool &yield) {
    switch (op) {
        case SCRIPT_OP_TIME_DELAY: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            int32_t ms = 0;
            if (!ScriptResolveOperandInt(s, opbase, ms)) return SCRIPT_ERR_OPERAND;
            if (ms < 0) ms = 0;
            s->ic++;
            s->waitUntil = nowMs + (uint32_t)ms;
            s->waitingOnTime = true;
            s->state = (uint8_t)ScriptState::Waiting;
            yield = true;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_WAIT: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            bool ok = false;
            if (!ScriptExprTruthy(s, opbase, ln.opCount, ok)) return SCRIPT_ERR_OPERAND;
            if (!ok) {
                s->waitingOnTime = false;
                s->state = (uint8_t)ScriptState::Waiting;
                yield = true; // ic stays: re-evaluated on resume
                return SCRIPT_ERR_NONE;
            }
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_TIME_GET: {
            if (ln.destCount < 1) return SCRIPT_ERR_OPERAND;
            uint16_t dtype = 0; uint8_t *dest = nullptr; uint8_t dsize = 0;
            if (!ScriptResolveDest(s, s->instr + (size_t)ln.start * 4, dtype, dest, dsize)) return SCRIPT_ERR_OPERAND;
            // Time is a whole millisecond count. A Q16.16 Number destination only has a
            // 15-bit integer part (it overflows past ~32767 ms), so restrict Get time to
            // the integer types.
            if (dtype != (uint16_t)DataType::Index && dtype != (uint16_t)DataType::Uint32)
                return SCRIPT_ERR_TYPE;
            ScriptScalar v;
            v.i = (int32_t)nowMs;
            v.n = Number::FromRaw(0); // unused: integer destination
            ScriptStoreScalar(dtype, dest, dsize, false, v);
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

// Reads a 4-byte BlockInfo from a Constant operand.
static bool ScriptOperandBlockInfo(LoadedScript *s, const uint8_t *sym, uint32_t &bi) {
    uint8_t scratch[4];
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    if (size < 4) return false;
    memcpy(&bi, data, 4);
    return true;
}

static void ScriptSendRegisterRequest(LoadedScript *s, uint16_t addr, uint8_t cid,
                                      const uint8_t *payload, uint16_t len) {
    PacketFrame req;
    PacketConstruct(&req, addr, MakeService(ServiceType::Register, cid), s->trid,
                    FLAG_REQACK | FLAG_START | FLAG_STOP, payload, len);
    SendAndVerifyPacket(req);
}

static uint8_t ScriptExecService(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase,
                                 uint8_t op, uint32_t nowMs, bool &yield) {
    switch (op) {
        case SCRIPT_OP_SERVICE_NOP:
            s->ic++;
            return SCRIPT_ERR_NONE;
        case SCRIPT_OP_SERVICE_LOG: {
            // Custom log (docs "Custom logs"): the first operand's value becomes the
            // log code; the source is the Script service tagged with the instance.
            uint16_t code = (uint16_t)(s->slot & 0xFF);
            if (ln.opCount >= 1) {
                int32_t v = 0;
                if (ScriptResolveOperandInt(s, opbase, v)) code = (uint16_t)(v & 0xFFFF);
            }
            ReportLog(MakeLog(false, (uint16_t)ServiceType::Script, code, 0));
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_STATE: {
            if (ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            if (opbase[0] == SCRIPT_SYM_PREDEFINE && opbase[1] == SCRIPT_PRE_STATE) {
                uint8_t st = (uint8_t)(ScriptSymVal(opbase) & 0xFF);
                if (st > (uint8_t)ScriptState::Error) return SCRIPT_ERR_OPERAND;
                ScriptSetState(s, st);
                if (st == (uint8_t)ScriptState::Stopped || st == (uint8_t)ScriptState::Finished ||
                    st == (uint8_t)ScriptState::Error)
                    return SCRIPT_ERR_NONE; // terminal: the run loop stops here
                s->ic++;
                return SCRIPT_ERR_NONE;
            }
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ: { // dest = register[BlockInfo]
            if (ln.destCount < 1 || ln.opCount < 1) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase, bi)) return SCRIPT_ERR_OPERAND;
            BlockMeta rm;
            uint8_t rbuf[256]; // a register value can be up to the u8 size limit
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t err = ScriptAssign(s, s->instr + (size_t)ln.start * 4, rbuf, rsz,
                                       BlockMetaType(rm.FlagsAndType));
            if (err) return err;
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE: { // register[BlockInfo] = operand2
            if (ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase, bi)) return SCRIPT_ERR_OPERAND;
            BlockMeta rm;
            uint8_t rbuf[256];
            uint8_t rsz = 0;
            if (!RegisterGetByBlockInfo(bi, rm, rbuf, rsz)) return SCRIPT_ERR_REGISTER;
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[4], opbase[5], ScriptSymVal(opbase + 4), scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            uint8_t wbuf[256];
            memset(wbuf, 0, sizeof(wbuf));
            uint8_t err = ScriptAssignResolved(BlockMetaType(rm.FlagsAndType), wbuf, rm.Size, val, vsize, vtype);
            if (err) return err;
            BlockMeta wm = rm;
            wm.FlagsAndType = (uint16_t)(rm.FlagsAndType | FieldFlags::ScriptUpdated);
            wm.Size = rm.Size;
            if (!RegisterSetByBlockInfo(bi, wm, wbuf, rm.Size)) return SCRIPT_ERR_REGISTER;
            s->ic++;
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_READ_FOREIGN: { // dest = register[addr, BlockInfo]
            if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
            int32_t addr = 0;
            if (!ScriptResolveOperandInt(s, opbase, addr)) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase + 4, bi)) return SCRIPT_ERR_OPERAND;
            memcpy(s->pendingDest, s->instr + (size_t)ln.start * 4, 4);
            s->pendingRead = true;
            s->pendingForeign = true;
            s->pendingTrid = s->trid;
            s->pendingDeadline = nowMs + 500;
            s->ic++;
            s->state = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            yield = true;
            uint8_t payload[4];
            memcpy(payload, &bi, 4);
            ScriptSendRegisterRequest(s, (uint16_t)addr, 1, payload, 4);
            return SCRIPT_ERR_NONE;
        }
        case SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN: { // register[addr, BlockInfo] = operand3
            if (ln.opCount < 3) return SCRIPT_ERR_OPERAND;
            int32_t addr = 0;
            if (!ScriptResolveOperandInt(s, opbase, addr)) return SCRIPT_ERR_OPERAND;
            uint32_t bi = 0;
            if (!ScriptOperandBlockInfo(s, opbase + 4, bi)) return SCRIPT_ERR_OPERAND;
            uint8_t scratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[8], opbase[9], ScriptSymVal(opbase + 8), scratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            if (vsize > 64) vsize = 64;
            uint8_t payload[8 + 64];
            memcpy(payload, &bi, 4);
            BlockMeta wm;
            wm.FlagsAndType = vtype;
            wm.Key = 0;
            wm.Size = vsize;
            memcpy(payload + 4, &wm, 4);
            if (vsize) memcpy(payload + 8, val, vsize);
            s->pendingRead = false;
            s->pendingForeign = true;
            s->pendingTrid = s->trid;
            s->pendingDeadline = nowMs + 500;
            s->ic++;
            s->state = (uint8_t)ScriptState::Waiting;
            s->waitingOnTime = false;
            yield = true;
            ScriptSendRegisterRequest(s, (uint16_t)addr, 2, payload, (uint16_t)(8 + vsize));
            return SCRIPT_ERR_NONE;
        }
        default:
            return SCRIPT_ERR_NOT_IMPL;
    }
}

// Resolves one element of a vector/matrix/colour/string container.
static bool ScriptContainerElement(LoadedScript *s, const uint8_t *sym, int32_t index,
                                   uint8_t *scratch, const uint8_t **elem, uint8_t *elemSize,
                                   uint16_t *elemType) {
    const uint8_t *data = nullptr;
    uint8_t size = 0;
    uint16_t dtype = 0;
    if (!ScriptResolve(s, sym[0], sym[1], ScriptSymVal(sym), scratch, &data, &size, &dtype)) return false;
    switch (dtype) {
        case (uint16_t)DataType::Vector:
            if (index < 0 || (uint32_t)(index + 1) * 4 > size) return false;
            *elem = data + (size_t)index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        case (uint16_t)DataType::Matrix: {
            if (size < 4 || index < 0) return false;
            uint16_t h = (uint16_t)(data[0] | (data[1] << 8));
            uint16_t w = (uint16_t)(data[2] | (data[3] << 8));
            if ((uint32_t)index >= (uint32_t)h * w) return false;
            if (4u + (uint32_t)(index + 1) * 4 > size) return false;
            *elem = data + 4 + (size_t)index * 4;
            *elemSize = 4;
            *elemType = (uint16_t)DataType::Number;
            return true;
        }
        case (uint16_t)DataType::Colour:
            if (index < 0 || index > 3 || size < 4) return false;
            *elem = data + index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Uint32;
            return true;
        case (uint16_t)DataType::String:
            if (index < 0 || index >= size) return false;
            *elem = data + index;
            *elemSize = 1;
            *elemType = (uint16_t)DataType::Uint32;
            return true;
        default:
            if (index != 0) return false;
            *elem = data;
            *elemSize = size;
            *elemType = dtype;
            return true;
    }
}

// Compose: container[index...] = value...   Extract: dest = container[index]
static uint8_t ScriptExecCompose(LoadedScript *s, const ScriptLineInfo &ln, const uint8_t *opbase, uint8_t op) {
    if (op == SCRIPT_OP_COMPOSE) {
        if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        const uint8_t *destSym = s->instr + (size_t)ln.start * 4;
        // The container must be writable (a variable or an output).
        if (destSym[0] != SCRIPT_SYM_VARIABLE && destSym[0] != SCRIPT_SYM_OUTPUT)
            return SCRIPT_ERR_OPERAND;
        int32_t index = 0;
        if (!ScriptResolveOperandInt(s, opbase, index)) return SCRIPT_ERR_OPERAND;
        for (uint8_t k = 1; k < ln.opCount; k++) {
            uint8_t vscratch[4];
            const uint8_t *val = nullptr;
            uint8_t vsize = 0;
            uint16_t vtype = 0;
            if (!ScriptResolve(s, opbase[k * 4], opbase[k * 4 + 1], ScriptSymVal(opbase + (size_t)k * 4),
                               vscratch, &val, &vsize, &vtype))
                return SCRIPT_ERR_OPERAND;
            const uint8_t *elem = nullptr;
            uint8_t esize = 0;
            uint16_t etype = 0;
            // Separate scratch: `val` may point into `vscratch`.
            uint8_t escratch[4];
            if (!ScriptContainerElement(s, destSym, index + (k - 1), escratch, &elem, &esize, &etype))
                return SCRIPT_ERR_BOUNDS;
            uint8_t err = ScriptAssignResolved(etype, const_cast<uint8_t *>(elem), esize, val, vsize, vtype);
            if (err) return err;
        }
        s->ic++;
        return SCRIPT_ERR_NONE;
    }
    if (op == SCRIPT_OP_EXTRACT) {
        if (ln.destCount < 1 || ln.opCount < 2) return SCRIPT_ERR_OPERAND;
        int32_t index = 0;
        if (!ScriptResolveOperandInt(s, opbase + 4, index)) return SCRIPT_ERR_OPERAND;
        uint8_t scratch[4];
        const uint8_t *elem = nullptr;
        uint8_t esize = 0;
        uint16_t etype = 0;
        if (!ScriptContainerElement(s, opbase, index, scratch, &elem, &esize, &etype)) return SCRIPT_ERR_BOUNDS;
        uint8_t err = ScriptAssign(s, s->instr + (size_t)ln.start * 4, elem, esize, etype);
        if (err) return err;
        s->ic++;
        return SCRIPT_ERR_NONE;
    }
    return SCRIPT_ERR_UNKNOWN_OP;
}

static uint8_t ScriptExecLine(LoadedScript *s, uint32_t nowMs, bool &yield) {
    const ScriptLineInfo &ln = s->lines[s->ic];
    const uint8_t *instr = s->instr + (size_t)(ln.start + ln.destCount) * 4;
    uint8_t cat = instr[1];
    uint8_t op = instr[2];
    const uint8_t *opbase = instr + 4;
    switch (cat) {
        case SCRIPT_CAT_MATH:
        case SCRIPT_CAT_LOGIC:
            return ScriptExecMath(s, ln, opbase, cat, op);
        case SCRIPT_CAT_FLOW:
            return ScriptExecFlow(s, ln, opbase, op);
        case SCRIPT_CAT_TIME:
            return ScriptExecTime(s, ln, opbase, op, nowMs, yield);
        case SCRIPT_CAT_SERVICE:
            return ScriptExecService(s, ln, opbase, op, nowMs, yield);
        case SCRIPT_CAT_COMPOSE:
            return ScriptExecCompose(s, ln, opbase, op);
        default:
            return SCRIPT_ERR_UNKNOWN_OP;
    }
}

static uint32_t scriptTick = 0;

static void ScriptRun(LoadedScript *s, uint32_t nowMs) {
    uint16_t steps = 0;
    while (steps++ < SCRIPT_INSTR_BUDGET) {
        if (s->lineCount == 0 || s->ic >= s->lineCount) { s->state = (uint8_t)ScriptState::Finished; return; }
        if (s->lineStamp && s->lineStamp[s->ic] == scriptTick) return; // looped this tick -> yield
        if (s->lineStamp) s->lineStamp[s->ic] = scriptTick;
        bool yield = false;
        uint8_t err = ScriptExecLine(s, nowMs, yield);
        if (err) { s->errorCode = err; s->state = (uint8_t)ScriptState::Error; return; }
        if (yield) return;
        if (s->state != (uint8_t)ScriptState::Running) return; // Finished/Error/Paused set by the op
    }
}

void ScriptsTick(uint32_t nowMs) {
    scriptTick++;
    uint64_t mask = scriptActiveMask;
    while (mask) {
        uint8_t i = (uint8_t)__builtin_ctzll(mask);
        mask &= mask - 1;
        LoadedScript *s = &scriptRegistry[i];
        if (!s->active) continue;
        if (s->state == (uint8_t)ScriptState::Running) {
            ScriptRun(s, nowMs);
        } else if (s->state == (uint8_t)ScriptState::Waiting) {
            if (s->pendingForeign) {
                if ((int32_t)(nowMs - s->pendingDeadline) >= 0) {
                    s->pendingForeign = false;
                    s->errorCode = SCRIPT_ERR_TIMEOUT;
                    s->state = (uint8_t)ScriptState::Error;
                }
                continue;
            }
            if (s->waitingOnTime && (int32_t)(nowMs - s->waitUntil) >= 0) {
                s->waitingOnTime = false;
                s->state = (uint8_t)ScriptState::Running;
            } else if (!s->waitingOnTime) {
                s->state = (uint8_t)ScriptState::Running; // WaitUntil: re-evaluate
            }
        }
    }
}

// Handles a reply to a foreign register request. The reply's CMD carries the script's
// TRID, so the Dispatcher routes it here by range instead of by ServiceType.
static void HandleScriptResponse(const PacketFrame &frame) {
    uint16_t trid = frame.srv_tgt;
    uint64_t mask = scriptActiveMask;
    while (mask) {
        uint8_t i = (uint8_t)__builtin_ctzll(mask);
        mask &= mask - 1;
        LoadedScript *s = &scriptRegistry[i];
        if (!s->active || !s->pendingForeign || s->pendingTrid != trid) continue;
        s->pendingForeign = false;
        if (s->pendingRead) {
            uint16_t pb = PayloadBytes(frame);
            if (pb < 8) {
                s->errorCode = SCRIPT_ERR_REGISTER;
                s->state = (uint8_t)ScriptState::Error;
                return;
            }
            BlockMeta rm;
            memcpy(&rm, frame.payload + 4, 4);
            const uint8_t *val = frame.payload + 8;
            uint8_t avail = (uint8_t)(pb - 8);
            uint16_t vsz = rm.Size;
            if (vsz > avail) vsz = avail;
            uint16_t dtype = 0;
            uint8_t *dest = nullptr;
            uint8_t dsize = 0;
            if (ScriptResolveDest(s, s->pendingDest, dtype, dest, dsize))
                ScriptAssignResolved(dtype, dest, dsize, val, (uint8_t)vsz, BlockMetaType(rm.FlagsAndType));
        }
        // Only a script still waiting on this confirmation resumes.
        if (s->state == (uint8_t)ScriptState::Waiting)
            s->state = (uint8_t)ScriptState::Running;
        return;
    }
}

static void ScriptSetState(LoadedScript *s, uint8_t newState) {
    if (newState == (uint8_t)ScriptState::Running) {
        if (s->state == (uint8_t)ScriptState::Stopped || s->state == (uint8_t)ScriptState::Finished ||
            s->state == (uint8_t)ScriptState::Error) {
            s->ic = 0;
            s->callDepth = 0;
        }
        s->waitingOnTime = false;
        s->errorCode = SCRIPT_ERR_NONE;
    } else if (newState == (uint8_t)ScriptState::Stopped) {
        s->ic = 0;
        s->callDepth = 0;
        s->waitingOnTime = false;
        s->errorCode = SCRIPT_ERR_NONE;
    }
    // A manual state change abandons any outstanding foreign confirmation.
    if (newState != (uint8_t)ScriptState::Waiting) s->pendingForeign = false;
    s->state = newState;
}

// Boot: load every stored script flagged Load-on-boot; run those flagged Run-on-load.
void ScriptsBootLoad() {
    for (uint8_t i = 0; i < MAX_SCRIPTS; i++) {
        char name[8];
        ScriptFileName(i, name);
        if (Storage.FileExists(name) == 0xFFFFFFFF) continue;
        if (!ScriptLoad(i)) continue;
        if (!(scriptRegistry[i].properties & SCRIPT_PROP_LOAD_ON_BOOT)) {
            scriptRegistry[i].Release(); // stored, but not pre-loaded
            continue;
        }
        if (scriptRegistry[i].properties & SCRIPT_PROP_RUN_ON_LOAD)
            scriptRegistry[i].state = (uint8_t)ScriptState::Running;
    }
}

// ===== Management commands (0x050X) =====

static void ScriptReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len) {
    SendResponse(frame, payload, len);
}

__attribute__((noinline)) static void HandleScript(const PacketFrame &frame) {
    if (frame.flags & FLAG_TYPE) return; // responses are not handled locally

    uint8_t cid = GetServiceCID(frame.srv_tgt);
    uint16_t bytes = PayloadBytes(frame);

    switch (cid) {
        case 0: { // Get currently loaded scripts
            uint8_t buf[1 + MAX_SCRIPTS];
            uint8_t n = ScriptListInstances(buf + 1, MAX_SCRIPTS);
            buf[0] = n;
            ScriptReply(frame, buf, 1 + n);
            break;
        }

        case 1: { // Load Script (File ID -> loaded ID)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            uint8_t fileId = frame.payload[0];
            bool ok = ScriptLoad(fileId);
            uint8_t reply = ok ? fileId : 0xFF;
            ScriptReply(frame, &reply, 1);
            break;
        }

        case 2: { // Unload script (loaded ID)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            uint8_t slot = frame.payload[0];
            bool ok = ScriptActive(slot) != nullptr;
            if (ok) ScriptUnload(slot);
            RespondStatus(frame, ok);
            break;
        }

        case 3: { // Read state (loaded ID -> state)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            ScriptReply(frame, &s->state, 1);
            break;
        }

        case 4: { // Set state (loaded ID, new state)
            if (bytes < 2) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s || frame.payload[1] > (uint8_t)ScriptState::Error) { RespondStatus(frame, false); return; }
            ScriptSetState(s, frame.payload[1]);
            RespondStatus(frame, true);
            break;
        }

        case 5: { // Read internal state (loaded ID -> instruction counter (line) + variable RAM)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            uint8_t buf[MAX_PAYLOAD_SIZE];
            uint32_t ic = s->ic;
            memcpy(buf, &ic, 4);
            uint16_t n = s->varTotal;
            if (n > MAX_PAYLOAD_SIZE - 4) n = MAX_PAYLOAD_SIZE - 4;
            if (n) memcpy(buf + 4, s->varSpace + 4, n);
            ScriptReply(frame, buf, 4 + n);
            break;
        }

        case 6: { // Move to instruction (loaded ID, line index u32)
            if (bytes < 5) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            uint32_t line = 0;
            memcpy(&line, frame.payload + 1, 4);
            if (s->lineCount && line >= s->lineCount) line = s->lineCount - 1;
            s->ic = (uint16_t)line;
            RespondStatus(frame, true);
            break;
        }

        case 7: { // Write Variable (loaded ID, variable ID, value)
            if (bytes < 2) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            uint8_t varId = frame.payload[1];
            if (!s || varId >= s->varCount) { RespondStatus(frame, false); return; }
            // The wire payload is 4-byte padded, so the value length comes from the
            // variable's declared size, not from the remaining payload bytes.
            uint16_t vlen = s->varMeta[varId].Size;
            if ((uint16_t)(2 + vlen) > bytes) { RespondStatus(frame, false); return; }
            RespondStatus(frame, ScriptSetVariable(frame.payload[0], varId, frame.payload + 2, vlen));
            break;
        }

        case 8: { // Read error (loaded ID -> error code)
            if (bytes < 1) { RespondStatus(frame, false); return; }
            LoadedScript *s = ScriptActive(frame.payload[0]);
            if (!s) { RespondStatus(frame, false); return; }
            ScriptReply(frame, &s->errorCode, 1);
            break;
        }

        default:
            RespondStatus(frame, false);
            break;
    }
}

#endif // USE_SCRIPTS
